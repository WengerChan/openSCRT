// KDBX 4.x reader tests (P2-T4).
//
// The test reads a real KeePass database (tests/fixtures/kdbx/blank_database.kdbx) and
// checks that the reader produces the same thing the reference implementation produces
// for it. Two independent implementations agreeing is evidence; this project agreeing
// with itself is not.
//
// The fixture is a blank database with the public password "password". It holds no
// entries and no secrets, and its provenance is written down in the fixture's README.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "credential/kdbx/Kdbx4Reader.h"

namespace openscrt::credential::kdbx {
namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

std::string fixturePath() {
    return std::string(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/blank_database.kdbx";
}

std::string asText(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

TEST(Kdbx4ReaderTest, TheRealDatabaseDecrypts) {
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty()) << "the fixture is missing: " << fixturePath();

    const Kdbx4ReadResult result = readKdbx4(database.data(), database.size(), "password", {});
    ASSERT_TRUE(result.ok()) << result.error;

    // The payload is the inner header followed by the XML. The values inside the XML
    // are protected by the inner random stream, but the XML itself is plain text.
    const std::string text = asText(result.payload);
    EXPECT_NE(text.find("<KeePassFile>"), std::string::npos) << text.substr(0, 200);
    EXPECT_NE(text.find("<Generator>KeePassXC</Generator>"), std::string::npos);
    // The root group of this database, base64 as the format stores uuids.
    EXPECT_NE(text.find("F69os494TDK6douojK65vQ=="), std::string::npos);
}

TEST(Kdbx4ReaderTest, AWrongPasswordIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty());

    const Kdbx4ReadResult result =
        readKdbx4(database.data(), database.size(), "not-the-password", {});
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
    // The failure has to name the reason, not just "it did not work".
    EXPECT_NE(result.error.find("password"), std::string::npos) << result.error;
}

TEST(Kdbx4ReadResultTest, AWrongKeyFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty());

    const std::vector<std::uint8_t> wrong_key_file(32, 0x42);
    const Kdbx4ReadResult result =
        readKdbx4(database.data(), database.size(), "password", wrong_key_file);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(Kdbx4ReaderTest, ADamagedFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty());

    // Flip a byte in the payload: the block HMAC has to catch it.
    std::vector<std::uint8_t> damaged = database;
    damaged[damaged.size() / 2] = static_cast<std::uint8_t>(damaged[damaged.size() / 2] ^ 0xFF);

    const Kdbx4ReadResult result = readKdbx4(damaged.data(), damaged.size(), "password", {});
    EXPECT_FALSE(result.ok());
}

TEST(Kdbx4ReaderTest, ATruncatedFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty());

    for (std::size_t length : {std::size_t{0}, std::size_t{10}, std::size_t{200},
                               database.size() / 2, database.size() - 1}) {
        const Kdbx4ReadResult result = readKdbx4(database.data(), length, "password", {});
        EXPECT_FALSE(result.ok()) << "a file truncated to " << length << " bytes was read";
    }
}

TEST(Kdbx4ReaderTest, AKdbx31FileIsNotReadByThisReader) {
    // The 3.1 format has a different header framing and a different payload layout;
    // this reader must say so rather than produce a plausible-looking result.
    const std::vector<std::uint8_t> database = readFile(fixturePath());
    ASSERT_FALSE(database.empty());

    std::vector<std::uint8_t> fake = database;
    fake[8] = 0x00;
    fake[9] = 0x03;
    fake[10] = 0x00;
    fake[11] = 0x01;  // the version 3.1
    fake[4] = 0x66;   // the second signature of 3.1
    fake[5] = 0xfb;
    fake[6] = 0x4b;
    fake[7] = 0xb6;

    const Kdbx4ReadResult result = readKdbx4(fake.data(), fake.size(), "password", {});
    EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace openscrt::credential::kdbx
