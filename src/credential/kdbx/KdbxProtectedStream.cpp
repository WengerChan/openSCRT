#include "credential/kdbx/KdbxProtectedStream.h"

#include <sodium.h>

#include <cstring>

#include "credential/kdbx/KdbxCrypto.h"

namespace openscrt::credential::kdbx {
namespace {

// The nonce of the Salsa20 inner stream: a constant, and the format says so.
constexpr std::uint8_t kSalsa20Nonce[8] = {0xE8, 0x30, 0x09, 0x4B, 0x97, 0x20, 0x5D, 0x2A};

}  // namespace

KdbxProtectedStream::KdbxProtectedStream(std::uint32_t id, const std::uint8_t* key,
                                         std::size_t key_size)
    : id_(id), key_size_(key_size) {
    if (key != nullptr && key_size > 0 && key_size <= key_.size()) {
        std::memcpy(key_.data(), key, key_size);
    } else {
        key_size_ = 0;
    }
}

bool KdbxProtectedStream::decrypt(const std::uint8_t* data, std::size_t size,
                                  std::vector<std::uint8_t>& out, std::string& error) const {
    out.clear();
    if (data == nullptr && size != 0) {
        error = "no data to decrypt";
        return false;
    }
    if (id_ != 0 && key_size_ == 0) {
        // A protected stream without a key cannot be decrypted: the cipher would run
        // with a key that was never there and hand back garbage that looks like a
        // password. Say so instead.
        error = "the protected stream has no key";
        return false;
    }

    switch (id_) {
        case 0: {
            // No protection: the value is plain text.
            out.assign(data, data + size);
            return true;
        }
        case 2: {
            // Salsa20: the key is SHA-256 of the stream key.
            if (sodium_init() < 0) {
                error = "libsodium could not be initialised";
                return false;
            }
            const Digest key = sha256(key_.data(), key_size_);
            out.assign(size, 0);
            // The _xor variant, not the plain one: the stored value is the plaintext
            // XORed with the keystream, and the plain variant would only produce the
            // keystream itself.
            crypto_stream_salsa20_xor(out.data(), data, static_cast<unsigned long long>(size),
                                      kSalsa20Nonce, key.data());
            return true;
        }
        case 3: {
            // ChaCha20: the key and the nonce come out of one SHA-512 of the stream key.
            if (sodium_init() < 0) {
                error = "libsodium could not be initialised";
                return false;
            }
            const Sha512Digest digest = sha512(key_.data(), key_size_);
            out.assign(size, 0);
            crypto_stream_chacha20_ietf_xor(out.data(), data, static_cast<unsigned long long>(size),
                                            digest.data() + 32, digest.data());
            return true;
        }
        case 1:
            error =
                "the database uses the ARCFOUR protected stream, which this build does "
                "not implement";
            return false;
        default:
            error = "the database uses a protected stream this build does not know";
            return false;
    }
}

}  // namespace openscrt::credential::kdbx
