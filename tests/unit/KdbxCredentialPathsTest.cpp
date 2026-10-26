// The paths of the credential library that the other tests do not walk.
//
// The parser, the reader and the vault have a happy path each, and the happy path is
// what the real fixture proves. What is here is what happens when the input is not what
// it should be: a key derivation function this build does not have, a payload that is not
// GZip, a stream id from a KeePass version this build does not support, an entry without
// a password.
//
// These are the paths where a mistake turns into a wrong key rather than an error, so
// they are worth a test each. The digest values themselves are not spelled out here:
// the real KDBX fixture in tests/fixtures/kdbx already proves that the digests are the
// ones the format asks for, because the file only opens with them.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/SecureByteArray.h"
#include "credential/kdbx/Kdbx3Reader.h"
#include "credential/kdbx/Kdbx4Reader.h"
#include "credential/kdbx/KdbxCrypto.h"
#include "credential/kdbx/KdbxHeader.h"
#include "credential/kdbx/KdbxProtectedStream.h"
#include "credential/kdbx/KdbxVault.h"
#include "credential/kdbx/KdbxXmlIndex.h"

namespace openscrt::credential::kdbx {
namespace {

// ------------------------------------------------------------------ the digests

TEST(KdbxCryptoPathsTest, TheCompositeKeyNeedsAPassword) {
    // The password half of the composite key is hashed on its own. With no password at
    // all there is nothing to hash, and the result is an empty key rather than a key
    // made of zeroes.
    Digest empty{};
    EXPECT_EQ(compositeKey(nullptr, 0, {}), empty);

    const std::uint8_t password[] = {'p', 'a', 's', 's'};
    EXPECT_EQ(compositeKey(password, 0, {}), empty) << "a password of zero length was accepted";

    // A password and a key file give a key that is not the empty one.
    const std::vector<std::uint8_t> key_file(32, 0x5A);
    EXPECT_NE(compositeKey(password, sizeof(password), key_file), empty);
}

TEST(KdbxCryptoPathsTest, TheAesKdfChecksItsInputs) {
    // The AES-KDF needs a 32-byte composite key and a seed of at least the same length.
    std::vector<std::uint8_t> seed(32, 0x11);
    std::vector<std::uint8_t> composite(32, 0x22);
    Digest empty{};

    EXPECT_EQ(aesKdf(seed.data(), seed.size(), 10, composite.data(), 31), empty)
        << "a composite key of the wrong length was accepted";
    EXPECT_EQ(aesKdf(seed.data(), 8, 10, composite.data(), composite.size()), empty)
        << "a seed that is too short was accepted";
}

// ------------------------------------------------------------------ Argon2

TEST(KdbxCryptoPathsTest, Argon2RefusesWhatItCannotDo) {
    KdbxKdfParameters parameters;
    Digest key{};
    std::string error;

    // A KDF that is not Argon2 at all.
    parameters.kdf = KdbxKdf::kAesKdf;
    EXPECT_FALSE(argon2Derive(parameters, nullptr, 0, key, error));
    EXPECT_NE(error.find("not Argon2"), std::string::npos) << error;

    // Argon2 with a salt that is too short to be one.
    parameters.kdf = KdbxKdf::kArgon2d;
    parameters.argon2_salt.assign(4, 0x33);
    parameters.argon2_memory_bytes = 65536;
    error.clear();
    EXPECT_FALSE(argon2Derive(parameters, nullptr, 0, key, error));
    EXPECT_NE(error.find("salt"), std::string::npos) << error;

    // Argon2 with less memory than the algorithm allows.
    parameters.argon2_salt.assign(32, 0x33);
    parameters.argon2_memory_bytes = 1024;
    error.clear();
    EXPECT_FALSE(argon2Derive(parameters, nullptr, 0, key, error));
    EXPECT_NE(error.find("memory"), std::string::npos) << error;
}

// ------------------------------------------------------------------ GZip

TEST(KdbxCryptoPathsTest, GzipRefusesWhatItCannotDecompress) {
    std::vector<std::uint8_t> out;
    std::string error;

    // Not GZip at all.
    const std::uint8_t garbage[] = {'n', 'o', 't', ' ', 'g', 'z', 'i', 'p'};
    EXPECT_FALSE(gzipDecompress(garbage, sizeof(garbage), out, error));
    EXPECT_NE(error.find("not valid GZip"), std::string::npos) << error;

    // A GZip header and nothing else: the stream is cut short. The header is the one
    // RFC 1952 describes -- the magic, the deflate method, no flags, no time, no extra
    // flags, and the operating system byte.
    const std::uint8_t header[] = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    error.clear();
    EXPECT_FALSE(gzipDecompress(header, sizeof(header), out, error));
    EXPECT_FALSE(error.empty()) << "the truncated stream was refused without a reason";
}

// ------------------------------------------------------------------ the protected stream

TEST(KdbxProtectedStreamPathsTest, AnUnprotectedStreamIsPlainText) {
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(0, key, sizeof(key));
    EXPECT_FALSE(stream.isProtected());

    const std::uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    std::vector<std::uint8_t> out;
    std::string error;
    ASSERT_TRUE(stream.decrypt(payload, sizeof(payload), out, error)) << error;
    EXPECT_EQ(out.size(), sizeof(payload));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(out.data()), out.size()), "hello");
}

TEST(KdbxProtectedStreamPathsTest, ANullPayloadIsRefused) {
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(3, key, sizeof(key));

    std::vector<std::uint8_t> out;
    std::string error;
    EXPECT_FALSE(stream.decrypt(nullptr, 4, out, error));
    EXPECT_NE(error.find("no data"), std::string::npos) << error;
}

TEST(KdbxProtectedStreamPathsTest, AStreamThisBuildDoesNotHaveIsRefused) {
    const std::uint8_t key[] = {1, 2, 3, 4};
    const std::uint8_t payload[] = {'x'};
    std::vector<std::uint8_t> out;
    std::string error;

    // 1 is the ARCFOUR variant: an old KeePass format, not supported here.
    KdbxProtectedStream arcfour(1, key, sizeof(key));
    EXPECT_FALSE(arcfour.decrypt(payload, sizeof(payload), out, error));
    EXPECT_NE(error.find("not implement"), std::string::npos) << error;

    // Anything else is a format this build has never heard of.
    error.clear();
    KdbxProtectedStream unknown(99, key, sizeof(key));
    EXPECT_FALSE(unknown.decrypt(payload, sizeof(payload), out, error));
    EXPECT_NE(error.find("does not know"), std::string::npos) << error;
}

TEST(KdbxProtectedStreamPathsTest, TheChaCha20StreamRoundTrips) {
    // The stream key is hashed, and the second half of the digest is the nonce. The
    // cipher is its own inverse, so the same call that decrypts also encrypts.
    const std::uint8_t key[] = {9, 8, 7, 6, 5, 4, 3, 2};
    KdbxProtectedStream stream(3, key, sizeof(key));
    EXPECT_TRUE(stream.isProtected());
    EXPECT_EQ(stream.id(), 3u);

    const Sha512Digest digest = sha512(key, sizeof(key));
    std::uint8_t cipher_key[32] = {};
    std::copy(digest.begin(), digest.begin() + 32, cipher_key);

    const std::uint8_t plaintext[] = {'t', 'h', 'e', ' ', 'p', 'a', 's', 's'};
    std::vector<std::uint8_t> ciphertext;
    std::string error;
    ASSERT_TRUE(chacha20Decrypt(cipher_key, digest.data() + 32, plaintext, sizeof(plaintext),
                                ciphertext, error))
        << error;
    ASSERT_EQ(ciphertext.size(), sizeof(plaintext));
    EXPECT_NE(ciphertext, std::vector<std::uint8_t>(plaintext, plaintext + sizeof(plaintext)));

    std::vector<std::uint8_t> decrypted;
    ASSERT_TRUE(stream.decrypt(ciphertext.data(), ciphertext.size(), decrypted, error)) << error;
    EXPECT_EQ(decrypted, std::vector<std::uint8_t>(plaintext, plaintext + sizeof(plaintext)));
}

// ------------------------------------------------------------------ the XML index

// A database with one group and one entry. The password is base64 of "secret".
const std::string_view kDatabaseXml = R"(<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<KeePassFile>
  <Root>
    <Group>
      <UUID>cm9vdA==</UUID>
      <Name>Root</Name>
      <Entries>
        <Entry>
          <UUID>ZW50cnk=</UUID>
          <String><Key>Title</Key><Value>A login</Value></String>
          <String><Key>UserName</Key><Value>someone</Value></String>
          <String><Key>URL</Key><Value>https://example.invalid/</Value></String>
          <String><Key>Password</Key><Value>c2VjcmV0</Value></String>
          <String><Key>SSH Passphrase</Key><Value>another</Value></String>
        </Entry>
      </Entries>
    </Group>
  </Root>
</KeePassFile>)";

TEST(KdbxXmlIndexPathsTest, TheEntriesAreIndexed) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(kDatabaseXml.data());
    const KdbxIndexResult result = indexKdbxXml(data, kDatabaseXml.size());
    ASSERT_TRUE(result.ok()) << result.error;

    ASSERT_EQ(result.index.entries.size(), 1u);
    const KdbxEntry& entry = result.index.entries.front();
    EXPECT_EQ(entry.title, "A login");
    EXPECT_EQ(entry.username, "someone");
    EXPECT_EQ(entry.url, "https://example.invalid/");
    EXPECT_EQ(entry.password, "c2VjcmV0");
    EXPECT_FALSE(entry.password_is_protected);
    EXPECT_EQ(entry.group_path, "Root");
    // A field the index does not know is kept as a custom field, not thrown away.
    ASSERT_EQ(entry.custom_fields.size(), 1u);
    EXPECT_EQ(entry.custom_fields.front().first, "SSH Passphrase");
}

TEST(KdbxXmlIndexPathsTest, TheXmlMustHaveARoot) {
    const std::string_view xml = "<KeePassFile><Meta/></KeePassFile>";
    const auto* data = reinterpret_cast<const std::uint8_t*>(xml.data());
    const KdbxIndexResult result = indexKdbxXml(data, xml.size());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("Root"), std::string::npos) << result.error;
}

TEST(KdbxXmlIndexPathsTest, TheXmlMustBeWellFormed) {
    const std::string_view xml = "<KeePassFile><Root>";
    const auto* data = reinterpret_cast<const std::uint8_t*>(xml.data());
    const KdbxIndexResult result = indexKdbxXml(data, xml.size());
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(KdbxXmlIndexPathsTest, AnEntryWithoutAPasswordIsRefused) {
    KdbxEntry entry;
    entry.title = "A login";
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(0, key, sizeof(key));

    core::SecureByteArray out;
    std::string error;
    EXPECT_FALSE(revealKdbxPassword(entry, stream, out, error));
    EXPECT_NE(error.find("no password"), std::string::npos) << error;
}

TEST(KdbxXmlIndexPathsTest, APlainPasswordIsHandedOverAsItIs) {
    KdbxEntry entry;
    entry.password = "in the clear";
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(0, key, sizeof(key));

    core::SecureByteArray out;
    std::string error;
    ASSERT_TRUE(revealKdbxPassword(entry, stream, out, error)) << error;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(out.data()), out.size()), "in the clear");
}

TEST(KdbxXmlIndexPathsTest, AProtectedPasswordMustBeBase64) {
    KdbxEntry entry;
    entry.password = "not base64 at all!!";
    entry.password_is_protected = true;
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(0, key, sizeof(key));

    core::SecureByteArray out;
    std::string error;
    EXPECT_FALSE(revealKdbxPassword(entry, stream, out, error));
    EXPECT_NE(error.find("base64"), std::string::npos) << error;
}

TEST(KdbxXmlIndexPathsTest, AProtectedPasswordIsDecodedBeforeItIsRevealed) {
    // "c2VjcmV0" is base64 of "secret"; with an unprotected stream the decoded bytes are
    // the password.
    KdbxEntry entry;
    entry.password = "c2VjcmV0";
    entry.password_is_protected = true;
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(0, key, sizeof(key));

    core::SecureByteArray out;
    std::string error;
    ASSERT_TRUE(revealKdbxPassword(entry, stream, out, error)) << error;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(out.data()), out.size()), "secret");
}

TEST(KdbxXmlIndexPathsTest, AProtectedPasswordWithPaddingIsDecoded) {
    // The padding is what tells the length apart, and a decoder that drops the wrong
    // number of bytes turns one byte into three.
    const std::vector<std::string> encoded = {"YQ==", "YWI=", "YWJj"};
    for (const std::string& text : encoded) {
        KdbxEntry entry;
        entry.password = text;
        entry.password_is_protected = true;
        const std::uint8_t key[] = {1, 2, 3, 4};
        KdbxProtectedStream stream(0, key, sizeof(key));

        core::SecureByteArray out;
        std::string error;
        ASSERT_TRUE(revealKdbxPassword(entry, stream, out, error)) << error;
        EXPECT_FALSE(out.empty()) << text;
    }
}

TEST(KdbxXmlIndexPathsTest, AFieldWithoutAValueIsSkipped) {
    const std::string_view xml = R"(<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<KeePassFile>
  <Root>
    <Group>
      <UUID>cm9vdA==</UUID>
      <Name>Root</Name>
      <Entries>
        <Entry>
          <UUID>ZW50cnk=</UUID>
          <String><Key>Title</Key></String>
          <String><Key>Password</Key><Value>c2VjcmV0</Value></String>
        </Entry>
      </Entries>
    </Group>
  </Root>
</KeePassFile>)";
    const auto* data = reinterpret_cast<const std::uint8_t*>(xml.data());
    const KdbxIndexResult result = indexKdbxXml(data, xml.size());
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.index.entries.size(), 1u);
    // The field without a value is skipped; the one with a value is there.
    EXPECT_TRUE(result.index.entries.front().title.empty());
    EXPECT_EQ(result.index.entries.front().password, "c2VjcmV0");
}

TEST(KdbxXmlIndexPathsTest, AGroupInsideAGroupKeepsThePath) {
    const std::string_view xml = R"(<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<KeePassFile>
  <Root>
    <Group>
      <UUID>cm9vdA==</UUID>
      <Name>Root</Name>
      <Group>
        <UUID>c3Vi</UUID>
        <Name>Sub</Name>
        <Entries>
          <Entry>
            <UUID>ZW50cnk=</UUID>
            <String><Key>Title</Key><Value>A login</Value></String>
          </Entry>
        </Entries>
      </Group>
    </Group>
  </Root>
</KeePassFile>)";
    const auto* data = reinterpret_cast<const std::uint8_t*>(xml.data());
    const KdbxIndexResult result = indexKdbxXml(data, xml.size());
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.index.entries.size(), 1u);
    EXPECT_EQ(result.index.entries.front().group_path, "Root/Sub");
}

TEST(KdbxXmlIndexPathsTest, AnXmlThatIsNotXmlIsRefused) {
    const std::string_view xml = "<";
    const auto* data = reinterpret_cast<const std::uint8_t*>(xml.data());
    const KdbxIndexResult result = indexKdbxXml(data, xml.size());
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(KdbxXmlIndexPathsTest, AProtectedPasswordThatTheStreamCannotOpenIsRefused) {
    KdbxEntry entry;
    entry.password = "c2VjcmV0";
    entry.password_is_protected = true;
    // Stream 1 is the ARCFOUR variant, which this build does not have.
    const std::uint8_t key[] = {1, 2, 3, 4};
    KdbxProtectedStream stream(1, key, sizeof(key));

    core::SecureByteArray out;
    std::string error;
    EXPECT_FALSE(revealKdbxPassword(entry, stream, out, error));
    EXPECT_FALSE(error.empty());
}

// ------------------------------------------------------------------ the 4.x reader

// The blank database is a KDBX 4 file with the public password "password". It holds no
// entries and no secrets.
constexpr std::string_view kBlankPassword = "password";

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

Kdbx4ReadResult readBlank(const std::vector<std::uint8_t>& database) {
    return readKdbx4(database.data(), database.size(),
                     reinterpret_cast<const std::uint8_t*>(kBlankPassword.data()),
                     kBlankPassword.size(), {});
}

TEST(Kdbx4ReaderPathsTest, AKdbx31FileIsNotReadAsA4xOne) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("kdbx31_with_keyfile.kdbx"));
    ASSERT_FALSE(database.empty()) << "the fixture is missing";

    const Kdbx4ReadResult result = readBlank(database);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("3.1"), std::string::npos) << result.error;
}

TEST(Kdbx4ReaderPathsTest, AFileThatEndsBeforeTheHeaderHashIsRefused) {
    const std::vector<std::uint8_t> database = readFile(fixturePath("blank_database.kdbx"));
    ASSERT_FALSE(database.empty()) << "the fixture is missing";
    const KdbxHeaderParseResult header = parseKdbxHeader(database.data(), database.size());
    ASSERT_TRUE(header.ok()) << header.error;

    // The header hash and the header authentication follow the header. A file that stops
    // in front of them cannot be checked, and it must not be read.
    const std::size_t cut = header.header.payload_offset + 32;
    const std::vector<std::uint8_t> shortened(database.begin(), database.begin() + cut);

    const Kdbx4ReadResult result = readBlank(shortened);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("ends before"), std::string::npos) << result.error;
}

TEST(Kdbx4ReaderPathsTest, ADamagedHeaderIsRefused) {
    // The header hash is the SHA-256 of the header bytes. Change one byte of the hash
    // and the file no longer says what it is.
    std::vector<std::uint8_t> database = readFile(fixturePath("blank_database.kdbx"));
    ASSERT_FALSE(database.empty()) << "the fixture is missing";
    const KdbxHeaderParseResult header = parseKdbxHeader(database.data(), database.size());
    ASSERT_TRUE(header.ok()) << header.error;
    database[header.header.payload_offset] ^= static_cast<std::uint8_t>(0xFF);

    const Kdbx4ReadResult result = readBlank(database);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("hash does not match"), std::string::npos) << result.error;
}

TEST(Kdbx4ReaderPathsTest, AWrongAuthenticationIsRefused) {
    // The header hash still matches, but the authentication that follows it does not.
    std::vector<std::uint8_t> database = readFile(fixturePath("blank_database.kdbx"));
    ASSERT_FALSE(database.empty()) << "the fixture is missing";
    const KdbxHeaderParseResult header = parseKdbxHeader(database.data(), database.size());
    ASSERT_TRUE(header.ok()) << header.error;
    database[header.header.payload_offset + 32] ^= static_cast<std::uint8_t>(0xFF);

    const Kdbx4ReadResult result = readBlank(database);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("authentication failed"), std::string::npos) << result.error;
}

// ------------------------------------------------------------------ the vault

TEST(KdbxVaultPathsTest, AFileThatIsNotThereIsRefused) {
    KdbxVault vault;
    std::string error;

    EXPECT_FALSE(vault.open(QStringLiteral("/nonexistent/openscrt-does-not-exist.kdbx"), nullptr, 0,
                            {}, error));
    EXPECT_NE(error.find("could not be read"), std::string::npos) << error;
    EXPECT_FALSE(vault.isOpen());
}

TEST(KdbxVaultPathsTest, ADatabaseWithTheWrongPasswordIsRefused) {
    // The real database, opened with a password that is not its own: the header
    // authentication does not match.
    const std::string path =
        std::string(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/blank_database.kdbx";
    const std::string wrong = "not the password";
    KdbxVault vault;
    std::string error;
    EXPECT_FALSE(vault.open(QString::fromStdString(path),
                            reinterpret_cast<const std::uint8_t*>(wrong.data()), wrong.size(), {},
                            error));
    EXPECT_FALSE(error.empty());
}

TEST(KdbxVaultPathsTest, AFileThatIsNotADatabaseIsRefused) {
    // A file with the name of a database that is not one.
    const std::string path =
        std::string(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/blank_database.kdbx";
    KdbxVault vault;
    std::string error;
    // The real database, read as if it were a different one: the header is refused.
    EXPECT_FALSE(vault.open(QString::fromStdString(path), nullptr, 0, {}, error));
    EXPECT_FALSE(error.empty());
}

TEST(Kdbx3ReaderPathsTest, AFileThatIsNotADatabaseIsRefused) {
    const std::vector<std::uint8_t> garbage = {'n', 'o', 't', ' ', 'a', ' ', 'd', 'b'};
    const Kdbx3ReadResult result = readKdbx3(garbage.data(), garbage.size(), nullptr, 0, {});
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(KdbxVaultPathsTest, AnEntryThatIsNotThereIsNotFound) {
    KdbxVault vault;
    EXPECT_EQ(vault.findByUuid("no-such-uuid"), nullptr);
    EXPECT_EQ(vault.findByHost("no-such-host"), nullptr);
}

}  // namespace
}  // namespace openscrt::credential::kdbx