// KDBX XML index and protected stream tests (P2-T6).
//
// The point of the index is that it does *not* decrypt anything: an entry's password
// stays in the form the database stored it, and only the session that needs it asks
// for the plaintext. The test therefore checks both halves -- the index keeps the
// password protected, and asking for it yields the right plaintext.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "credential/kdbx/Kdbx3Reader.h"
#include "credential/kdbx/Kdbx4Reader.h"
#include "credential/kdbx/KdbxProtectedStream.h"
#include "credential/kdbx/KdbxXmlIndex.h"

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

TEST(KdbxXmlIndexTest, TheIndexKeepsThePasswordProtected) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    const std::vector<std::uint8_t> key_file = readFile(fixturePath("kdbx31_with_keyfile.key"));
    ASSERT_FALSE(database.empty());

    const Kdbx3ReadResult read =
        readKdbx3(database.data(), database.size(), kFixturePassword, key_file);
    ASSERT_TRUE(read.ok()) << read.error;

    const KdbxIndexResult indexed =
        indexKdbxXml(read.payload.data() + read.xml_offset, read.payload.size() - read.xml_offset);
    ASSERT_TRUE(indexed.ok()) << indexed.error;
    ASSERT_EQ(indexed.index.entries.size(), 1u);

    const KdbxEntry& entry = indexed.index.entries.front();
    EXPECT_EQ(entry.title, "router");
    EXPECT_EQ(entry.username, "netops");
    EXPECT_EQ(entry.url, "ssh://10.20.30.40:22");
    EXPECT_EQ(entry.group_path, "Root");
    EXPECT_TRUE(entry.password_is_protected);
    // The stored value is the ciphertext, so the plaintext must not be in it.
    EXPECT_EQ(entry.password.find("s3cret"), std::string::npos);
    EXPECT_FALSE(entry.password.empty());
}

TEST(KdbxXmlIndexTest, ThePasswordIsRevealedOnRequest) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    const std::vector<std::uint8_t> key_file = readFile(fixturePath("kdbx31_with_keyfile.key"));
    ASSERT_FALSE(database.empty());

    const Kdbx3ReadResult read =
        readKdbx3(database.data(), database.size(), kFixturePassword, key_file);
    ASSERT_TRUE(read.ok()) << read.error;

    const KdbxIndexResult indexed =
        indexKdbxXml(read.payload.data() + read.xml_offset, read.payload.size() - read.xml_offset);
    ASSERT_TRUE(indexed.ok()) << indexed.error;
    ASSERT_EQ(indexed.index.entries.size(), 1u);

    const KdbxProtectedStream stream(read.protected_stream_id, read.protected_stream_key.data(),
                                     read.protected_stream_key.size());
    EXPECT_EQ(read.protected_stream_id, 2u) << "the fixture uses the Salsa20 protected stream";

    core::SecureByteArray password;
    ASSERT_TRUE(revealKdbxPassword(indexed.index.entries.front(), stream, password))
        << "the password could not be revealed";
    ASSERT_EQ(password.size(), 6u);
    char buffer[7] = {};
    EXPECT_EQ(password.copyOut(buffer, sizeof(buffer)), 6u);
    EXPECT_STREQ(buffer, "s3cret");
}

TEST(KdbxXmlIndexTest, TheKdbx4InnerHeaderCarriesTheProtectedStream) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("blank_database.kdbx"));
    ASSERT_FALSE(database.empty());

    const Kdbx4ReadResult read = readKdbx4(database.data(), database.size(), "password", {});
    ASSERT_TRUE(read.ok()) << read.error;
    // The inner header sits in front of the XML.
    EXPECT_GT(read.xml_offset, 0u);
    EXPECT_LT(read.xml_offset, read.payload.size());
    // The blank database uses the ChaCha20 protected stream.
    EXPECT_EQ(read.protected_stream_id, 3u);
    EXPECT_FALSE(read.protected_stream_key.empty());

    const KdbxIndexResult indexed =
        indexKdbxXml(read.payload.data() + read.xml_offset, read.payload.size() - read.xml_offset);
    ASSERT_TRUE(indexed.ok()) << indexed.error;
    // The blank database has no entries, but it does have a root group.
    EXPECT_TRUE(indexed.index.entries.empty());
    EXPECT_FALSE(indexed.index.root_group_uuid.empty());
}

TEST(KdbxXmlIndexTest, MalformedXmlIsRejected) {
    const std::string broken = "<KeePassFile><Root><Group>";
    const KdbxIndexResult indexed =
        indexKdbxXml(reinterpret_cast<const std::uint8_t*>(broken.data()), broken.size());
    EXPECT_FALSE(indexed.ok());
    EXPECT_FALSE(indexed.error.empty());
}

TEST(KdbxXmlIndexTest, AnEmptyPayloadIsRejected) {
    const KdbxIndexResult indexed = indexKdbxXml(nullptr, 0);
    EXPECT_FALSE(indexed.ok());
}

}  // namespace
}  // namespace openscrt::credential::kdbx
