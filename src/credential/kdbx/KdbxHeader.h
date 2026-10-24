// The KDBX header (P2-T3).
//
// The header is the part of a KeePass database that can be read without the master
// password: it says which cipher, which compression and which key derivation function
// protect the payload. Everything the reader needs to know before it has a key is here.
//
// The byte-level facts this parser is built on are written down with their sources in
// docs/evidence/kdbx-format-notes.md. Two of them are easy to get wrong by memory and
// both of them fail silently rather than loudly:
//
//   * the version number is 0x00030001 / 0x00040000 / 0x00040001, with the major
//     version in the high 16 bits -- not 0x03040000;
//   * the length of a header field is a UInt16 in KDBX 3.1 and an Int32 in KDBX 4.x.
//     Reading a 3.1 file with a 4-byte length does not fail: it reads the high bytes of
//     the length as the next field id.
//
// So the parser is deliberately strict: it reports what it found and refuses anything it
// does not understand, instead of guessing.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_HEADER_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_HEADER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openscrt::credential::kdbx {

// The file format version. The value in the file is 0x00030001 / 0x00040000 /
// 0x00040001: the major version is in the high 16 bits.
enum class KdbxVersion : std::uint8_t {
    kV3_1,
    kV4_0,
    kV4_1,
};

[[nodiscard]] const char* toString(KdbxVersion version) noexcept;

// The cipher that protects the payload.
enum class KdbxCipher : std::uint8_t {
    kAes256Cbc,
    kChaCha20,
    kTwofishCbc,  // a plugin cipher; supported only if a plugin is installed
    kUnknown,
};

[[nodiscard]] const char* toString(KdbxCipher cipher) noexcept;

// The compression of the payload.
enum class KdbxCompression : std::uint8_t {
    kNone,
    kGzip,
};

[[nodiscard]] const char* toString(KdbxCompression compression) noexcept;

// The key derivation function that turns the composite key into the transformed key.
enum class KdbxKdf : std::uint8_t {
    kAesKdf,
    kArgon2d,
    kArgon2id,
    kUnknown,
};

[[nodiscard]] const char* toString(KdbxKdf kdf) noexcept;

// One value of a variant dictionary.
//
// The KDBX variant dictionary is a typed key-value store; the type tag decides how the
// bytes are read. The accessors below return nullopt when the value is not of that
// type, so a caller never has to trust that a key holds what it expects.
class VariantValue {
 public:
    enum class Type : std::uint8_t {
        kUInt32,
        kUInt64,
        kBool,
        kInt32,
        kInt64,
        kString,
        kByteArray,
    };

    using Bytes = std::vector<std::uint8_t>;

    [[nodiscard]] Type type() const noexcept { return type_; }

    // The unsigned types and the boolean. A UInt32 widens to 64 bits.
    [[nodiscard]] std::optional<std::uint64_t> asUnsigned() const noexcept;
    // The signed types.
    [[nodiscard]] std::optional<std::int64_t> asSigned() const noexcept;
    [[nodiscard]] const Bytes* asBytes() const noexcept;
    [[nodiscard]] const std::string* asText() const noexcept;

    // Used by the parser; the public constructors are not part of the interface.
    static VariantValue makeUInt32(std::uint32_t value);
    static VariantValue makeUInt64(std::uint64_t value);
    static VariantValue makeBool(bool value);
    static VariantValue makeInt32(std::int32_t value);
    static VariantValue makeInt64(std::int64_t value);
    static VariantValue makeString(std::string value);
    static VariantValue makeBytes(Bytes value);

 private:
    Type type_{Type::kByteArray};
    std::uint64_t unsigned_{0};
    std::int64_t signed_{0};
    std::string text_;
    Bytes bytes_;
};

// The parameters of the key derivation function.
//
// AES-KDF and Argon2 do not share a single field, so the two sets live side by side and
// only the ones that belong to the selected KDF are meaningful.
struct KdbxKdfParameters {
    KdbxKdf kdf{KdbxKdf::kUnknown};

    // AES-KDF: the salt is the transform seed (KDBX 3.1) or the "S" entry of the
    // variant dictionary (KDBX 4.x), and the rounds are the transform rounds.
    std::array<std::uint8_t, 32> aes_salt{};
    std::uint64_t aes_rounds{0};

    // Argon2. The memory is in BYTES in the file, while libsodium wants KiB.
    std::vector<std::uint8_t> argon2_salt;
    std::uint32_t argon2_version{0};
    std::uint64_t argon2_iterations{0};
    std::uint64_t argon2_memory_bytes{0};
    std::uint32_t argon2_parallelism{0};
};

// Everything the header says about the database.
struct KdbxHeader {
    KdbxVersion version{KdbxVersion::kV4_0};
    KdbxCipher cipher{KdbxCipher::kUnknown};
    KdbxCompression compression{KdbxCompression::kNone};
    KdbxKdfParameters kdf;

    std::array<std::uint8_t, 32> master_seed{};
    std::vector<std::uint8_t> encryption_iv;

    // KDBX 3.1 only: the inner random stream that protects the individual values, and
    // the bytes that prove the key was right.
    std::array<std::uint8_t, 32> inner_random_stream_key{};
    std::array<std::uint8_t, 32> stream_start_bytes{};
    std::uint32_t inner_random_stream_id{0};

    // The raw header bytes, from the first signature to the end of the field list.
    // KDBX 4.x hashes and authenticates exactly these bytes.
    std::vector<std::uint8_t> raw_bytes;

    // Where the payload starts, i.e. how many bytes the header occupies. For KDBX 4.x
    // the 32-byte header hash and the 32-byte header HMAC follow it.
    std::size_t payload_offset{0};
};

// The result of a parse: either a header or a reason.
struct KdbxHeaderParseResult {
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    KdbxHeader header;
    std::string error;
};

// Parses the header at the start of `data`.
//
// `size` is how many bytes are available; the parser never reads past it. A header that
// runs past the end of the buffer is an error, not a truncated result.
[[nodiscard]] KdbxHeaderParseResult parseKdbxHeader(const std::uint8_t* data, std::size_t size);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_HEADER_H_
