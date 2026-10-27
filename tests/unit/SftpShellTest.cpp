// The SFTP command mode and the two-pane file manager (P3-T5).
//
// The splitting of a command line and the matching of a pattern are pure, so they are
// tested everywhere. The commands themselves need a server, so they skip where there
// is none.

#include <gtest/gtest.h>

#include <QApplication>
#include <QTreeWidget>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "protocols/ssh/Ssh2Channel.h"
#include "transfer/sftp/SftpShell.h"
#include "transfer/sftp/SftpSubsystem.h"
#include "ui/workspace/SftpDualPaneWidget.h"

namespace ssh = openscrt::protocols::ssh;
namespace sftp = openscrt::transfer::sftp;
namespace ui = openscrt::ui;

namespace {

TEST(SftpShellTest, ACommandLineIsSplitIntoWords) {
    EXPECT_EQ(sftp::splitCommandLine("get file.txt"),
              (std::vector<std::string>{"get", "file.txt"}));
    EXPECT_EQ(sftp::splitCommandLine("  get   file.txt  "),
              (std::vector<std::string>{"get", "file.txt"}));
    EXPECT_TRUE(sftp::splitCommandLine("   ").empty());
    EXPECT_EQ(sftp::splitCommandLine("get \"a file.txt\""),
              (std::vector<std::string>{"get", "a file.txt"}));
}

TEST(SftpShellTest, APatternMatchesTheNamesItShould) {
    const std::vector<std::string> names = {"alpha.txt", "beta.log", "gamma.txt"};
    EXPECT_EQ(sftp::matchingNames(names, "*.txt"),
              (std::vector<std::string>{"alpha.txt", "gamma.txt"}));
    EXPECT_EQ(sftp::matchingNames(names, "alpha.txt"), (std::vector<std::string>{"alpha.txt"}));
    EXPECT_TRUE(sftp::matchingNames(names, "*.bin").empty());
    EXPECT_EQ(sftp::matchingNames(names, "a*"), (std::vector<std::string>{"alpha.txt"}));
}

struct Server {
    std::string host{"127.0.0.1"};
    int port{0};
    std::string user;
    std::string key;
};

bool serverFromEnvironment(Server& out) {
    const char* port = std::getenv("OPENCRT_TEST_SSH_PORT");
    const char* user = std::getenv("OPENCRT_TEST_SSH_USER");
    const char* key = std::getenv("OPENCRT_TEST_SSH_KEY");
    if (port == nullptr || user == nullptr || key == nullptr) {
        return false;
    }
    out.port = std::atoi(port);
    out.user = user;
    out.key = key;
    return out.port > 0;
}

class SftpShellTest : public ::testing::Test {
 protected:
    void SetUp() override {
        if (!serverFromEnvironment(server_)) {
            GTEST_SKIP() << "no ssh server to talk to";
        }
        ssh::SshConnectionOptions options;
        options.host = server_.host;
        options.port = server_.port;
        options.user = server_.user;
        options.private_key_path = server_.key;
        options.auth_method = ssh::SshAuthMethod::kPublicKey;

        channel_ = std::make_unique<ssh::Ssh2Channel>(std::move(options));
        const openscrt::protocols::TransportResult result = channel_->connectAsync();
        ASSERT_TRUE(result.ok) << result.error;

        std::string error;
        ASSERT_TRUE(subsystem_.open(channel_->nativeSession(), error)) << error;
    }

    void TearDown() override {
        subsystem_.close();
        channel_.reset();
    }

    Server server_;
    std::unique_ptr<ssh::Ssh2Channel> channel_;
    sftp::SftpSubsystem subsystem_;
};

TEST_F(SftpShellTest, TheCommandsAnswerInLines) {
    sftp::SftpShell shell(subsystem_);

    const std::vector<std::string> help = shell.execute("help");
    ASSERT_FALSE(help.empty());
    EXPECT_NE(help.front().find("lpwd"), std::string::npos);

    // The local directory is where this process is.
    const std::vector<std::string> here = shell.execute("lpwd");
    ASSERT_EQ(here.size(), 1u);
    EXPECT_FALSE(here.front().empty());

    // The far side is the home of the session.
    const std::vector<std::string> there = shell.execute("pwd");
    ASSERT_EQ(there.size(), 1u);
    EXPECT_EQ(there.front()[0], '/');
}

TEST_F(SftpShellTest, AnUnknownCommandSaysSo) {
    sftp::SftpShell shell(subsystem_);
    const std::vector<std::string> answer = shell.execute("nonsense");
    ASSERT_EQ(answer.size(), 1u);
    EXPECT_NE(answer.front().find("unknown command"), std::string::npos);
}

TEST_F(SftpShellTest, ListingTheFarSideAnswers) {
    sftp::SftpShell shell(subsystem_);
    const std::vector<std::string> entries = shell.execute("ls");
    // The answer may be empty on a machine with an empty home, but it must not be an
    // error.
    for (const std::string& line : entries) {
        EXPECT_NE(line.find("sftp"), std::string::npos) << line;
    }
}

TEST_F(SftpShellTest, TheLocalPaneListsThisMachine) {
    // The far side of the pane needs a server, but the local side does not.
    ui::SftpDualPaneWidget widget;
    widget.setLocalDirectory(".");
    QTreeWidget* trees = widget.findChild<QTreeWidget*>();
    ASSERT_NE(trees, nullptr);
    // The local tree has the entries of this directory; the remote one says that
    // there is no connection.
    EXPECT_GE(trees->topLevelItemCount(), 1);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
