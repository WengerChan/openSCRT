#include "protocols/ssh/SshComKey.h"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace openscrt::protocols::ssh {
namespace {

constexpr std::uint32_t kMagic = 0x3f6ff9eb;
constexpr char kBeginMarker[] = "---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----";
constexpr char kEndMarker[] = "---- END SSH2 ENCRYPTED PRIVATE KEY ----";

// The cipher name in the body, and the only two values the format uses.
constexpr char kCipherNone[] = "none";
constexpr char kCipher3des[] = "3des-cbc";

int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::vector<std::uint8_t> base64Decode(const std::string& text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);

    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char character : text) {
        const int value = base64Value(character);
        if (value < 0) {
            // Whitespace and the padding are the only other characters that may appear.
            continue;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    return out;
}

std::string base64Encode(const std::vector<std::uint8_t>& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t index = 0;
    while (index + 2 < data.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16) |
                                     (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                     static_cast<std::uint32_t>(data[index + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[triple & 0x3Fu]);
        index += 3;
    }
    if (index + 1 == data.size()) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (index + 2 == data.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16) |
                                     (static_cast<std::uint32_t>(data[index + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

// A cursor over a byte buffer, with the big-endian reads the format needs.
class Reader {
 public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

    [[nodiscard]] bool atEnd() const { return offset_ >= data_.size(); }
    [[nodiscard]] std::size_t remaining() const { return data_.size() - offset_; }

    [[nodiscard]] bool readUint32(std::uint32_t& value) {
        if (remaining() < 4) {
            return false;
        }
        value = (static_cast<std::uint32_t>(data_[offset_]) << 24) |
                (static_cast<std::uint32_t>(data_[offset_ + 1]) << 16) |
                (static_cast<std::uint32_t>(data_[offset_ + 2]) << 8) |
                static_cast<std::uint32_t>(data_[offset_ + 3]);
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool readBytes(std::size_t count, std::vector<std::uint8_t>& out) {
        if (remaining() < count) {
            return false;
        }
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool readString(std::string& out) {
        std::uint32_t length = 0;
        if (!readUint32(length)) {
            return false;
        }
        if (remaining() < length) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
        offset_ += length;
        return true;
    }

    // A number whose length is given in bits, as the ssh.com blob stores them.
    [[nodiscard]] bool readBigInt(std::vector<std::uint8_t>& out) {
        std::uint32_t bits = 0;
        if (!readUint32(bits)) {
            return false;
        }
        const std::size_t bytes = (static_cast<std::size_t>(bits) + 7) / 8;
        return readBytes(bytes, out);
    }

 private:
    const std::vector<std::uint8_t>& data_;
    std::size_t offset_ = 0;
};

// The 3DES key the ssh.com format derives from the passphrase:
//
//   block A = MD5(passphrase)
//   block B = MD5(passphrase || A)
//   block C = MD5(passphrase || A || B) and so on
//   the key is the first 24 bytes of A || B || C
//
// The initialisation vector is eight zero bytes and there is no padding.
std::vector<std::uint8_t> derive3desKey(const std::string& passphrase) {
    std::vector<std::uint8_t> key;
    key.reserve(24);

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    const auto* password = reinterpret_cast<const unsigned char*>(passphrase.data());
    const std::size_t password_length = passphrase.size();

    EVP_Digest(password, password_length, digest.data(), &digest_length, EVP_md5(), nullptr);
    key.insert(key.end(), digest.begin(), digest.begin() + digest_length);

    while (key.size() < 24) {
        std::vector<unsigned char> input;
        input.reserve(password_length + digest_length);
        input.insert(input.end(), password, password + password_length);
        input.insert(input.end(), digest.begin(), digest.begin() + digest_length);
        EVP_Digest(input.data(), input.size(), digest.data(), &digest_length, EVP_md5(), nullptr);
        key.insert(key.end(), digest.begin(), digest.begin() + digest_length);
    }
    key.resize(24);
    return key;
}

bool decrypt3des(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& input,
                 std::vector<std::uint8_t>& output) {
    if (input.empty() || input.size() % 8 != 0) {
        return false;
    }
    std::array<unsigned char, 8> iv{};
    output.assign(input.size(), 0);

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return false;
    }
    const bool ok =
        EVP_DecryptInit_ex(context, EVP_des_ede3_cbc(), nullptr, key.data(), iv.data()) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1;
    int written = 0;
    bool result = false;
    if (ok) {
        result = EVP_DecryptUpdate(context, output.data(), &written, input.data(),
                                   static_cast<int>(input.size())) == 1;
        int final_written = 0;
        if (result) {
            result = EVP_DecryptFinal_ex(context, output.data() + written, &final_written) == 1;
            written += final_written;
        }
    }
    EVP_CIPHER_CTX_free(context);
    if (result) {
        output.resize(static_cast<std::size_t>(written));
    }
    return result;
}

// Pulls the base64 body out of the text envelope.
std::string readSshComBody(const std::string& path, bool& ok) {
    ok = false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line)) {
        return {};
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != kBeginMarker) {
        return {};
    }

    std::string body;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == kEndMarker) {
            ok = true;
            return body;
        }
        // Header lines ("Comment: ...") come before the body and carry a colon; the
        // base64 body never does.
        if (line.find(':') != std::string::npos) {
            continue;
        }
        body += line;
    }
    return {};
}

// Appends a length-prefixed string, the way the SSH wire format does.
void putString(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value) {
    const std::uint32_t length = static_cast<std::uint32_t>(value.size());
    out.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    out.insert(out.end(), value.begin(), value.end());
}

void putString(std::vector<std::uint8_t>& out, const std::string& value) {
    putString(out, std::vector<std::uint8_t>(value.begin(), value.end()));
}

void putUint32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

// The SSH wire format's mpint: a big-endian two's complement integer, so a leading zero
// byte marks a positive number whose top bit is set.
void putMpInt(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value) {
    std::size_t start = 0;
    while (start + 1 < value.size() && value[start] == 0) {
        ++start;
    }
    std::vector<std::uint8_t> trimmed(value.begin() + static_cast<std::ptrdiff_t>(start),
                                      value.end());
    if (trimmed.empty()) {
        putString(out, trimmed);
        return;
    }
    if ((trimmed.front() & 0x80u) != 0) {
        trimmed.insert(trimmed.begin(), 0);
    }
    putString(out, trimmed);
}

}  // namespace

bool isSshComKeyFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string line;
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line == kBeginMarker;
}

bool readSshComKeyFile(const std::string& path, const std::string& passphrase, SshComRsaKey& key,
                       std::string& error) {
    bool ok = false;
    const std::string body = readSshComBody(path, ok);
    if (!ok) {
        error = "the file is not an ssh.com private key";
        return false;
    }

    const std::vector<std::uint8_t> data = base64Decode(body);
    Reader reader(data);

    std::uint32_t magic = 0;
    std::uint32_t total_length = 0;
    std::string key_type;
    std::string cipher_name;
    std::uint32_t blob_length = 0;
    if (!reader.readUint32(magic) || magic != kMagic) {
        error = "the key does not start with the ssh.com magic number";
        return false;
    }
    if (!reader.readUint32(total_length) || !reader.readString(key_type) ||
        !reader.readString(cipher_name) || !reader.readUint32(blob_length)) {
        error = "the ssh.com key header is truncated";
        return false;
    }
    // The total length covers the whole body, magic number included. It is not needed
    // to read the key, and the blob length below is what the format is actually read by.
    (void)total_length;
    if (key_type.find("rsa") == std::string::npos) {
        error = "only RSA ssh.com keys are supported, this one is " + key_type;
        return false;
    }

    std::vector<std::uint8_t> blob;
    if (!reader.readBytes(blob_length, blob)) {
        error = "the ssh.com key body is truncated";
        return false;
    }

    if (cipher_name == kCipher3des) {
        if (passphrase.empty()) {
            error = "the ssh.com key is encrypted and needs a passphrase";
            return false;
        }
        const std::vector<std::uint8_t> cipher_key = derive3desKey(passphrase);
        std::vector<std::uint8_t> plain;
        if (!decrypt3des(cipher_key, blob, plain)) {
            error = "the ssh.com key could not be decrypted";
            return false;
        }
        blob = std::move(plain);
    } else if (cipher_name != kCipherNone) {
        error = "the ssh.com key uses the unknown cipher " + cipher_name;
        return false;
    }

    Reader blob_reader(blob);
    std::uint32_t plain_length = 0;
    if (!blob_reader.readUint32(plain_length)) {
        error = "the ssh.com key body is truncated";
        return false;
    }
    if (plain_length + 4 > blob.size()) {
        // The decrypted body says how long it is, and a wrong passphrase produces a length
        // that does not fit. This is the check that tells a wrong passphrase from a
        // damaged file.
        error = "wrong passphrase";
        return false;
    }
    if (!blob_reader.readBigInt(key.exponent) || !blob_reader.readBigInt(key.private_exponent) ||
        !blob_reader.readBigInt(key.modulus) || !blob_reader.readBigInt(key.inverse_q) ||
        !blob_reader.readBigInt(key.q) || !blob_reader.readBigInt(key.p)) {
        error = "the ssh.com key body is truncated";
        return false;
    }
    if (key.modulus.empty() || key.exponent.empty()) {
        error = "the ssh.com key carries no RSA key";
        return false;
    }
    return true;
}

std::string toOpenSshPrivateKey(const SshComRsaKey& key) {
    // The public key blob: the algorithm name, then e and n.
    std::vector<std::uint8_t> public_blob;
    putString(public_blob, std::string("ssh-rsa"));
    putMpInt(public_blob, key.exponent);
    putMpInt(public_blob, key.modulus);

    // The private section: a check word twice, the key fields, a comment, then padding.
    std::vector<std::uint8_t> private_section;
    putUint32(private_section, 0x01020304u);
    putUint32(private_section, 0x01020304u);
    putString(private_section, std::string("ssh-rsa"));
    putMpInt(private_section, key.modulus);
    putMpInt(private_section, key.exponent);
    putMpInt(private_section, key.private_exponent);
    putMpInt(private_section, key.inverse_q);
    putMpInt(private_section, key.p);
    putMpInt(private_section, key.q);
    putString(private_section, std::string("openscrt-sshcom-import"));
    for (unsigned char padding = 1; private_section.size() % 8 != 0; ++padding) {
        private_section.push_back(padding);
    }

    std::vector<std::uint8_t> container;
    const char* magic = "openssh-key-v1";
    container.insert(container.end(), magic, magic + std::strlen(magic) + 1);
    putString(container, std::string(kCipherNone));
    putString(container, std::string("none"));
    putString(container, std::string(""));
    putUint32(container, 1);
    putString(container, public_blob);
    putString(container, private_section);

    const std::string encoded = base64Encode(container);
    std::string out = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    for (std::size_t offset = 0; offset < encoded.size(); offset += 70) {
        out += encoded.substr(offset, 70);
        out.push_back('\n');
    }
    out += "-----END OPENSSH PRIVATE KEY-----\n";
    return out;
}

}  // namespace openscrt::protocols::ssh
