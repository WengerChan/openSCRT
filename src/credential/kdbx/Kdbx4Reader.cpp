#include "credential/kdbx/Kdbx4Reader.h"

#include <sodium.h>

#include <cstring>

#include "credential/kdbx/KdbxCrypto.h"
#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

// The key that authenticates the header and every block.
//
// SHA-512( 0xFF * 8 || SHA-512( master seed || transformed key || 0x01 ) )
Sha512Digest blockHmacKey(const std::array<std::uint8_t, 32>& master_seed,
                          const std::array<std::uint8_t, 32>& transformed_key) {
    std::vector<std::uint8_t> inner;
    inner.reserve(master_seed.size() + transformed_key.size() + 1);
    inner.insert(inner.end(), master_seed.begin(), master_seed.end());
    inner.insert(inner.end(), transformed_key.begin(), transformed_key.end());
    inner.push_back(0x01);

    const Sha512Digest inner_hash = sha512(inner.data(), inner.size());

    std::vector<std::uint8_t> outer;
    outer.reserve(8 + inner_hash.size());
    for (int i = 0; i < 8; ++i) {
        outer.push_back(0xFF);
    }
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return sha512(outer.data(), outer.size());
}

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
                     const Sha512Digest& hmac_key, std::vector<std::uint8_t>& out,
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

}  // namespace

Kdbx4ReadResult readKdbx4(const std::uint8_t* data, std::size_t size, const std::string& password,
                          const std::vector<std::uint8_t>& key_file) {
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

    const Digest composite = compositeKey(password, key_file);

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

    const Sha512Digest hmac_key = blockHmacKey(header.master_seed, transformed);

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
    const Digest header_hmac = hmacSha256(hmac_key.data(), hmac_key.size(), header.raw_bytes.data(),
                                          header.raw_bytes.size());
    if (std::memcmp(header_hmac.data(), data + header_end + 32, header_hmac.size()) != 0) {
        result.error = "the header authentication failed (wrong password or wrong key file)";
        return result;
    }

    std::vector<std::uint8_t> encrypted;
    if (!readBlockStream(data, size, header_end + 64, hmac_key, encrypted, result.error)) {
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

    sodium_memzero(composite.data(), composite.size());
    return result;
}

}  // namespace openscrt::credential::kdbx
