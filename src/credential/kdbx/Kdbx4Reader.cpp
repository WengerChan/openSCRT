#include "credential/kdbx/Kdbx4Reader.h"

#include <sodium.h>

#include <cstring>

#include "credential/kdbx/KdbxCrypto.h"
#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

// The inner key: SHA-512( master seed || transformed key || 0x01 ).
//
// Both the header and the blocks are authenticated with a key derived from it, but
// they do it differently, and the difference is not cosmetic: the header's key is
// SHA-512( 0xFF * 8 || inner ) while **every block has its own key**,
// SHA-512( block index || inner ). Using the header's key for the blocks produces a
// block stream that fails its first integrity check.
Sha512Digest innerKey(const std::array<std::uint8_t, 32>& master_seed,
                      const std::array<std::uint8_t, 32>& transformed_key) {
    std::vector<std::uint8_t> inner;
    inner.reserve(master_seed.size() + transformed_key.size() + 1);
    inner.insert(inner.end(), master_seed.begin(), master_seed.end());
    inner.insert(inner.end(), transformed_key.begin(), transformed_key.end());
    inner.push_back(0x01);
    return sha512(inner.data(), inner.size());
}

// The key that authenticates the header.
Sha512Digest headerHmacKey(const Sha512Digest& inner) {
    std::vector<std::uint8_t> outer;
    outer.reserve(8 + inner.size());
    for (int i = 0; i < 8; ++i) {
        outer.push_back(0xFF);
    }
    outer.insert(outer.end(), inner.begin(), inner.end());
    return sha512(outer.data(), outer.size());
}

// The key of one block.
Sha512Digest blockHmacKey(std::uint64_t index, const Sha512Digest& inner) {
    std::vector<std::uint8_t> material;
    material.reserve(8 + inner.size());
    appendU64(material, index);
    material.insert(material.end(), inner.begin(), inner.end());
    return sha512(material.data(), material.size());
}

[[nodiscard]] std::uint32_t readU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

// Reads the HMAC-protected block stream that follows the header.
//
// Every block is HMAC(32) || length(UInt32) || data, and the stream ends with a block
// of length zero. Each block's HMAC covers the block index, the length and the data.
bool readBlockStream(const std::uint8_t* data, std::size_t size, std::size_t offset,
                     const Sha512Digest& inner_key, std::vector<std::uint8_t>& out,
                     std::string& error) {
    out.clear();
    std::uint64_t block_index = 0;
    while (true) {
        if (offset + 36 > size) {
            error = "the block stream ends in the middle of a block";
            return false;
        }
        const std::uint8_t* stored_hmac = data + offset;
        const std::uint32_t length = readU32(data + offset + 32);
        offset += 36;
        if (length > size - offset) {
            error = "a block claims more data than the file holds";
            return false;
        }

        std::vector<std::uint8_t> authenticated;
        authenticated.reserve(12 + length);
        appendU64(authenticated, block_index);
        appendU32(authenticated, length);
        authenticated.insert(authenticated.end(), data + offset, data + offset + length);

        const Sha512Digest hmac_key = blockHmacKey(block_index, inner_key);
        const Digest computed = hmacSha256(hmac_key.data(), hmac_key.size(), authenticated.data(),
                                           authenticated.size());
        if (std::memcmp(computed.data(), stored_hmac, computed.size()) != 0) {
            error =
                "a block failed its integrity check (wrong password, wrong key file, "
                "or a damaged database)";
            return false;
        }

        if (length == 0) {
            return true;
        }
        out.insert(out.end(), data + offset, data + offset + length);
        offset += length;
        ++block_index;
    }
}

// Reads the KDBX 4 inner header: the header that sits in front of the XML and says
// which protected stream protects the individual values.
//
// The fields are type(1) || length(UInt32 LE) || value, and the type 0 ends the list.
bool readInnerHeader(const std::uint8_t* data, std::size_t size, std::uint32_t& stream_id,
                     std::vector<std::uint8_t>& stream_key, std::size_t& xml_offset,
                     std::string& error) {
    stream_id = 0;
    stream_key.clear();
    std::size_t offset = 0;
    while (true) {
        if (offset + 5 > size) {
            error = "the inner header ends in the middle of a field";
            return false;
        }
        const std::uint8_t type = data[offset];
        const std::uint32_t length = static_cast<std::uint32_t>(data[offset + 1]) |
                                     (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
                                     (static_cast<std::uint32_t>(data[offset + 3]) << 16) |
                                     (static_cast<std::uint32_t>(data[offset + 4]) << 24);
        offset += 5;
        if (length > size - offset) {
            error = "an inner header field runs past the end of the payload";
            return false;
        }
        if (type == 0) {
            xml_offset = offset;
            return true;
        }
        if (type == 1) {
            if (length != 4) {
                error = "the protected stream id is not four bytes long";
                return false;
            }
            stream_id = static_cast<std::uint32_t>(data[offset]) |
                        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
                        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        } else if (type == 2) {
            stream_key.assign(data + offset, data + offset + length);
        }
        offset += length;
    }
}

}  // namespace

Kdbx4ReadResult readKdbx4(const std::uint8_t* data, std::size_t size, const std::uint8_t* password,
                          std::size_t password_size, const std::vector<std::uint8_t>& key_file) {
    Kdbx4ReadResult result;

    const KdbxHeaderParseResult header_result = parseKdbxHeader(data, size);
    if (!header_result.ok()) {
        result.error = header_result.error;
        return result;
    }
    const KdbxHeader& header = header_result.header;
    if (header.version == KdbxVersion::kV3_1) {
        result.error = "this is a KDBX 3.1 database, not a 4.x one";
        return result;
    }

    Digest composite = compositeKey(password, password_size, key_file);

    Digest transformed{};
    if (header.kdf.kdf == KdbxKdf::kArgon2d || header.kdf.kdf == KdbxKdf::kArgon2id) {
        if (!argon2Derive(header.kdf, composite.data(), composite.size(), transformed,
                          result.error)) {
            return result;
        }
    } else if (header.kdf.kdf == KdbxKdf::kAesKdf) {
        transformed = aesKdf(header.kdf.aes_salt.data(), header.kdf.aes_salt.size(),
                             header.kdf.aes_rounds, composite.data(), composite.size());
    } else {
        result.error = "the database uses a key derivation function this build does not know";
        return result;
    }

    // The master key: the seed and the transformed key, hashed together.
    std::vector<std::uint8_t> master_input;
    master_input.reserve(header.master_seed.size() + transformed.size());
    master_input.insert(master_input.end(), header.master_seed.begin(), header.master_seed.end());
    master_input.insert(master_input.end(), transformed.begin(), transformed.end());
    const Digest master_key = sha256(master_input.data(), master_input.size());

    const Sha512Digest inner_key = innerKey(header.master_seed, transformed);
    const Sha512Digest header_key = headerHmacKey(inner_key);

    // The header hash and the header HMAC follow the header itself.
    const std::size_t header_end = header.payload_offset;
    if (header_end + 64 > size) {
        result.error = "the file ends before the header hash";
        return result;
    }
    const Digest header_hash = sha256(header.raw_bytes.data(), header.raw_bytes.size());
    if (std::memcmp(header_hash.data(), data + header_end, header_hash.size()) != 0) {
        result.error = "the header hash does not match (the file is damaged)";
        return result;
    }
    const Digest header_hmac = hmacSha256(header_key.data(), header_key.size(),
                                          header.raw_bytes.data(), header.raw_bytes.size());
    if (std::memcmp(header_hmac.data(), data + header_end + 32, header_hmac.size()) != 0) {
        result.error = "the header authentication failed (wrong password or wrong key file)";
        return result;
    }

    std::vector<std::uint8_t> encrypted;
    if (!readBlockStream(data, size, header_end + 64, inner_key, encrypted, result.error)) {
        return result;
    }

    std::vector<std::uint8_t> plain;
    if (header.cipher == KdbxCipher::kAes256Cbc) {
        if (header.encryption_iv.size() != 16) {
            result.error = "the AES initialisation vector is not 16 bytes long";
            return result;
        }
        if (!aesCbcDecrypt(master_key.data(), header.encryption_iv.data(), encrypted.data(),
                           encrypted.size(), plain, result.error)) {
            return result;
        }
    } else if (header.cipher == KdbxCipher::kChaCha20) {
        if (header.encryption_iv.size() != 12) {
            result.error = "the ChaCha20 nonce is not 12 bytes long";
            return result;
        }
        if (!chacha20Decrypt(master_key.data(), header.encryption_iv.data(), encrypted.data(),
                             encrypted.size(), plain, result.error)) {
            return result;
        }
    } else {
        result.error = "the database uses a cipher this build does not implement";
        return result;
    }

    if (header.compression == KdbxCompression::kGzip) {
        std::vector<std::uint8_t> decompressed;
        if (!gzipDecompress(plain.data(), plain.size(), decompressed, result.error)) {
            return result;
        }
        result.payload = std::move(decompressed);
    } else {
        result.payload = std::move(plain);
    }

    // The inner header says which protected stream protects the values, and where the
    // XML starts.
    if (!readInnerHeader(result.payload.data(), result.payload.size(), result.protected_stream_id,
                         result.protected_stream_key, result.xml_offset, result.error)) {
        result.payload.clear();
        return result;
    }

    sodium_memzero(composite.data(), composite.size());
    return result;
}

}  // namespace openscrt::credential::kdbx
