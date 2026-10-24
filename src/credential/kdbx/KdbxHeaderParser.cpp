#include <algorithm>
#include <cstring>
#include <utility>

#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

// The signatures. KDBX 3.1 and KDBX 4.x differ in the second one, and getting that
// wrong means reading a 3.1 file as if it were 4.x -- or the other way round.
constexpr std::uint32_t kSignature1 = 0x9AA2D903;
constexpr std::uint32_t kSignature2V4 = 0xB54BFB67;  // KDBX 4.x
constexpr std::uint32_t kSignature2V3 = 0xB54BFB66;  // KDBX 3.1 (KeePass 2.x pre-release)

// The version numbers. The major version is in the high 16 bits.
constexpr std::uint32_t kVersion3_1 = 0x00030001;
constexpr std::uint32_t kVersion4_0 = 0x00040000;
constexpr std::uint32_t kVersion4_1 = 0x00040001;
constexpr std::uint32_t kVersionCriticalMask = 0xFFFF0000;

// The header field ids.
enum HeaderFieldId : std::uint8_t {
    kFieldEndOfHeader = 0,
    kFieldComment = 1,
    kFieldCipherId = 2,
    kFieldCompressionFlags = 3,
    kFieldMasterSeed = 4,
    kFieldTransformSeed = 5,
    kFieldTransformRounds = 6,
    kFieldEncryptionIv = 7,
    kFieldInnerRandomStreamKey = 8,
    kFieldStreamStartBytes = 9,
    kFieldInnerRandomStreamId = 10,
    kFieldKdfParameters = 11,
    kFieldPublicCustomData = 12,
};

// The cipher uuids.
constexpr std::array<std::uint8_t, 16> kCipherAes256 = {
    0x31, 0xC1, 0xF2, 0xE6, 0xBF, 0x71, 0x43, 0x50, 0xBE, 0x58, 0x05, 0x21, 0x6A, 0xFC, 0x5A, 0xFF};
constexpr std::array<std::uint8_t, 16> kCipherChaCha20 = {
    0xD6, 0x03, 0x8A, 0x2B, 0x8B, 0x6F, 0x4C, 0xB5, 0xA5, 0x24, 0x33, 0x9A, 0x31, 0xDB, 0xB5, 0x9A};
constexpr std::array<std::uint8_t, 16> kCipherTwofish = {
    0xAD, 0x68, 0xF2, 0x9F, 0x57, 0x6F, 0x4B, 0xB9, 0xA3, 0x6A, 0xD4, 0x7A, 0xF9, 0x65, 0x34, 0x6C};

// The key derivation uuids.
constexpr std::array<std::uint8_t, 16> kKdfAes = {0xC9, 0xD9, 0xF3, 0x9A, 0x62, 0x8A, 0x44, 0x60,
                                                  0xBF, 0x74, 0x0D, 0x08, 0xC1, 0x8A, 0x4F, 0xEA};
constexpr std::array<std::uint8_t, 16> kKdfArgon2d = {
    0xEF, 0x63, 0x6D, 0xDF, 0x8C, 0x29, 0x44, 0x4B, 0x91, 0xF7, 0xA9, 0xA4, 0x03, 0xE3, 0x0A, 0x0C};
constexpr std::array<std::uint8_t, 16> kKdfArgon2id = {
    0x9E, 0x29, 0x8B, 0x19, 0x56, 0xDB, 0x47, 0x73, 0xB2, 0x3D, 0xFC, 0x3E, 0xC6, 0xF0, 0xA1, 0xE6};

template <std::size_t N>
bool matchesUuid(const std::uint8_t* data, const std::array<std::uint8_t, N>& uuid) noexcept {
    return std::memcmp(data, uuid.data(), N) == 0;
}

// A bounds-checked reader. Every read that would run past the end fails instead of
// reading whatever happens to be there.
class ByteReader {
 public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool readU8(std::uint8_t& out) noexcept {
        if (remaining() < 1) {
            return false;
        }
        out = data_[position_];
        position_ += 1;
        return true;
    }

    [[nodiscard]] bool readU16(std::uint16_t& out) noexcept {
        if (remaining() < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>(data_[position_]) |
              (static_cast<std::uint16_t>(data_[position_ + 1]) << 8);
        position_ += 2;
        return true;
    }

    [[nodiscard]] bool readU32(std::uint32_t& out) noexcept {
        if (remaining() < 4) {
            return false;
        }
        out = static_cast<std::uint32_t>(data_[position_]) |
              (static_cast<std::uint32_t>(data_[position_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[position_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[position_ + 3]) << 24);
        position_ += 4;
        return true;
    }

    [[nodiscard]] bool readU64(std::uint64_t& out) noexcept {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        if (!readU32(low) || !readU32(high)) {
            return false;
        }
        out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
        return true;
    }

    [[nodiscard]] bool skip(std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        position_ += count;
        return true;
    }

    [[nodiscard]] const std::uint8_t* current() const noexcept { return data_ + position_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }

 private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_{0};
};

struct DictionaryEntry {
    std::string key;
    VariantValue value;
};

// Parses a variant dictionary: a version, the typed entries, and a zero terminator.
bool parseVariantDictionary(const std::uint8_t* data, std::size_t size,
                            std::vector<DictionaryEntry>& out, std::string& error) {
    ByteReader reader(data, size);
    std::uint16_t version = 0;
    if (!reader.readU16(version)) {
        error = "the variant dictionary is truncated before its version";
        return false;
    }
    if ((version >> 8) != 0x01) {
        error = "unsupported variant dictionary version";
        return false;
    }

    while (true) {
        std::uint8_t type = 0;
        if (!reader.readU8(type)) {
            error = "the variant dictionary is truncated before its terminator";
            return false;
        }
        if (type == 0x00) {
            return true;
        }

        std::uint32_t key_size = 0;
        if (!reader.readU32(key_size)) {
            error = "the variant dictionary is truncated in a key length";
            return false;
        }
        if (key_size > reader.remaining()) {
            error = "a variant dictionary key runs past the end of the buffer";
            return false;
        }
        std::string key(reinterpret_cast<const char*>(reader.current()), key_size);
        if (!reader.skip(key_size)) {
            error = "a variant dictionary key runs past the end of the buffer";
            return false;
        }

        std::uint32_t value_size = 0;
        if (!reader.readU32(value_size)) {
            error = "the variant dictionary is truncated in a value length";
            return false;
        }
        if (value_size > reader.remaining()) {
            error = "a variant dictionary value runs past the end of the buffer";
            return false;
        }
        const std::uint8_t* value_bytes = reader.current();
        if (!reader.skip(value_size)) {
            error = "a variant dictionary value runs past the end of the buffer";
            return false;
        }

        switch (type) {
            case 0x04: {  // UInt32
                std::uint32_t value = 0;
                ByteReader value_reader(value_bytes, value_size);
                if (!value_reader.readU32(value)) {
                    error = "a UInt32 value is not four bytes long";
                    return false;
                }
                out.push_back({std::move(key), VariantValue::makeUInt32(value)});
                break;
            }
            case 0x05: {  // UInt64
                std::uint64_t value = 0;
                ByteReader value_reader(value_bytes, value_size);
                if (!value_reader.readU64(value)) {
                    error = "a UInt64 value is not eight bytes long";
                    return false;
                }
                out.push_back({std::move(key), VariantValue::makeUInt64(value)});
                break;
            }
            case 0x08: {  // Bool
                if (value_size != 1) {
                    error = "a boolean value is not one byte long";
                    return false;
                }
                out.push_back({std::move(key), VariantValue::makeBool(value_bytes[0] != 0)});
                break;
            }
            case 0x0C: {  // Int32
                std::uint32_t raw = 0;
                ByteReader value_reader(value_bytes, value_size);
                if (!value_reader.readU32(raw)) {
                    error = "an Int32 value is not four bytes long";
                    return false;
                }
                out.push_back(
                    {std::move(key), VariantValue::makeInt32(static_cast<std::int32_t>(raw))});
                break;
            }
            case 0x0D: {  // Int64
                std::uint64_t raw = 0;
                ByteReader value_reader(value_bytes, value_size);
                if (!value_reader.readU64(raw)) {
                    error = "an Int64 value is not eight bytes long";
                    return false;
                }
                out.push_back(
                    {std::move(key), VariantValue::makeInt64(static_cast<std::int64_t>(raw))});
                break;
            }
            case 0x18: {  // String
                out.push_back(
                    {std::move(key), VariantValue::makeString(std::string(
                                         reinterpret_cast<const char*>(value_bytes), value_size))});
                break;
            }
            case 0x42: {  // ByteArray
                VariantValue::Bytes bytes(value_bytes, value_bytes + value_size);
                out.push_back({std::move(key), VariantValue::makeBytes(std::move(bytes))});
                break;
            }
            default:
                error = "unknown variant dictionary value type";
                return false;
        }
    }
}

const VariantValue* findEntry(const std::vector<DictionaryEntry>& entries,
                              std::string_view key) noexcept {
    for (const DictionaryEntry& entry : entries) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

// Reads the KDF parameters out of the variant dictionary of a KDBX 4.x file.
bool parseKdfParameters(const std::uint8_t* data, std::size_t size, KdbxKdfParameters& out,
                        std::string& error) {
    std::vector<DictionaryEntry> entries;
    if (!parseVariantDictionary(data, size, entries, error)) {
        return false;
    }

    const VariantValue* uuid = findEntry(entries, "$UUID");
    if (uuid == nullptr) {
        error = "the KDF parameters have no $UUID";
        return false;
    }
    const VariantValue::Bytes* uuid_bytes = uuid->asBytes();
    if (uuid_bytes == nullptr || uuid_bytes->size() != 16) {
        error = "the KDF $UUID is not a 16-byte value";
        return false;
    }

    if (matchesUuid(uuid_bytes->data(), kKdfAes)) {
        out.kdf = KdbxKdf::kAesKdf;
        const VariantValue* salt = findEntry(entries, "S");
        const VariantValue* rounds = findEntry(entries, "R");
        const VariantValue::Bytes* salt_bytes = salt != nullptr ? salt->asBytes() : nullptr;
        const std::optional<std::uint64_t> round_count =
            rounds != nullptr ? rounds->asUnsigned() : std::nullopt;
        if (salt_bytes == nullptr || salt_bytes->size() < out.aes_salt.size()) {
            error = "the AES-KDF parameters have no usable salt";
            return false;
        }
        if (!round_count.has_value()) {
            error = "the AES-KDF parameters have no round count";
            return false;
        }
        std::copy(salt_bytes->begin(), salt_bytes->begin() + out.aes_salt.size(),
                  out.aes_salt.begin());
        out.aes_rounds = *round_count;
        return true;
    }

    if (matchesUuid(uuid_bytes->data(), kKdfArgon2d) ||
        matchesUuid(uuid_bytes->data(), kKdfArgon2id)) {
        out.kdf =
            matchesUuid(uuid_bytes->data(), kKdfArgon2id) ? KdbxKdf::kArgon2id : KdbxKdf::kArgon2d;
        const VariantValue* salt = findEntry(entries, "S");
        const VariantValue* version = findEntry(entries, "V");
        const VariantValue* iterations = findEntry(entries, "I");
        const VariantValue* memory = findEntry(entries, "M");
        const VariantValue* parallelism = findEntry(entries, "P");

        const VariantValue::Bytes* salt_bytes = salt != nullptr ? salt->asBytes() : nullptr;
        const std::optional<std::uint64_t> argon2_version =
            version != nullptr ? version->asUnsigned() : std::nullopt;
        const std::optional<std::uint64_t> argon2_iterations =
            iterations != nullptr ? iterations->asUnsigned() : std::nullopt;
        const std::optional<std::uint64_t> argon2_memory =
            memory != nullptr ? memory->asUnsigned() : std::nullopt;
        const std::optional<std::uint64_t> argon2_parallelism =
            parallelism != nullptr ? parallelism->asUnsigned() : std::nullopt;

        if (salt_bytes == nullptr || salt_bytes->size() < 8) {
            error = "the Argon2 parameters have no usable salt";
            return false;
        }
        if (!argon2_version.has_value() || !argon2_iterations.has_value() ||
            !argon2_memory.has_value() || !argon2_parallelism.has_value()) {
            error = "the Argon2 parameters are incomplete";
            return false;
        }
        out.argon2_salt = *salt_bytes;
        out.argon2_version = static_cast<std::uint32_t>(*argon2_version);
        out.argon2_iterations = *argon2_iterations;
        out.argon2_memory_bytes = *argon2_memory;
        out.argon2_parallelism = static_cast<std::uint32_t>(*argon2_parallelism);
        return true;
    }

    error = "the database uses a key derivation function this build does not know";
    return false;
}

}  // namespace

const char* toString(KdbxVersion version) noexcept {
    switch (version) {
        case KdbxVersion::kV3_1:
            return "3.1";
        case KdbxVersion::kV4_0:
            return "4.0";
        case KdbxVersion::kV4_1:
            return "4.1";
    }
    return "unknown";
}

const char* toString(KdbxCipher cipher) noexcept {
    switch (cipher) {
        case KdbxCipher::kAes256Cbc:
            return "AES-256-CBC";
        case KdbxCipher::kChaCha20:
            return "ChaCha20";
        case KdbxCipher::kTwofishCbc:
            return "Twofish-CBC";
        case KdbxCipher::kUnknown:
            return "unknown";
    }
    return "unknown";
}

const char* toString(KdbxCompression compression) noexcept {
    switch (compression) {
        case KdbxCompression::kNone:
            return "none";
        case KdbxCompression::kGzip:
            return "gzip";
    }
    return "unknown";
}

const char* toString(KdbxKdf kdf) noexcept {
    switch (kdf) {
        case KdbxKdf::kAesKdf:
            return "AES-KDF";
        case KdbxKdf::kArgon2d:
            return "Argon2d";
        case KdbxKdf::kArgon2id:
            return "Argon2id";
        case KdbxKdf::kUnknown:
            return "unknown";
    }
    return "unknown";
}

std::optional<std::uint64_t> VariantValue::asUnsigned() const noexcept {
    switch (type_) {
        case Type::kUInt32:
        case Type::kUInt64:
            return unsigned_;
        case Type::kBool:
            return unsigned_ != 0 ? 1u : 0u;
        default:
            return std::nullopt;
    }
}

std::optional<std::int64_t> VariantValue::asSigned() const noexcept {
    switch (type_) {
        case Type::kInt32:
        case Type::kInt64:
            return signed_;
        default:
            return std::nullopt;
    }
}

const VariantValue::Bytes* VariantValue::asBytes() const noexcept {
    return type_ == Type::kByteArray ? &bytes_ : nullptr;
}

const std::string* VariantValue::asText() const noexcept {
    return type_ == Type::kString ? &text_ : nullptr;
}

VariantValue VariantValue::makeUInt32(std::uint32_t value) {
    VariantValue result;
    result.type_ = Type::kUInt32;
    result.unsigned_ = value;
    return result;
}

VariantValue VariantValue::makeUInt64(std::uint64_t value) {
    VariantValue result;
    result.type_ = Type::kUInt64;
    result.unsigned_ = value;
    return result;
}

VariantValue VariantValue::makeBool(bool value) {
    VariantValue result;
    result.type_ = Type::kBool;
    result.unsigned_ = value ? 1u : 0u;
    return result;
}

VariantValue VariantValue::makeInt32(std::int32_t value) {
    VariantValue result;
    result.type_ = Type::kInt32;
    result.signed_ = value;
    return result;
}

VariantValue VariantValue::makeInt64(std::int64_t value) {
    VariantValue result;
    result.type_ = Type::kInt64;
    result.signed_ = value;
    return result;
}

VariantValue VariantValue::makeString(std::string value) {
    VariantValue result;
    result.type_ = Type::kString;
    result.text_ = std::move(value);
    return result;
}

VariantValue VariantValue::makeBytes(Bytes value) {
    VariantValue result;
    result.type_ = Type::kByteArray;
    result.bytes_ = std::move(value);
    return result;
}

KdbxHeaderParseResult parseKdbxHeader(const std::uint8_t* data, std::size_t size) {
    KdbxHeaderParseResult result;
    if (data == nullptr || size < 12) {
        result.error = "the file is too short to be a KDBX database";
        return result;
    }

    ByteReader reader(data, size);
    std::uint32_t signature1 = 0;
    std::uint32_t signature2 = 0;
    std::uint32_t version = 0;
    if (!reader.readU32(signature1) || !reader.readU32(signature2) || !reader.readU32(version)) {
        result.error = "the file is too short to be a KDBX database";
        return result;
    }

    if (signature1 != kSignature1) {
        result.error = "not a KDBX file: the first signature does not match";
        return result;
    }

    const std::uint32_t major = version & kVersionCriticalMask;
    const std::uint32_t minor = version & ~kVersionCriticalMask;
    if (major == kVersion3_1 && minor == 0x0001) {
        result.header.version = KdbxVersion::kV3_1;
    } else if (major == kVersion4_0 && minor == 0x0000) {
        result.header.version = KdbxVersion::kV4_0;
    } else if (major == kVersion4_0 && minor == 0x0001) {
        result.header.version = KdbxVersion::kV4_1;
    } else {
        result.error = "unsupported KDBX version";
        return result;
    }

    // The second signature tells 3.1 and 4.x apart, and it has to agree with the
    // version: a file that claims 3.1 but carries the 4.x signature is not a file we
    // should be guessing about.
    const bool is_v3 = result.header.version == KdbxVersion::kV3_1;
    const std::uint32_t expected_signature2 = is_v3 ? kSignature2V3 : kSignature2V4;
    if (signature2 != expected_signature2) {
        result.error = "the KDBX signatures and the version do not agree";
        return result;
    }

    // KDBX 3.1 writes the length of a header field as a UInt16; KDBX 4.x as an Int32.
    // Reading a 3.1 file with the 4.x width does not fail -- it reads the high bytes of
    // the length as the next field id -- so the width follows the version.
    const bool field_length_is_32_bit = !is_v3;

    bool saw_end_of_header = false;
    bool have_cipher = false;
    bool have_compression = false;
    bool have_master_seed = false;
    bool have_kdf = false;

    while (true) {
        std::uint8_t field_id = 0;
        if (!reader.readU8(field_id)) {
            result.error = "the header ends without an end-of-header field";
            return result;
        }

        std::size_t field_size = 0;
        if (field_length_is_32_bit) {
            std::uint32_t raw = 0;
            if (!reader.readU32(raw)) {
                result.error = "the header is truncated in a field length";
                return result;
            }
            if (raw > reader.remaining()) {
                result.error = "a header field runs past the end of the buffer";
                return result;
            }
            field_size = raw;
        } else {
            std::uint16_t raw = 0;
            if (!reader.readU16(raw)) {
                result.error = "the header is truncated in a field length";
                return result;
            }
            if (raw > reader.remaining()) {
                result.error = "a header field runs past the end of the buffer";
                return result;
            }
            field_size = raw;
        }

        const std::uint8_t* field_data = reader.current();
        if (!reader.skip(field_size)) {
            result.error = "a header field runs past the end of the buffer";
            return result;
        }

        switch (field_id) {
            case kFieldEndOfHeader: {
                if (field_size != 4 || field_data[0] != 0x0D || field_data[1] != 0x0A ||
                    field_data[2] != 0x0D || field_data[3] != 0x0A) {
                    result.error = "the end-of-header field does not carry the expected marker";
                    return result;
                }
                saw_end_of_header = true;
                break;
            }
            case kFieldCipherId: {
                if (field_size != 16) {
                    result.error = "the cipher id is not 16 bytes long";
                    return result;
                }
                if (matchesUuid(field_data, kCipherAes256)) {
                    result.header.cipher = KdbxCipher::kAes256Cbc;
                } else if (matchesUuid(field_data, kCipherChaCha20)) {
                    result.header.cipher = KdbxCipher::kChaCha20;
                } else if (matchesUuid(field_data, kCipherTwofish)) {
                    result.header.cipher = KdbxCipher::kTwofishCbc;
                } else {
                    result.header.cipher = KdbxCipher::kUnknown;
                }
                have_cipher = true;
                break;
            }
            case kFieldCompressionFlags: {
                if (field_size != 4) {
                    result.error = "the compression flags are not four bytes long";
                    return result;
                }
                std::uint32_t flags = 0;
                ByteReader flags_reader(field_data, field_size);
                (void)flags_reader.readU32(flags);  // the length was checked above
                if (flags == 0) {
                    result.header.compression = KdbxCompression::kNone;
                } else if (flags == 1) {
                    result.header.compression = KdbxCompression::kGzip;
                } else {
                    result.error = "unsupported compression algorithm";
                    return result;
                }
                have_compression = true;
                break;
            }
            case kFieldMasterSeed: {
                if (field_size != 32) {
                    result.error = "the master seed is not 32 bytes long";
                    return result;
                }
                std::copy(field_data, field_data + 32, result.header.master_seed.begin());
                have_master_seed = true;
                break;
            }
            case kFieldTransformSeed: {
                if (field_size != 32) {
                    result.error = "the transform seed is not 32 bytes long";
                    return result;
                }
                std::copy(field_data, field_data + 32, result.header.kdf.aes_salt.begin());
                break;
            }
            case kFieldTransformRounds: {
                if (field_size != 8) {
                    result.error = "the transform rounds are not eight bytes long";
                    return result;
                }
                std::uint64_t rounds = 0;
                ByteReader rounds_reader(field_data, field_size);
                (void)rounds_reader.readU64(rounds);  // the length was checked above
                result.header.kdf.aes_rounds = rounds;
                break;
            }
            case kFieldEncryptionIv: {
                result.header.encryption_iv.assign(field_data, field_data + field_size);
                break;
            }
            case kFieldInnerRandomStreamKey: {
                if (field_size != 32) {
                    result.error = "the inner random stream key is not 32 bytes long";
                    return result;
                }
                std::copy(field_data, field_data + 32,
                          result.header.inner_random_stream_key.begin());
                break;
            }
            case kFieldStreamStartBytes: {
                if (field_size != 32) {
                    result.error = "the stream start bytes are not 32 bytes long";
                    return result;
                }
                std::copy(field_data, field_data + 32, result.header.stream_start_bytes.begin());
                break;
            }
            case kFieldInnerRandomStreamId: {
                if (field_size != 4) {
                    result.error = "the inner random stream id is not four bytes long";
                    return result;
                }
                std::uint32_t stream_id = 0;
                ByteReader stream_reader(field_data, field_size);
                (void)stream_reader.readU32(stream_id);  // the length was checked above
                result.header.inner_random_stream_id = stream_id;
                break;
            }
            case kFieldKdfParameters: {
                std::string error;
                if (!parseKdfParameters(field_data, field_size, result.header.kdf, error)) {
                    result.error = error;
                    return result;
                }
                have_kdf = true;
                break;
            }
            case kFieldComment:
            case kFieldPublicCustomData:
            default:
                // A field this build does not know is skipped by its length. That is the
                // only way to stay compatible with the versions that add fields.
                break;
        }

        if (saw_end_of_header) {
            // The end-of-header field is the last one; the header ends here.
            result.header.raw_bytes.assign(data, data + reader.position());
            result.header.payload_offset = reader.position();
            break;
        }
    }

    if (!have_cipher || !have_compression || !have_master_seed) {
        result.error = "the header is missing the cipher, the compression or the master seed";
        return result;
    }
    if (!have_kdf) {
        if (is_v3) {
            // KDBX 3.1 keeps the AES-KDF parameters in the transform seed and rounds.
            result.header.kdf.kdf = KdbxKdf::kAesKdf;
            have_kdf = result.header.kdf.aes_rounds != 0;
        }
        if (!have_kdf) {
            result.error = "the header has no key derivation parameters";
            return result;
        }
    }
    if (result.header.cipher == KdbxCipher::kUnknown) {
        result.error = "the database uses a cipher this build does not know";
        return result;
    }

    return result;
}

}  // namespace openscrt::credential::kdbx
