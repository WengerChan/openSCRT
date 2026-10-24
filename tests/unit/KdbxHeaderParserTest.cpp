// KDBX header parser tests (P2-T3).
//
// The parser's input is a binary format, and the two ways to get it wrong are both
// silent: a header field length read with the wrong width does not fail, it just
// produces a different header. So the tests are built around bytes that came from
// somewhere else:
//
//   * the KDBX 4.0 header below is the first 253 bytes of a real KeePass database
//     (the blank database that ships with pykeepass), copied verbatim -- not bytes
//     this project produced;
//   * the KDBX 3.1 header is assembled here from the rules in the reference
//     implementation, and the interesting part is that the *same* bytes with the
//     KDBX 4.x framing do NOT parse.
//
// The facts this parser rests on are written down with their sources in
// docs/evidence/kdbx-format-notes.md.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "credential/kdbx/KdbxHeader.h"

namespace openscrt::credential::kdbx {
namespace {

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
// end-of-header marker. AES-256-CBC, GZip, Argon2d with 14 iterations, 256 KiB and
// two lanes.
const std::string_view kRealKdbx4Header =
    "03d9a29a67fb4bb500000400021000000031c1f2e6bf714350be5805216afc5a"
    "ff0304000000010000000420000000b52178a734d0b577423679d02ab37dd17c"
    "b6781861d216c7d32d52c354ab2123071000000090ba3750281556cb96fea4c4"
    "3545a9760b8b00000000014205000000245555494410000000ef636ddf8c2944"
    "4b91f7a9a403e30a0c050100000049080000000e000000000000000501000000"
    "4d08000000000000040000000004010000005004000000020000004201000000"
    "53200000007839e86543e0449b3d6cc46b95b4bbab7d519c7eb01d9cf59d64d8"
    "2f6ab1a15f04010000005604000000130000000000040000000d0a0d0a";

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

// A header field: id, length, value. The width of the length is the thing that
// differs between KDBX 3.1 and KDBX 4.x.
void appendField(std::vector<std::uint8_t>& out, std::uint8_t id,
                 const std::vector<std::uint8_t>& value, bool wide_length) {
    out.push_back(id);
    if (wide_length) {
        appendU32(out, static_cast<std::uint32_t>(value.size()));
    } else {
        appendU16(out, static_cast<std::uint16_t>(value.size()));
    }
    out.insert(out.end(), value.begin(), value.end());
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

// Builds a KDBX 3.1 header. The field lengths are 16-bit there, which is the whole
// point of the second parameter.
std::vector<std::uint8_t> buildKdbx31Header(bool wide_length, std::uint32_t signature2,
                                            std::uint32_t version) {
    const std::vector<std::uint8_t> aes_uuid = {0x31, 0xC1, 0xF2, 0xE6, 0xBF, 0x71, 0x43, 0x50,
                                                0xBE, 0x58, 0x05, 0x21, 0x6A, 0xFC, 0x5A, 0xFF};

    std::vector<std::uint8_t> header;
    appendU32(header, 0x9AA2D903);
    appendU32(header, signature2);
    appendU32(header, version);
    appendField(header, 0x02, aes_uuid, wide_length);
    appendField(header, 0x03, u32Value(0), wide_length);
    appendField(header, 0x04, repeated(0xA5, 32), wide_length);
    appendField(header, 0x05, repeated(0x5A, 32), wide_length);
    appendField(header, 0x06, u64Value(60000), wide_length);
    appendField(header, 0x07, repeated(0x11, 16), wide_length);
    appendField(header, 0x08, repeated(0x22, 32), wide_length);
    appendField(header, 0x09, repeated(0x33, 32), wide_length);
    appendField(header, 0x0A, u32Value(2), wide_length);
    appendField(header, 0x00, {0x0D, 0x0A, 0x0D, 0x0A}, wide_length);
    return header;
}

TEST(KdbxHeaderParserTest, TheRealKdbx4HeaderParses) {
    const std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    ASSERT_EQ(bytes.size(), 253u);

    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok()) << result.error;

    EXPECT_EQ(result.header.version, KdbxVersion::kV4_0);
    EXPECT_EQ(result.header.cipher, KdbxCipher::kAes256Cbc);
    EXPECT_EQ(result.header.compression, KdbxCompression::kGzip);
    EXPECT_EQ(result.header.master_seed[0], 0xB5);
    EXPECT_EQ(result.header.master_seed[31], 0x07);
    ASSERT_EQ(result.header.encryption_iv.size(), 16u);
    EXPECT_EQ(result.header.encryption_iv[0], 0x90);
    EXPECT_EQ(result.header.encryption_iv[15], 0x76);

    EXPECT_EQ(result.header.kdf.kdf, KdbxKdf::kArgon2d);
    EXPECT_EQ(result.header.kdf.argon2_iterations, 14u);
    // The memory is in bytes in the file, not in KiB.
    EXPECT_EQ(result.header.kdf.argon2_memory_bytes, 262144u);
    EXPECT_EQ(result.header.kdf.argon2_parallelism, 2u);
    EXPECT_EQ(result.header.kdf.argon2_version, 0x13u);
    ASSERT_EQ(result.header.kdf.argon2_salt.size(), 32u);
    EXPECT_EQ(result.header.kdf.argon2_salt[0], 0x78);
    EXPECT_EQ(result.header.kdf.argon2_salt[31], 0x5F);

    // The header ends right after the end-of-header marker.
    EXPECT_EQ(result.header.payload_offset, 253u);
    EXPECT_EQ(result.header.raw_bytes.size(), 253u);
}

TEST(KdbxHeaderParserTest, AKdbx31HeaderParses) {
    const std::vector<std::uint8_t> bytes = buildKdbx31Header(false, 0xB54BFB66, 0x00030001);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok()) << result.error;

    EXPECT_EQ(result.header.version, KdbxVersion::kV3_1);
    EXPECT_EQ(result.header.cipher, KdbxCipher::kAes256Cbc);
    EXPECT_EQ(result.header.compression, KdbxCompression::kNone);
    // KDBX 3.1 keeps the AES-KDF parameters in the transform seed and rounds.
    EXPECT_EQ(result.header.kdf.kdf, KdbxKdf::kAesKdf);
    EXPECT_EQ(result.header.kdf.aes_rounds, 60000u);
    EXPECT_EQ(result.header.kdf.aes_salt[0], 0x5A);
    EXPECT_EQ(result.header.inner_random_stream_id, 2u);
    EXPECT_EQ(result.header.inner_random_stream_key[0], 0x22);
    EXPECT_EQ(result.header.stream_start_bytes[0], 0x33);
}

TEST(KdbxHeaderParserTest, TheFieldLengthWidthIsNotInterchangeable) {
    // The same header, read with the other version's framing, must not parse. If it
    // did, a 3.1 database would be read as a 4.x one (or the other way round) and the
    // failure would show up much later, as garbage in the payload.
    const std::vector<std::uint8_t> narrow = buildKdbx31Header(false, 0xB54BFB66, 0x00030001);
    const std::vector<std::uint8_t> wide = buildKdbx31Header(true, 0xB54BFB67, 0x00040000);

    EXPECT_TRUE(parseKdbxHeader(narrow.data(), narrow.size()).ok());
    EXPECT_FALSE(parseKdbxHeader(wide.data(), wide.size()).ok())
        << "a header with 4-byte field lengths must not parse as KDBX 3.1";
}

TEST(KdbxHeaderParserTest, AnUnknownFieldIsSkippedByItsLength) {
    // A field this build does not know must not stop the parser: that is what keeps a
    // newer database readable.
    std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    // Insert an unknown field (id 0x7F) with a four-byte payload right after the
    // signature and version, where the cipher field starts.
    const std::size_t insert_at = 12;
    std::vector<std::uint8_t> extra = {0x7F, 0x04, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insert_at), extra.begin(),
                 extra.end());

    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.header.cipher, KdbxCipher::kAes256Cbc);
    EXPECT_EQ(result.header.payload_offset, 253u + extra.size());
}

TEST(KdbxHeaderParserTest, AFileThatIsNotKdbxIsRejected) {
    const std::vector<std::uint8_t> bytes = fromHex("00112233445566778899aabbccddeeff");
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(KdbxHeaderParserTest, ATruncatedHeaderIsRejected) {
    const std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    for (std::size_t length = 0; length < bytes.size(); length += 37) {
        const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), length);
        EXPECT_FALSE(result.ok()) << "a header truncated to " << length << " bytes parsed";
    }
}

TEST(KdbxHeaderParserTest, AVersionAndSignatureThatDisagreeAreRejected) {
    // The 4.x signature with the 3.1 version: the file is one of the two, and we do
    // not get to guess which.
    const std::vector<std::uint8_t> bytes = buildKdbx31Header(false, 0xB54BFB67, 0x00030001);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, AnUnsupportedVersionIsRejected) {
    // KDBX 5 does not exist yet; a file that claims it must not be guessed at.
    std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    bytes[8] = 0x05;
    bytes[9] = 0x00;
    bytes[10] = 0x00;
    bytes[11] = 0x00;
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, AnUnknownCipherIsRejected) {
    std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    bytes[17] = 0xAA;  // the cipher uuid starts at offset 17
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, AnUnsupportedCompressionIsRejected) {
    std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    bytes[38] = 0x02;  // the compression flags start at offset 38
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, AMissingEndOfHeaderIsRejected) {
    // Cut the header right before the end-of-header field.
    const std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), 244);
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, AnUnknownKdfIsRejected) {
    // A KDBX 4 header whose KDF parameters name a key derivation function that this
    // build does not know. The uuid is the first entry of the variant dictionary.
    std::vector<std::uint8_t> bytes = fromHex(kRealKdbx4Header);
    bytes[121] = 0x00;  // the KDF uuid starts at offset 121
    const KdbxHeaderParseResult result = parseKdbxHeader(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok());
}

TEST(KdbxHeaderParserTest, TheNamesAreStable) {
    EXPECT_STREQ(toString(KdbxVersion::kV3_1), "3.1");
    EXPECT_STREQ(toString(KdbxVersion::kV4_0), "4.0");
    EXPECT_STREQ(toString(KdbxVersion::kV4_1), "4.1");
    EXPECT_STREQ(toString(KdbxCipher::kAes256Cbc), "AES-256-CBC");
    EXPECT_STREQ(toString(KdbxCipher::kChaCha20), "ChaCha20");
    EXPECT_STREQ(toString(KdbxCompression::kGzip), "gzip");
    EXPECT_STREQ(toString(KdbxKdf::kArgon2d), "Argon2d");
    EXPECT_STREQ(toString(KdbxKdf::kArgon2id), "Argon2id");
}

}  // namespace
}  // namespace openscrt::credential::kdbx
