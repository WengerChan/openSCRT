#include "credential/kdbx/Kdbx3Reader.h"

#include <sodium.h>

#include <cstring>

#include "credential/kdbx/KdbxCrypto.h"
#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

// Reads the KDBX 3.1 block stream: index(UInt32) || SHA-256(block)(32) ||
// length(UInt32) || data, and the stream ends with a block of length zero.
//
// Unlike KDBX 4, the blocks carry a plain hash of their own data rather than an HMAC,
// and the index has to come out right.
bool readBlockStream(const std::uint8_t* data, std::size_t size, std::size_t offset,
                     std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    std::uint32_t expected_index = 0;
    while (true) {
        if (offset + 40 > size) {
            error = "the block stream ends in the middle of a block";
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(data[offset]) |
                                    (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                                    (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
                                    (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        const std::uint8_t* stored_hash = data + offset + 4;
        const std::uint32_t length = static_cast<std::uint32_t>(data[offset + 36]) |
                                     (static_cast<std::uint32_t>(data[offset + 37]) << 8) |
                                     (static_cast<std::uint32_t>(data[offset + 38]) << 16) |
                                     (static_cast<std::uint32_t>(data[offset + 39]) << 24);
        offset += 40;
        if (length > size - offset) {
            error = "a block claims more data than the file holds";
            return false;
        }
        if (index != expected_index) {
            error = "the block stream is out of order";
            return false;
        }

        if (length == 0) {
            // The terminator carries a zero hash; there is nothing left to read.
            return true;
        }

        const Digest hash = sha256(data + offset, length);
        if (std::memcmp(hash.data(), stored_hash, hash.size()) != 0) {
            error =
                "a block failed its integrity check (wrong password, wrong key file, "
                "or a damaged database)";
            return false;
        }
        out.insert(out.end(), data + offset, data + offset + length);
        offset += length;
        ++expected_index;
    }
}

}  // namespace

Kdbx3ReadResult readKdbx3(const std::uint8_t* data, std::size_t size, const std::string& password,
                          const std::vector<std::uint8_t>& key_file) {
    Kdbx3ReadResult result;

    const KdbxHeaderParseResult header_result = parseKdbxHeader(data, size);
    if (!header_result.ok()) {
        result.error = header_result.error;
        return result;
    }
    const KdbxHeader& header = header_result.header;
    if (header.version != KdbxVersion::kV3_1) {
        result.error = "this is not a KDBX 3.1 database";
        return result;
    }
    if (header.kdf.kdf != KdbxKdf::kAesKdf) {
        result.error = "a KDBX 3.1 database has to use the AES key derivation function";
        return result;
    }
    if (header.encryption_iv.size() != 16) {
        result.error = "the AES initialisation vector is not 16 bytes long";
        return result;
    }

    Digest composite = compositeKey(password, key_file);
    const Digest transformed = aesKdf(header.kdf.aes_salt.data(), header.kdf.aes_salt.size(),
                                      header.kdf.aes_rounds, composite.data(), composite.size());

    std::vector<std::uint8_t> master_input;
    master_input.reserve(header.master_seed.size() + transformed.size());
    master_input.insert(master_input.end(), header.master_seed.begin(), header.master_seed.end());
    master_input.insert(master_input.end(), transformed.begin(), transformed.end());
    const Digest master_key = sha256(master_input.data(), master_input.size());

    std::vector<std::uint8_t> plain;
    if (!aesCbcDecrypt(master_key.data(), header.encryption_iv.data(), data + header.payload_offset,
                       size - header.payload_offset, plain, result.error)) {
        return result;
    }

    // The first 32 bytes of the decrypted stream are the stream start bytes: they are
    // the check that the key was right, and they sit outside the compressed data.
    if (plain.size() < header.stream_start_bytes.size()) {
        result.error = "the payload is too short to hold the stream start bytes";
        return result;
    }
    if (std::memcmp(plain.data(), header.stream_start_bytes.data(),
                    header.stream_start_bytes.size()) != 0) {
        result.error = "the stream start bytes do not match (wrong password or wrong key file)";
        return result;
    }

    std::vector<std::uint8_t> content;
    if (!readBlockStream(plain.data() + header.stream_start_bytes.size(),
                         plain.size() - header.stream_start_bytes.size(), 0, content,
                         result.error)) {
        return result;
    }

    if (header.compression == KdbxCompression::kGzip) {
        std::vector<std::uint8_t> decompressed;
        if (!gzipDecompress(content.data(), content.size(), decompressed, result.error)) {
            return result;
        }
        result.payload = std::move(decompressed);
    } else {
        result.payload = std::move(content);
    }

    // In KDBX 3.1 the protected stream is in the header, and the payload is the XML
    // itself -- there is no inner header.
    result.protected_stream_id = header.inner_random_stream_id;
    result.protected_stream_key.assign(header.inner_random_stream_key.begin(),
                                       header.inner_random_stream_key.end());
    result.xml_offset = 0;

    sodium_memzero(composite.data(), composite.size());
    return result;
}

}  // namespace openscrt::credential::kdbx
