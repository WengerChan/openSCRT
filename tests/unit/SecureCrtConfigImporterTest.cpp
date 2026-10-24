// The SecureCRT importer (P2-T11).
//
// The session files SecureCRT writes are the fixture here: the shape was taken from a
// file that SecureCRT itself produced and checked against two independent readers (the
// exporter of RustConn and the importer of oryxis), so the test is about our reading of
// a format and not about a format we made up.

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config/SessionProfile.h"
#include "config/importers/SecureCrtConfigImporter.h"

namespace openscrt::config {
namespace {

namespace fs = std::filesystem;

// A session file as SecureCRT writes it: a byte order mark, then a flat list of typed
// values.
std::string sessionFile(const std::string& host, const std::string& protocol,
                        const std::string& port_key, const std::string& port,
                        const std::string& user) {
    std::string text = "\xEF\xBB\xBF";
    text += "S:\"Username\"=" + user + "\r\n";
    text += "S:\"Password V2\"=02:4ffff611d02e5d7f8c94f65b5ff9b89\r\n";
    text += "D:\"Session Password Saved\"=00000001\r\n";
    text += "D:\"Is Session\"=00000001\r\n";
    text += "S:\"Protocol Name\"=" + protocol + "\r\n";
    text += "S:\"Hostname\"=" + host + "\r\n";
    text += "D:\"" + port_key + "\"=" + port + "\r\n";
    text += "S:\"Identity Filename V2\"=\r\n";
    text += "D:\"ANSI Color\"=00000000\r\n";
    text += "S:\"Emulation\"=VT100\r\n";
    return text;
}

void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// Every string a profile carries, so that a secret cannot hide in one of them.
std::string allText(const config::SessionProfile& profile) {
    std::string text;
    text += profile.host.value_or("");
    text += profile.user.value_or("");
    text += profile.private_key_path.value_or("");
    text += profile.credential_source.value_or("");
    text += profile.credential_entry_uuid.value_or("");
    text += profile.credential_match_host.value_or("");
    return text;
}

class SecureCrtImporterTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        root_ = fs::path(dir_.path().toStdString());
    }

    // The folder of the Session Manager, with one folder of sessions in it.
    void buildTree() {
        const fs::path site = root_ / "CP_CDR_MC_Site";
        fs::create_directories(site);

        writeFile(site / "CMCSSW1.ini",
                  sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));
        writeFile(site / "CMCSFW1.ini",
                  sessionFile("172.10.3.3", "SSH2", "[SSH2] Port", "0000b3a", "root"));
        writeFile(site / "__FolderData__.ini",
                  "S:\"Folder List\"=\r\nS:\"Session List\"=CMCSFW1:CMCSSW1:\r\n"
                  "D:\"Is Expanded\"=00000001\r\n");
    }

    const config::SessionProfile* find(const config::SecureCrtImportResult& result,
                                       const std::string& id) {
        for (const config::SessionProfile& profile : result.sessions) {
            if (profile.id == id) {
                return &profile;
            }
        }
        return nullptr;
    }

    QTemporaryDir dir_;
    fs::path root_;
};

TEST_F(SecureCrtImporterTest, ASessionFileBecomesAProfile) {
    writeFile(root_ / "one.ini",
              sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.sessions.size(), 1u);

    const config::SessionProfile& profile = result.sessions.front();
    EXPECT_EQ(profile.id, "one");
    EXPECT_EQ(profile.host.value_or(""), "172.10.3.2");
    EXPECT_EQ(profile.user.value_or(""), "admin");
    EXPECT_EQ(profile.port.value_or(0), 22);
    EXPECT_EQ(profile.protocol.value_or(config::Protocol::kSsh2), config::Protocol::kSsh2);
}

TEST_F(SecureCrtImporterTest, ThePortIsReadAsHex) {
    // 00000016 is 22 in hex. Read as a decimal it would be 16, and read as a hex
    // number without the leading zeros it would be 0x16 the second time.
    writeFile(root_ / "one.ini",
              sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 1u);
    EXPECT_EQ(result.sessions.front().port.value_or(0), 22);
}

TEST_F(SecureCrtImporterTest, APortThatIsNotANumberIsNotRead) {
    // A value that is not eight hex digits is not a number SecureCRT wrote. Reading
    // "22" as hex would give 34, which is worse than not reading it at all.
    writeFile(root_ / "one.ini", sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "22", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 1u);
    EXPECT_EQ(result.sessions.front().port.value_or(0), 22) << "the default of the protocol";
}

TEST_F(SecureCrtImporterTest, ATelnetSessionKeepsItsPort) {
    writeFile(root_ / "console.ini", sessionFile("10.0.0.9", "TELNET", "Port", "00000017", "ops"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 1u);
    EXPECT_EQ(result.sessions.front().protocol.value_or(config::Protocol::kSsh2),
              config::Protocol::kTelnet);
    EXPECT_EQ(result.sessions.front().port.value_or(0), 23);
}

TEST_F(SecureCrtImporterTest, AFolderBecomesAGroupAndItsSessionsHangUnderIt) {
    buildTree();

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.sessions.size(), 3u) << "the group and its two sessions";

    const config::SessionProfile* group = find(result, "CP_CDR_MC_Site");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->name, "CP_CDR_MC_Site");
    EXPECT_TRUE(group->parent_id.empty());

    const config::SessionProfile* session = find(result, "CP_CDR_MC_Site.CMCSSW1");
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->parent_id, "CP_CDR_MC_Site");
    EXPECT_EQ(session->host.value_or(""), "172.10.3.2");
}

TEST_F(SecureCrtImporterTest, TheGroupComesBeforeItsSessions) {
    buildTree();

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 3u);

    // The order has to let the sessions be added to a tree one after another: a session
    // whose parent is not there yet is refused.
    std::size_t group_at = 0;
    std::size_t session_at = 0;
    for (std::size_t i = 0; i < result.sessions.size(); ++i) {
        if (result.sessions[i].id == "CP_CDR_MC_Site") {
            group_at = i;
        }
        if (result.sessions[i].id == "CP_CDR_MC_Site.CMCSSW1") {
            session_at = i;
        }
    }
    EXPECT_LT(group_at, session_at);

    SessionTree tree;
    for (const config::SessionProfile& profile : result.sessions) {
        EXPECT_TRUE(tree.add(profile)) << profile.id;
    }
    EXPECT_EQ(tree.size(), 3u);
}

TEST_F(SecureCrtImporterTest, ASessionWithoutAHostIsSkipped) {
    const fs::path site = root_ / "CP_CDR_MC_Site";
    fs::create_directories(site);
    writeFile(site / "broken.ini", "D:\"Is Session\"=00000001\r\nS:\"Protocol Name\"=SSH2\r\n");

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    EXPECT_EQ(result.sessions.size(), 1u) << "only the folder itself is imported";
    ASSERT_EQ(result.skipped.size(), 1u);
    EXPECT_EQ(result.skipped.front().name, "broken");
    EXPECT_FALSE(result.skipped.front().reason.empty()) << "a refusal has to say why";
}

TEST_F(SecureCrtImporterTest, AProtocolThisBuildDoesNotSpeakIsSkipped) {
    const fs::path site = root_ / "CP_CDR_MC_Site";
    fs::create_directories(site);
    writeFile(site / "rdp.ini", sessionFile("10.0.0.9", "RDP", "Port", "00000338", "ops"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    EXPECT_EQ(result.sessions.size(), 1u) << "only the folder itself is imported";
    ASSERT_EQ(result.skipped.size(), 1u);
    EXPECT_NE(result.skipped.front().reason.find("RDP"), std::string::npos);
}

TEST_F(SecureCrtImporterTest, TheEncryptedPasswordIsNotBroughtOver) {
    writeFile(root_ / "one.ini",
              sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 1u);
    // The profile carries no password at all: there is nowhere in it for one to be.
    EXPECT_EQ(allText(result.sessions.front()).find("02:4ffff611"), std::string::npos)
        << "the encrypted password must not end up in the profile";
    ASSERT_FALSE(result.notes.empty()) << "the user is told that a password was left behind";
    EXPECT_NE(result.notes.front().find("encrypted"), std::string::npos);
}

TEST_F(SecureCrtImporterTest, ASessionIsFoundWithoutAFolderDataFile) {
    // Not every folder has a __FolderData__.ini. The directory is walked as well, so a
    // session in such a folder is still found.
    const fs::path site = root_ / "Loose";
    fs::create_directories(site);
    writeFile(site / "one.ini",
              sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    ASSERT_EQ(result.sessions.size(), 2u) << "the folder and the session in it";
    ASSERT_NE(find(result, "Loose.one"), nullptr);
}

TEST_F(SecureCrtImporterTest, ASessionThatIsNotThereIsReported) {
    const fs::path site = root_ / "CP_CDR_MC_Site";
    fs::create_directories(site);
    writeFile(site / "__FolderData__.ini", "S:\"Folder List\"=\r\nS:\"Session List\"=CMCSSW1:\r\n");

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(root_.string());
    EXPECT_EQ(result.sessions.size(), 1u) << "the folder itself is imported";
    ASSERT_NE(find(result, "CP_CDR_MC_Site"), nullptr);
    ASSERT_EQ(result.skipped.size(), 1u);
    EXPECT_EQ(result.skipped.front().name, "CMCSSW1");
}

TEST_F(SecureCrtImporterTest, ADirectoryThatIsNotThereIsAnError) {
    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree((root_ / "nowhere").string());
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(SecureCrtImporterTest, OneSessionFileCanBeImportedOnItsOwn) {
    const fs::path file = root_ / "one.ini";
    writeFile(file, sessionFile("172.10.3.2", "SSH2", "[SSH2] Port", "00000016", "admin"));

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importSession(file.string());
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.sessions.size(), 1u);
    EXPECT_EQ(result.sessions.front().id, "one");
    EXPECT_EQ(result.sessions.front().host.value_or(""), "172.10.3.2");
}

}  // namespace
}  // namespace openscrt::config
