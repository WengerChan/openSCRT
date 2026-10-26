// The KDBX header parser's refusal paths (P2-T3).
//
// The happy path is in KdbxHeaderParserTest.cpp. What is here is every way the parser
// can be handed something it must refuse: a file that stops in the middle of a field, a
// field whose length is wrong, a variant dictionary that names a value type this build
// does not know.
//
// The point of these cases is not that the parser says no. It is that it says no *with a
// reason* and without reading past the end of the buffer. A parser that reads out of
// bounds on a malformed file is a parser that can be crashed by a file somebody sends
// you, and the only way to know it does not is to hand it a file that ends in the
// middle of everything.
//
// The bytes are built here rather than taken from a file, because the interesting inputs
// are the ones no real database produces: a field of 15 bytes where 16 are required, a
// key length that points past the end of the buffer, a dictionary that stops before its
// terminator.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

// The bytes of a hex string. The same helper as in KdbxHeaderParserTest.cpp; that one
// lives in an anonymous namespace, so it cannot be shared from here.
std::vector<std::uint8_t> fromHex(std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = value(hex[i]);
        const int low = value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

// The first 253 bytes of a real KDBX 4.0 database: the whole header, ending with the
// end-of-header marker. The same bytes as in KdbxHeaderParserTest.cpp -- a copy, because
// that one is in an anonymous namespace.
const std::string_view kRealKdbx4Header =
    "03d9a29a67fb4bb500000400021000000031c1f2e6bf714350be5805216afc5a"
    "ff0304000000010000000420000000b52178a734d0b577423679d02ab37dd17c"
    "b6781861d216c7d32d52c354ab2123071000000090ba3750281556cb96fea4c4"
    "3545a9760b8b00000000014205000000245555494410000000ef636ddf8c2944"
    "4b91f7a9a403e30a0c050100000049080000000e000000000000000501000000"
    "4d08000000000000040000000004010000005004000000020000004201000000"
    "53200000007839e86543e0449b3d6cc46b95b4bbab7d519c7eb01d9cf59d64d8"
    "2f6ab1a15f04010000005604000000130000000000040000000d0a0d0a";

// The value types of the KDBX variant dictionary.
constexpr std::uint8_t kTypeUInt32 = 0x04;
constexpr std::uint8_t kTypeUInt64 = 0x05;
constexpr std::uint8_t kTypeBool = 0x08;
constexpr std::uint8_t kTypeInt32 = 0x0C;
constexpr std::uint8_t kTypeInt64 = 0x0D;
constexpr std::uint8_t kTypeString = 0x18;
constexpr std::uint8_t kTypeByteArray = 0x42;

const std::vector<std::uint8_t> kAesUuid = {0x31, 0xC1, 0xF2, 0xE6, 0xBF, 0x71, 0x43, 0x50,
                                            0xBE, 0x58, 0x05, 0x21, 0x6A, 0xFC, 0x5A, 0xFF};
const std::vector<std::uint8_t> kChaChaUuid = {0xD6, 0x03, 0x8A, 0x2B, 0x8B, 0x6F, 0x4C, 0xB5,
                                               0xA5, 0x24, 0x33, 0x9A, 0x31, 0xDB, 0xB5, 0x9A};
const std::vector<std::uint8_t> kTwofishUuid = {0xAD, 0x68, 0xF2, 0x9F, 0x57, 0x6F, 0x4B, 0xB9,
                                                0xA3, 0x6A, 0xD4, 0x7A, 0xF9, 0x65, 0x34, 0x6C};
const std::vector<std::uint8_t> kArgon2dUuid = {0xEF, 0x63, 0x6D, 0xDF, 0x8C, 0x29, 0x44, 0x4B,
                                                0x91, 0xF7, 0xA9, 0xA4, 0x03, 0xE3, 0x0A, 0x0C};
const std::vector<std::uint8_t> kAesKdfUuid = {0xC9, 0xD9, 0xF3, 0x9A, 0x62, 0x8A, 0x44, 0x60,
                                               0xBF, 0x74, 0x0D, 0x08, 0xC1, 0x8A, 0x4F, 0xEA};

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    appendU16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    appendU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    appendU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
    appendU32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFF));
}

std::vector<std::uint8_t> u32Value(std::uint32_t value) {
    std::vector<std::uint8_t> bytes;
    appendU32(bytes, value);
    return bytes;
}

std::vector<std::uint8_t> u64Value(std::uint64_t value) {
    std::vector<std::uint8_t> bytes;
    appendU64(bytes, value);
    return bytes;
}

std::vector<std::uint8_t> repeated(std::uint8_t value, std::size_t count) {
    return std::vector<std::uint8_t>(count, value);
}

// One header field: its id and its bytes.
struct Field {
    std::uint8_t id;
    std::vector<std::uint8_t> value;
};

// A header field on the wire: id, length, bytes. The width of the length is the thing
// that differs between KDBX 3.1 (16 bit) and KDBX 4.x (32 bit).
void appendField(std::vector<std::uint8_t>& out, const Field& field, bool wide_length) {
    out.push_back(field.id);
    if (wide_length) {
        appendU32(out, static_cast<std::uint32_t>(field.value.size()));
    } else {
        appendU16(out, static_cast<std::uint16_t>(field.value.size()));
    }
    out.insert(out.end(), field.value.begin(), field.value.end());
}

// One entry of a variant dictionary: a type, a key and the bytes of the value.
struct VariantEntry {
    std::uint8_t type;
    std::string key;
    std::vector<std::uint8_t> value;
};

// A variant dictionary: a two-byte version, then the entries, then the terminator.
std::vector<std::uint8_t> variantDictionary(const std::vector<VariantEntry>& entries) {
    std::vector<std::uint8_t> out{0x00, 0x01};
    for (const VariantEntry& entry : entries) {
        out.push_back(entry.type);
        appendU32(out, static_cast<std::uint32_t>(entry.key.size()));
        out.insert(out.end(), entry.key.begin(), entry.key.end());
        appendU32(out, static_cast<std::uint32_t>(entry.value.size()));
        out.insert(out.end(), entry.value.begin(), entry.value.end());
    }
    out.push_back(0x00);
    return out;
}

// The KDF parameters of a KDBX 4.x file that uses Argon2d.
std::vector<std::uint8_t> argon2dParameters() {
    return variantDictionary({
        {kTypeByteArray, "$UUID", kArgon2dUuid},
        {kTypeByteArray, "S", repeated(0x11, 32)},
        {kTypeUInt64, "V", u64Value(0x13)},
        {kTypeUInt64, "I", u64Value(10)},
        {kTypeUInt64, "M", u64Value(65536)},
        {kTypeUInt64, "P", u64Value(2)},
    });
}

// The KDF parameters of a file that uses the AES-KDF.
std::vector<std::uint8_t> aesKdfParameters() {
    return variantDictionary({
        {kTypeByteArray, "$UUID", kAesKdfUuid},
        {kTypeByteArray, "S", repeated(0x22, 32)},
        {kTypeUInt64, "R", u64Value(60000)},
    });
}

// The fields a KDBX 4.x header must carry.
std::vector<Field> mandatoryFields() {
    return {
        {0x02, kAesUuid},
        {0x03, u32Value(0)},
        {0x04, repeated(0xA5, 32)},
        {0x0B, argon2dParameters()},
    };
}

// A KDBX 4.x header with the given fields, closed by the end-of-header marker.
std::vector<std::uint8_t> kdbx4Header(const std::vector<Field>& fields,
                                      std::uint32_t version = 0x00040000,
                                      const std::vector<std::uint8_t>& marker = {0x0D, 0x0A, 0x0D,
                                                                                 0x0A}) {
    std::vector<std::uint8_t> out;
    appendU32(out, 0x9AA2D903);
    appendU32(out, 0xB54BFB67);
    appendU32(out, version);
    for (const Field& field : fields) {
        appendField(out, field, true);
    }
    appendField(out, Field{0x00, marker}, true);
    return out;
}

// A KDBX 3.1 header with the given fields. The field lengths are 16 bit there.
std::vector<std::uint8_t> kdbx31Header(const std::vector<Field>& fields) {
    std::vector<std::uint8_t> out;
    appendU32(out, 0x9AA2D903);
    appendU32(out, 0xB54BFB67);
    appendU32(out, 0x00030001);
    for (const Field& field : fields) {
        appendField(out, field, false);
    }
    appendField(out, Field{0x00, {0x0D, 0x0A, 0x0D, 0x0A}}, false);
    return out;
}

// A header whose KDF field carries the given variant dictionary.
std::vector<std::uint8_t> headerWithKdfDictionary(const std::vector<std::uint8_t>& dictionary) {
    std::vector<Field> fields = mandatoryFields();
    fields.push_back({0x0B, dictionary});
    return kdbx4Header(fields);
}

TEST(KdbxHeaderParserEdgesTest, AFileThatEndsInTheMiddleOfTheHeaderIsRejected) {
    // The first 253 bytes of a real KDBX 4.0 database. Every prefix of it must be
    // refused, and each refusal must say why -- the parser may not read past the end of
    // the buffer to find out what is missing.
    const std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), length);
        EXPECT_FALSE(result.ok()) << "a header cut to " << length << " bytes parsed";
        EXPECT_FALSE(result.error.empty()) << "a header cut to " << length << " bytes said nothing";
    }
}

TEST(KdbxHeaderParserEdgesTest, AFieldWithTheWrongLengthIsRejected) {
    // Every field with a fixed length says so, and the parser checks the length instead
    // of trusting the file.
    struct Case {
        std::uint8_t id;
        std::size_t wrong_size;
    };
    const std::vector<Case> cases = {
        {0x02, 15},  // the cipher id
        {0x03, 3},   // the compression flags
        {0x04, 31},  // the master seed
        {0x05, 31},  // the transform seed
        {0x06, 7},   // the transform rounds
        {0x08, 31},  // the inner random stream key
        {0x09, 31},  // the stream start bytes
        {0x0A, 3},   // the inner random stream id
    };
    for (const Case& item : cases) {
        std::vector<Field> fields = mandatoryFields();
        fields.push_back({item.id, repeated(0x00, item.wrong_size)});
        const std::vector<std::uint8_t> bytes = kdbx4Header(fields);
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        EXPECT_FALSE(result.ok()) << "field " << static_cast<int>(item.id) << " with "
                                  << item.wrong_size << " bytes parsed";
        EXPECT_FALSE(result.error.empty());
    }
}

TEST(KdbxHeaderParserEdgesTest, TheEndOfHeaderMarkerIsChecked) {
    // The end-of-header field carries a fixed marker. A header that ends differently is
    // not a header this build understands.
    const std::vector<std::uint8_t> bytes =
        kdbx4Header(mandatoryFields(), 0x00040000, {0x0D, 0x0A, 0x0D, 0x00});
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("end-of-header"), std::string::npos) << result.error;
}

TEST(KdbxHeaderParserEdgesTest, AHeaderWithoutAMandatoryFieldIsRejected) {
    // The cipher, the compression and the master seed are not optional.
    for (std::uint8_t missing : {0x02, 0x03, 0x04}) {
        std::vector<Field> fields;
        for (const Field& field : mandatoryFields()) {
            if (field.id != missing) {
                fields.push_back(field);
            }
        }
        const std::vector<std::uint8_t> bytes = kdbx4Header(fields);
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        EXPECT_FALSE(result.ok()) << "a header without field " << static_cast<int>(missing)
                                  << " parsed";
        EXPECT_FALSE(result.error.empty());
    }
}

TEST(KdbxHeaderParserEdgesTest, AVariantDictionaryThatIsCutShortIsRejected) {
    // Four ways a dictionary can be broken: the version is not 0x01, the terminator is
    // missing, a key length points past the end, a value length points past the end.
    const std::vector<std::vector<std::uint8_t>> broken = {
        {0x00, 0x02, 0x00},                               // the version is not 0x01
        {0x00, 0x01, 0x42},                               // the terminator is missing
        {0x00, 0x01, 0x42, 0xFF, 0x00, 0x00, 0x00, 'k'},  // the key runs past the end
        {0x00, 0x01, 0x42, 0x01, 0x00, 0x00, 0x00, 'k', 0xFF, 0x00, 0x00, 0x00},  // the value
    };
    for (const std::vector<std::uint8_t>& dictionary : broken) {
        const std::vector<std::uint8_t> bytes = headerWithKdfDictionary(dictionary);
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(result.error.empty());
    }
}

TEST(KdbxHeaderParserEdgesTest, AValueOfTheWrongSizeIsRejected) {
    // The type tag says how many bytes the value has, and the parser checks that it has
    // them.
    struct Case {
        std::uint8_t type;
        std::size_t size;
    };
    const std::vector<Case> cases = {
        {kTypeUInt32, 3}, {kTypeUInt64, 7}, {kTypeBool, 2}, {kTypeInt32, 3}, {kTypeInt64, 7},
    };
    for (const Case& item : cases) {
        const std::vector<std::uint8_t> dictionary =
            variantDictionary({{item.type, "R", repeated(0x01, item.size)}});
        const std::vector<std::uint8_t> bytes = headerWithKdfDictionary(dictionary);
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        EXPECT_FALSE(result.ok()) << "a value of type " << static_cast<int>(item.type) << " and "
                                  << item.size << " bytes parsed";
        EXPECT_FALSE(result.error.empty());
    }
}

TEST(KdbxHeaderParserEdgesTest, AStringValueIsReadAsText) {
    // The KDF parameters may carry values this build has no use for. A string among
    // them has to be read as text and skipped, not refused.
    const std::vector<std::uint8_t> dictionary =
        variantDictionary({{kTypeString, "Comment", std::vector<std::uint8_t>{'h', 'i'}}});
    const std::vector<std::uint8_t> bytes = headerWithKdfDictionary(dictionary);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
    // It still fails, because the dictionary has no $UUID -- but not because of the string.
    EXPECT_NE(result.error.find("$UUID"), std::string::npos) << result.error;
}

TEST(KdbxHeaderParserEdgesTest, AValueTypeThisBuildDoesNotKnowIsRejected) {
    // A dictionary that names a type this build has no reader for must be refused
    // rather than read as something else.
    const std::vector<std::uint8_t> dictionary =
        variantDictionary({{0x7F, "R", repeated(0x01, 4)}});
    const std::vector<std::uint8_t> bytes = headerWithKdfDictionary(dictionary);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("unknown variant dictionary value type"), std::string::npos)
        << result.error;
}

TEST(KdbxHeaderParserEdgesTest, TheKdfParametersAreChecked) {
    // A KDF that names no key derivation function, names one this build does not know,
    // or names one and then leaves out what it needs.
    struct Case {
        std::vector<VariantEntry> entries;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{}, "no $UUID at all"},
        {{VariantEntry{kTypeByteArray, "$UUID", repeated(0x01, 8)}},
         "a $UUID that is not 16 bytes"},
        {{VariantEntry{kTypeByteArray, "$UUID", kAesKdfUuid}}, "the AES-KDF without a salt"},
        {{VariantEntry{kTypeByteArray, "$UUID", kAesKdfUuid},
          VariantEntry{kTypeByteArray, "S", repeated(0x22, 32)}},
         "the AES-KDF without a round count"},
        {{VariantEntry{kTypeByteArray, "$UUID", kArgon2dUuid}}, "the Argon2 without a salt"},
        {{VariantEntry{kTypeByteArray, "$UUID", kArgon2dUuid},
          VariantEntry{kTypeByteArray, "S", repeated(0x11, 32)},
          VariantEntry{kTypeUInt64, "V", u64Value(0x13)},
          VariantEntry{kTypeUInt64, "I", u64Value(10)}},
         "the Argon2 without a memory and a parallelism"},
    };
    for (const Case& item : cases) {
        const std::vector<std::uint8_t> bytes =
            headerWithKdfDictionary(variantDictionary(item.entries));
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        EXPECT_FALSE(result.ok()) << item.what << " parsed";
        EXPECT_FALSE(result.error.empty()) << item.what << " said nothing";
    }
}

TEST(KdbxHeaderParserEdgesTest, TheOtherCiphersAreRecognised) {
    // ChaCha20 and Twofish are ciphers this build knows. A uuid it does not know is a
    // database it cannot open, and the parser has to say so rather than guess.
    struct Case {
        std::vector<std::uint8_t> uuid;
        KdbxCipher expected;
    };
    const std::vector<Case> cases = {
        {kAesUuid, KdbxCipher::kAes256Cbc},
        {kChaChaUuid, KdbxCipher::kChaCha20},
        {kTwofishUuid, KdbxCipher::kTwofishCbc},
    };
    for (const Case& item : cases) {
        std::vector<Field> fields = mandatoryFields();
        fields[0].value = item.uuid;
        const std::vector<std::uint8_t> bytes = kdbx4Header(fields);
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
        ASSERT_TRUE(result.ok()) << result.error;
        EXPECT_EQ(result.header.cipher, item.expected);
    }

    std::vector<Field> fields = mandatoryFields();
    fields[0].value = repeated(0x77, 16);
    const std::vector<std::uint8_t> unknown = kdbx4Header(fields);
    const KdbxHeaderParseResult result = parseKdbxHeader(unknown.data(), unknown.size());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("cipher"), std::string::npos) << result.error;
}

TEST(KdbxHeaderParserEdgesTest, Kdbx41IsRecognised) {
    // The version number carries the major version in the high half and the minor one in
    // the low half, so 4.0 and 4.1 are different files with the same framing.
    const std::vector<std::uint8_t> bytes = kdbx4Header(mandatoryFields(), 0x00040001);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.header.version, KdbxVersion::kV4_1);
}

TEST(KdbxHeaderParserEdgesTest, AKdbx31HeaderCanCarryAesKdfParameters) {
    // KDBX 3.1 keeps the AES-KDF parameters in the transform seed and rounds, but it may
    // also carry them in a field of their own -- and then the field is what says how the
    // salt and the round count are to be read.
    std::vector<Field> fields = {
        {0x02, kAesUuid},
        {0x03, u32Value(0)},
        {0x04, repeated(0xA5, 32)},
        {0x0B, aesKdfParameters()},
    };
    const std::vector<std::uint8_t> bytes = kdbx31Header(fields);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.header.kdf.kdf, KdbxKdf::kAesKdf);
    EXPECT_EQ(result.header.kdf.aes_rounds, 60000u);
    EXPECT_EQ(result.header.kdf.aes_salt[0], 0x22);
}

TEST(KdbxHeaderParserEdgesTest, TheVariantValuesSayWhatTheyAre) {
    // The accessors return nothing when the value is not of that type, so a caller never
    // has to trust that a key holds what it expects.
    EXPECT_EQ(VariantValue::makeBool(true).asUnsigned().value_or(0), 1u);
    EXPECT_EQ(VariantValue::makeBool(false).asUnsigned().value_or(1), 0u);
    EXPECT_EQ(VariantValue::makeUInt32(7).asUnsigned().value_or(0), 7u);
    EXPECT_EQ(VariantValue::makeUInt64(8).asUnsigned().value_or(0), 8u);
    EXPECT_EQ(VariantValue::makeInt32(-3).asSigned().value_or(0), -3);
    EXPECT_EQ(VariantValue::makeInt64(-4).asSigned().value_or(0), -4);
    EXPECT_NE(VariantValue::makeString("text").asText(), nullptr);
    EXPECT_NE(VariantValue::makeBytes({1, 2}).asBytes(), nullptr);

    // The signed and the unsigned accessors do not overlap, and neither of them answers
    // for a value of another type.
    EXPECT_FALSE(VariantValue::makeInt32(1).asUnsigned().has_value());
    EXPECT_FALSE(VariantValue::makeUInt32(1).asSigned().has_value());
    EXPECT_EQ(VariantValue::makeString("text").asBytes(), nullptr);
    EXPECT_EQ(VariantValue::makeBytes({1}).asText(), nullptr);
}

TEST(KdbxHeaderParserEdgesTest, TheNamesAreTheSameForTheUnknownOnes) {
    // A value the build does not know still has to print as something, because the name
    // ends up in an error message.
    EXPECT_STREQ(toString(KdbxVersion::kV3_1), "3.1");
    EXPECT_STREQ(toString(static_cast<KdbxVersion>(99)), "unknown");
    EXPECT_STREQ(toString(KdbxCipher::kAes256Cbc), "AES-256-CBC");
    EXPECT_STREQ(toString(KdbxCipher::kChaCha20), "ChaCha20");
    EXPECT_STREQ(toString(KdbxCipher::kTwofishCbc), "Twofish-CBC");
    EXPECT_STREQ(toString(KdbxCipher::kUnknown), "unknown");
    EXPECT_STREQ(toString(static_cast<KdbxCipher>(99)), "unknown");
    EXPECT_STREQ(toString(KdbxCompression::kNone), "none");
    EXPECT_STREQ(toString(KdbxCompression::kGzip), "gzip");
    EXPECT_STREQ(toString(static_cast<KdbxCompression>(99)), "unknown");
    EXPECT_STREQ(toString(KdbxKdf::kAesKdf), "AES-KDF");
    EXPECT_STREQ(toString(KdbxKdf::kArgon2d), "Argon2d");
    EXPECT_STREQ(toString(KdbxKdf::kArgon2id), "Argon2id");
    EXPECT_STREQ(toString(KdbxKdf::kUnknown), "unknown");
    EXPECT_STREQ(toString(static_cast<KdbxKdf>(99)), "unknown");
}

}  // namespace
}  // namespace openscrt::credential::kdbx
