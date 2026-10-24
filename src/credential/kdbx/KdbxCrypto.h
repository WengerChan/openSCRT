// The cryptography the KDBX readers need (P2-T4).
//
// Everything here is a thin, checked wrapper over OpenSSL and libsodium. The wrappers
// exist for two reasons: the KDBX code should not be full of EVP boilerplate, and every
// function reports failure instead of returning a half-finished result -- a KDBX reader
// that "succeeds" with garbage produces a database of nonsense, which is worse than a
// clear error.
//
// The algorithms and their exact parameters are written down with their sources in
// docs/evidence/kdbx-format-notes.md.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_CRYPTO_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_CRYPTO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {

using Digest = std::array<std::uint8_t, 32>;
using Sha512Digest = std::array<std::uint8_t, 64>;

[[nodiscard]] Digest sha256(const std::uint8_t* data, std::size_t size);
[[nodiscard]] Sha512Digest sha512(const std::uint8_t* data, std::size_t size);
[[nodiscard]] Digest hmacSha256(const std::uint8_t* key, std::size_t key_size,
                                const std::uint8_t* data, std::size_t size);

// The composite key: SHA-256( SHA-256(password) || key file ).
//
// The outer hash is the part that is easy to miss: without it the derived key is wrong
// and the symptom is "the payload decrypts to nonsense" rather than an error.
[[nodiscard]] Digest compositeKey(const std::string& password,
                                  const std::vector<std::uint8_t>& key_file);

// The AES-KDF, the way KeePass does it.
//
// Each half of the composite key is transformed on its own: AES-256-CBC with the seed
// as the key and the half itself as the IV, over blocks of zeroes, `rounds` times. The
// two halves are then concatenated and hashed.
//
// Note that this is *not* the same as encrypting the composite key in ECB mode, which
// is what at least one other implementation does. The two disagree, and the KeePass
// behaviour is the one that matches the files KeePass writes.
[[nodiscard]] Digest aesKdf(const std::uint8_t* seed, std::size_t seed_size, std::uint64_t rounds,
                            const std::uint8_t* composite, std::size_t composite_size);

// Derives the transformed key with Argon2d or Argon2id.
//
// libsodium's memory limit is in bytes, which is the same unit the KDBX file uses, so
// the value is passed through unchanged. Argon2 1.0 (the "V" parameter 0x10) is not
// supported by the library and is reported as an error rather than approximated.
[[nodiscard]] bool argon2Derive(const KdbxKdfParameters& parameters, const std::uint8_t* composite,
                                std::size_t composite_size, Digest& out, std::string& error);

// AES-256-CBC with PKCS#7 unpadding.
[[nodiscard]] bool aesCbcDecrypt(const std::uint8_t* key, const std::uint8_t* iv,
                                 const std::uint8_t* data, std::size_t size,
                                 std::vector<std::uint8_t>& out, std::string& error);

// ChaCha20 in the IETF variant (a 12-byte nonce and a 32-bit block counter), which is
// what KDBX 4 uses.
[[nodiscard]] bool chacha20Decrypt(const std::uint8_t* key, const std::uint8_t* nonce,
                                   const std::uint8_t* data, std::size_t size,
                                   std::vector<std::uint8_t>& out, std::string& error);

// GZip, as used by the KDBX compression flag.
[[nodiscard]] bool gzipDecompress(const std::uint8_t* data, std::size_t size,
                                  std::vector<std::uint8_t>& out, std::string& error);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_CRYPTO_H_
