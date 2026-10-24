// The inner random stream (P2-T6).
//
// KeePass protects the individual values of a database -- the password of an entry --
// with a stream cipher keyed by the database itself. The value stays encrypted in the
// XML and is turned back into plaintext only when a session actually needs it.
//
// The stream id comes from the database: the header fields 8 and 10 in KDBX 3.1, the
// inner header in KDBX 4. The shapes were measured against a real KDBX 3.1 database;
// see docs/evidence/kdbx-format-notes.md.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_PROTECTED_STREAM_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_PROTECTED_STREAM_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::credential::kdbx {

class KdbxProtectedStream {
 public:
    KdbxProtectedStream() = default;

    // The stream ids of the format:
    //   0 = no protection (the value is plain text)
    //   1 = ARCFOUR variant (an old KeePass format; not supported here)
    //   2 = Salsa20
    //   3 = ChaCha20
    KdbxProtectedStream(std::uint32_t id, const std::uint8_t* key, std::size_t key_size);

    [[nodiscard]] bool isProtected() const noexcept { return id_ != 0; }
    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

    // Decrypts into the caller's buffer. The caller owns the plaintext and wipes it.
    [[nodiscard]] bool decrypt(const std::uint8_t* data, std::size_t size,
                               std::vector<std::uint8_t>& out, std::string& error) const;

 private:
    std::uint32_t id_{0};
    // The stream key as it came out of the database; the cipher key is derived from it.
    std::array<std::uint8_t, 64> key_{};
    std::size_t key_size_{0};
};

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_PROTECTED_STREAM_H_
