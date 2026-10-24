// KDBX 3.1 reader tests (P2-T5).
//
// The fixture is a KDBX 3.1 database that this project generates with
// tools/make_kdbx_fixture.py, unlocked by a master password *and* a key file -- the
// combination the acceptance criterion asks for.
//
// The file is not trusted on the generator's word: the reference implementation
// (pykeepass) opens it and reads the entry back, and that check is recorded in the
// fixture's README. So the file is known to be a real KDBX 3.1 database, and this
// test checks that the reader agrees.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "credential/kdbx/Kdbx3Reader.h"

namespace openscrt::credential::kdbx {
namespace {

constexpr const char* kFixturePassword = "correct horse battery staple";

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

std::string fixturePath(const char* name) {
    return std::string(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/" + name;
}

std::string asText(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

TEST(Kdbx3ReaderTest, TheDatabaseWithAKeyFileOpens) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    ASSERT_FALSE(database.empty()) << "the fixture is missing";
    const std::vector<std::uint8_t> key_file = readFile(fixturePath("kdbx31_with_keyfile.key"));
    ASSERT_EQ(key_file.size(), 32u);

    const Kdbx3ReadResult result =
        readKdbx3(database.data(), database.size(), kFixturePassword, key_file);
    ASSERT_TRUE(result.ok()) << result.error;

    const std::string text = asText(result.payload);
    EXPECT_NE(text.find("<KeePassFile>"), std::string::npos) << text.substr(0, 200);
    EXPECT_NE(text.find("<Value>netops</Value>"), std::string::npos);
    EXPECT_NE(text.find("<Value>s3cret</Value>"), std::string::npos);
    EXPECT_NE(text.find("<Value>ssh://10.20.30.40:22</Value>"), std::string::npos);
}

TEST(Kdbx3ReaderTest, AWrongPasswordIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    const std::vector<std::uint8_t> key_file = readFile(fixturePath("kdbx31_with_keyfile.key"));
    ASSERT_FALSE(database.empty());

    const Kdbx3ReadResult result = readKdbx3(database.data(), database.size(), "wrong", key_file);
    EXPECT_FALSE(result.ok());
    // The reason has to be named. With a wrong password the padding check usually
    // fails first, so the message is about the payload not decrypting; when the
    // padding happens to be valid, it is the stream start bytes that catch it.
    EXPECT_NE(result.error.find("password"), std::string::npos) << result.error;
}

TEST(Kdbx3ReaderTest, AWrongKeyFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    ASSERT_FALSE(database.empty());

    const std::vector<std::uint8_t> wrong_key_file(32, 0x42);
    const Kdbx3ReadResult result =
        readKdbx3(database.data(), database.size(), kFixturePassword, wrong_key_file);
    EXPECT_FALSE(result.ok());
}

TEST(Kdbx3ReaderTest, AMissingKeyFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    ASSERT_FALSE(database.empty());

    const Kdbx3ReadResult result =
        readKdbx3(database.data(), database.size(), kFixturePassword, {});
    EXPECT_FALSE(result.ok());
}

TEST(Kdbx3ReaderTest, ADamagedFileIsReported) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    const std::vector<std::uint8_t> key_file = readFile(fixturePath("kdbx31_with_keyfile.key"));
    ASSERT_FALSE(database.empty());

    std::vector<std::uint8_t> damaged = database;
    damaged[damaged.size() / 2] = static_cast<std::uint8_t>(damaged[damaged.size() / 2] ^ 0xFF);

    const Kdbx3ReadResult result =
        readKdbx3(damaged.data(), damaged.size(), kFixturePassword, key_file);
    EXPECT_FALSE(result.ok());
}

TEST(Kdbx3ReaderTest, AKdbx4FileIsNotReadByThisReader) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("blank_database.kdbx"));
    ASSERT_FALSE(database.empty());

    const Kdbx3ReadResult result = readKdbx3(database.data(), database.size(), "password", {});
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("3.1"), std::string::npos) << result.error;
}

}  // namespace
}  // namespace openscrt::credential::kdbx
