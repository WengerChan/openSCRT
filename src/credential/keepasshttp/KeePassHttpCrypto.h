// The crypto of the KeePassHTTP protocol (P2-T8).
//
// The protocol encrypts its fields with AES-256-CBC and PKCS#7 padding and carries
// everything base64. These helpers are in their own file because the mock server in
// the tests has to do the same thing from the other side: it decrypts what the client
// sent and encrypts what it answers.

#ifndef OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CRYPTO_H_
#define OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::credential::keepasshttp {

inline constexpr std::size_t kKeyBytes = 32;    // AES-256
inline constexpr std::size_t kNonceBytes = 16;  // one AES block

// Base64 of a byte range.
[[nodiscard]] std::string base64(const std::uint8_t* data, std::size_t size);
[[nodiscard]] bool decodeBase64(const std::string& text, std::vector<std::uint8_t>& out);

// AES-256-CBC with PKCS#7 padding. The key and the nonce are base64.
[[nodiscard]] bool encryptField(const std::string& key_base64, const std::string& nonce_base64,
                                const std::string& plaintext, std::string& out, std::string& error);
[[nodiscard]] bool decryptField(const std::string& key_base64, const std::string& nonce_base64,
                                const std::string& ciphertext_base64, std::string& out,
                                std::string& error);

}  // namespace openscrt::credential::keepasshttp

#endif  // OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CRYPTO_H_
