// The KDBX 4.x reader (P2-T4).
//
// Reads a KDBX 4 database with a master password and an optional key file, and hands
// back the decrypted XML. The XML itself is parsed in P2-T6; this layer is about the
// cryptography and the framing, and it verifies every step rather than assuming it:
// the header hash, the header HMAC and every block's HMAC are checked, so a wrong
// password or a damaged file is reported as an error instead of producing a database
// of nonsense.
//
// The byte-level facts are in docs/evidence/kdbx-format-notes.md.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX4_READER_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX4_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::credential::kdbx {

struct Kdbx4ReadResult {
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    std::string error;
    // The decrypted and decompressed payload: the inner header, then the XML.
    std::vector<std::uint8_t> payload;

    // The inner header of a KDBX 4 database: which protected stream protects the
    // individual values, and where the XML starts inside the payload.
    std::uint32_t protected_stream_id{0};
    std::vector<std::uint8_t> protected_stream_key;
    std::size_t xml_offset{0};
};

// Reads a KDBX 4.x database.
//
// `key_file` is the raw content of the key file, or empty when there is none. The
// password may be empty for a database that is unlocked by a key file alone, and it
// comes in as bytes rather than as a string: a string leaves a copy on the heap.
[[nodiscard]] Kdbx4ReadResult readKdbx4(const std::uint8_t* data, std::size_t size,
                                        const std::uint8_t* password, std::size_t password_size,
                                        const std::vector<std::uint8_t>& key_file);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX4_READER_H_
