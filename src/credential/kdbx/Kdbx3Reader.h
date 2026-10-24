// The KDBX 3.1 reader (P2-T5).
//
// KDBX 3.1 is the format KeePass 2.x writes by default, and it differs from 4.x in
// three places that matter:
//
//   * the header field lengths are 16-bit, not 32-bit;
//   * the payload is one encrypted stream, not HMAC-protected blocks -- the key is
//     checked by comparing the first 32 decrypted bytes with the header's stream start
//     bytes;
//   * the key derivation is AES-KDF, and the payload framing is a block stream where
//     each block carries a plain SHA-256 of its own data.
//
// The byte-level facts are in docs/evidence/kdbx-format-notes.md; the AES-KDF shape
// was measured against a real KDBX 3.1 database rather than read off a description.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX3_READER_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX3_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::credential::kdbx {

struct Kdbx3ReadResult {
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    std::string error;
    // The decrypted and decompressed XML.
    std::vector<std::uint8_t> payload;

    // The protected stream of the database. In KDBX 3.1 it is in the header (fields 8
    // and 10) rather than in an inner header.
    std::uint32_t protected_stream_id{0};
    std::vector<std::uint8_t> protected_stream_key;
    std::size_t xml_offset{0};
};

// Reads a KDBX 3.1 database.
//
// `key_file` is the raw content of the key file, or empty when there is none. The
// password may be empty for a database that is unlocked by a key file alone.
[[nodiscard]] Kdbx3ReadResult readKdbx3(const std::uint8_t* data, std::size_t size,
                                        const std::string& password,
                                        const std::vector<std::uint8_t>& key_file);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX3_READER_H_
