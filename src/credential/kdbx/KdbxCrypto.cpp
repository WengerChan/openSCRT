#include "credential/kdbx/KdbxCrypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <sodium.h>
#include <zlib.h>

#include <cstring>

namespace openscrt::credential::kdbx {
namespace {

// The AES-256 encryption of one block, with no padding.
//
// The AES-KDF needs the raw block cipher, not a padded mode: it encrypts blocks of
// zeroes and XORs the result into the running value.
[[nodiscard]] bool aes256EncryptBlock(const std::uint8_t* key, const std::uint8_t* in,
                                      std::uint8_t* out) {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return false;
    }
    bool ok = false;
    int length = 0;
    if (EVP_EncryptInit_ex(context, EVP_aes_256_ecb(), nullptr, key, nullptr) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
        EVP_EncryptUpdate(context, out, &length, in, 16) == 1) {
        ok = length == 16;
    }
    EVP_CIPHER_CTX_free(context);
    return ok;
}

// The KeePass AES-KDF transform of one 16-byte half.
//
// KeePass encrypts blocks of zeroes with AES-256-CBC, using the seed as the key and
// the half itself as the IV, and takes the last ciphertext block as the new value.
// For a zero plaintext that is exactly "XOR the encrypted zero block into the running
// value", which is what this loop does.
[[nodiscard]] bool aesKdfTransformHalf(const std::uint8_t* seed, std::uint8_t* half,
                                       std::uint64_t rounds) {
    const std::uint8_t zero_block[16] = {};
    std::uint8_t encrypted[16] = {};
    if (!aes256EncryptBlock(seed, zero_block, encrypted)) {
        return false;
    }
    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (std::size_t i = 0; i < 16; ++i) {
            half[i] = static_cast<std::uint8_t>(half[i] ^ encrypted[i]);
        }
    }
    return true;
}

}  // namespace

Digest sha256(const std::uint8_t* data, std::size_t size) {
    Digest digest{};
    unsigned int length = 0;
    if (EVP_Digest(data, size, digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != digest.size()) {
        return Digest{};
    }
    return digest;
}

Sha512Digest sha512(const std::uint8_t* data, std::size_t size) {
    Sha512Digest digest{};
    unsigned int length = 0;
    if (EVP_Digest(data, size, digest.data(), &length, EVP_sha512(), nullptr) != 1 ||
        length != digest.size()) {
        return Sha512Digest{};
    }
    return digest;
}

Digest hmacSha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                  std::size_t size) {
    Digest digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), key, static_cast<int>(key_size), data, size, digest.data(), &length) ==
            nullptr ||
        length != digest.size()) {
        return Digest{};
    }
    return digest;
}

Digest compositeKey(const std::string& password, const std::vector<std::uint8_t>& key_file) {
    // The password half is hashed on its own, and the key file half is the file's own
    // 32 bytes; the two are then hashed together. The outer hash is what the format
    // specifies -- leaving it out gives a wrong key and a payload of nonsense rather
    // than an error.
    Digest password_hash{};
    unsigned int length = 0;
    const std::uint8_t* password_bytes = reinterpret_cast<const std::uint8_t*>(password.data());
    if (EVP_Digest(password_bytes, password.size(), password_hash.data(), &length, EVP_sha256(),
                   nullptr) != 1 ||
        length != password_hash.size()) {
        return Digest{};
    }

    std::vector<std::uint8_t> composite;
    composite.reserve(password_hash.size() + key_file.size());
    composite.insert(composite.end(), password_hash.begin(), password_hash.end());
    composite.insert(composite.end(), key_file.begin(), key_file.end());

    return sha256(composite.data(), composite.size());
}

Digest aesKdf(const std::uint8_t* seed, std::size_t seed_size, std::uint64_t rounds,
              const std::uint8_t* composite, std::size_t composite_size) {
    Digest transformed{};
    // AES-256 needs a 32-byte key, and the composite key is two blocks wide.
    if (composite_size != transformed.size() || seed_size < transformed.size()) {
        return transformed;
    }

    std::uint8_t left[16] = {};
    std::uint8_t right[16] = {};
    std::memcpy(left, composite, 16);
    std::memcpy(right, composite + 16, 16);

    if (!aesKdfTransformHalf(seed, left, rounds) || !aesKdfTransformHalf(seed, right, rounds)) {
        return Digest{};
    }

    std::uint8_t halves[32] = {};
    std::memcpy(halves, left, 16);
    std::memcpy(halves + 16, right, 16);
    const Digest result = sha256(halves, sizeof(halves));
    sodium_memzero(halves, sizeof(halves));
    return result;
}

bool argon2Derive(const KdbxKdfParameters& parameters, const std::uint8_t* composite,
                  std::size_t composite_size, Digest& out, std::string& error) {
    // libsodium has Argon2i and Argon2id but **no Argon2d**, and KeePass writes
    // Argon2d by default -- so the derivation goes through OpenSSL, whose ARGON2 KDF
    // has all three variants.
    const char* algorithm = nullptr;
    if (parameters.kdf == KdbxKdf::kArgon2d) {
        algorithm = "ARGON2D";
    } else if (parameters.kdf == KdbxKdf::kArgon2id) {
        algorithm = "ARGON2ID";
    } else {
        error = "the key derivation function is not Argon2";
        return false;
    }
    if (parameters.argon2_salt.size() < 8) {
        error = "the database has no usable Argon2 salt";
        return false;
    }
    if (parameters.argon2_memory_bytes < 8192) {
        error = "the database asks for less memory than Argon2 allows";
        return false;
    }

    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, algorithm, nullptr);
    if (kdf == nullptr) {
        error = "this build of OpenSSL has no Argon2 key derivation";
        return false;
    }
    EVP_KDF_CTX* context = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (context == nullptr) {
        error = "the Argon2 context could not be created";
        return false;
    }

    // The memory cost of Argon2 is a number of 1 KiB blocks, while the KDBX file
    // stores the same thing in bytes.
    std::uint32_t iterations = static_cast<std::uint32_t>(parameters.argon2_iterations);
    std::uint32_t lanes = parameters.argon2_parallelism;
    std::uint32_t memory_kib = static_cast<std::uint32_t>(parameters.argon2_memory_bytes / 1024);
    std::uint32_t version = parameters.argon2_version;
    std::uint32_t size = static_cast<std::uint32_t>(out.size());

    // The parameter names are the strings OpenSSL documents; the argon2-specific ones
    // are spelled out because their macros are not in every OpenSSL header.
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                          const_cast<std::uint8_t*>(composite), composite_size),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                          const_cast<std::uint8_t*>(parameters.argon2_salt.data()),
                                          parameters.argon2_salt.size()),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_construct_uint("memcost", &memory_kib),
        OSSL_PARAM_construct_uint("lanes", &lanes),
        OSSL_PARAM_construct_uint("version", &version),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_SIZE, &size),
        OSSL_PARAM_construct_end(),
    };

    const int derived = EVP_KDF_derive(context, out.data(), out.size(), params);
    EVP_KDF_CTX_free(context);
    if (derived != 1) {
        error =
            "the Argon2 parameters were rejected (they may be outside the range the "
            "library accepts)";
        return false;
    }
    return true;
}

bool aesCbcDecrypt(const std::uint8_t* key, const std::uint8_t* iv, const std::uint8_t* data,
                   std::size_t size, std::vector<std::uint8_t>& out, std::string& error) {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        error = "the cipher context could not be created";
        return false;
    }

    out.clear();
    out.resize(size + EVP_MAX_BLOCK_LENGTH);
    int length = 0;
    int total = 0;
    bool ok = false;
    if (EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key, iv) == 1) {
        if (EVP_DecryptUpdate(context, out.data(), &length, data, static_cast<int>(size)) == 1) {
            total = length;
            int final_length = 0;
            if (EVP_DecryptFinal_ex(context, out.data() + total, &final_length) == 1) {
                total += final_length;
                out.resize(static_cast<std::size_t>(total));
                ok = true;
            }
        }
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        out.clear();
        error =
            "the payload did not decrypt (wrong password, wrong key file, or a "
            "damaged database)";
    }
    return ok;
}

bool chacha20Decrypt(const std::uint8_t* key, const std::uint8_t* nonce, const std::uint8_t* data,
                     std::size_t size, std::vector<std::uint8_t>& out, std::string& error) {
    if (sodium_init() < 0) {
        error = "libsodium could not be initialised";
        return false;
    }
    out.assign(size, 0);
    // Decryption of a stream cipher is the ciphertext XORed with the keystream, which
    // is what the _xor variant does. The arguments are (output, input, length, nonce,
    // key): the nonce comes third and the key fourth.
    crypto_stream_chacha20_ietf_xor(out.data(), data, static_cast<unsigned long long>(size), nonce,
                                    key);
    return true;
}

bool gzipDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                    std::string& error) {
    z_stream stream{};
    // 16 + MAX_WBITS selects GZip rather than the zlib wrapper.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        error = "the decompressor could not be initialised";
        return false;
    }

    stream.next_in = const_cast<Bytef*>(data);
    stream.avail_in = static_cast<uInt>(size);

    std::vector<std::uint8_t> buffer(64 * 1024);
    out.clear();
    while (true) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            out.clear();
            error = "the payload is not valid GZip data";
            return false;
        }
        out.insert(out.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(buffer.size() - stream.avail_out));
        if (status == Z_STREAM_END) {
            break;
        }
        if (stream.avail_in == 0 && stream.avail_out != 0) {
            // No more input and the output buffer is not full: the stream was cut short.
            inflateEnd(&stream);
            out.clear();
            error = "the compressed payload ended before the stream did";
            return false;
        }
    }

    inflateEnd(&stream);
    return true;
}

}  // namespace openscrt::credential::kdbx
