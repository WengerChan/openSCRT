// The SFTP subsystem (P3-T4).
//
// The test needs a server, so it skips itself where there is none. In CI the Linux
// leg starts a real sshd and points the test at it, so what is checked here is the
// subsystem against a real server and not against a stand-in.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "protocols/ssh/Ssh2Channel.h"
#include "transfer/sftp/SftpSubsystem.h"

namespace ssh = openscrt::protocols::ssh;
namespace sftp = openscrt::transfer::sftp;

namespace {

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

// Connects and opens the subsystem on the same session. The point of the whole
// exercise: no second password, no second handshake.
class SftpFixture : public ::testing::Test {
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

}  // namespace

TEST_F(SftpFixture, TheSubsystemOpensOnTheSessionThatIsAlreadyConnected) {
    EXPECT_TRUE(subsystem_.isOpen());

    // The home directory is the proof that the subsystem is really talking to the
    // server: it is the one thing the server has to answer.
    std::string error;
    const std::string home = subsystem_.homeDirectory(error);
    EXPECT_FALSE(home.empty()) << error;
    EXPECT_EQ(home[0], '/') << "the home of a session is an absolute path";
}

TEST_F(SftpFixture, TheHomeDirectoryCanBeListed) {
    std::string error;
    const std::string home = subsystem_.homeDirectory(error);
    ASSERT_FALSE(home.empty()) << error;

    const std::vector<sftp::SftpFileInfo> entries = subsystem_.listDirectory(home, error);
    EXPECT_TRUE(error.empty()) << error;
    // The home directory of a session has at least the two entries that point at
    // itself -- which the listing leaves out -- and usually more.
    EXPECT_FALSE(entries.empty());
}

TEST_F(SftpFixture, AFileSurvivesTheRoundTrip) {
    std::string error;
    const std::string home = subsystem_.homeDirectory(error);
    ASSERT_FALSE(home.empty()) << error;

    // A file with bytes that are not text, so a transfer that mangles them shows.
    const std::string name = "openscrt-sftp-roundtrip.bin";
    const std::string remote = home + "/" + name;
    std::string payload;
    for (int index = 0; index < 4096; ++index) {
        payload.push_back(static_cast<char>(index % 256));
    }

    // The local names are in the working directory of the test, which is the build
    // directory.
    const std::string local = "openscrt-sftp-out.bin";
    const std::string back = "openscrt-sftp-back.bin";

    {
        std::ofstream writer(local, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(writer.good());
        writer.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(writer.good());
    }

    ASSERT_TRUE(subsystem_.upload(local, remote, error)) << error;

    ASSERT_TRUE(subsystem_.download(remote, back, error)) << error;

    std::ifstream reader(back, std::ios::binary);
    ASSERT_TRUE(reader.good());
    const std::string received((std::istreambuf_iterator<char>(reader)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);

    std::remove(local.c_str());
    std::remove(back.c_str());
    subsystem_.remove(remote, error);
}

TEST_F(SftpFixture, AFileCanBeStat'ed) {
    std::string error;
    const std::string home = subsystem_.homeDirectory(error);
    ASSERT_FALSE(home.empty()) << error;

    const std::optional<sftp::SftpFileInfo> info = subsystem_.stat(home, error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_TRUE(info->is_directory) << "the home of a session is a directory";
}

TEST_F(SftpFixture, ADirectoryCanBeMadeAndRemoved) {
    std::string error;
    const std::string home = subsystem_.homeDirectory(error);
    ASSERT_FALSE(home.empty()) << error;

    const std::string directory = home + "/openscrt-sftp-directory";
    ASSERT_TRUE(subsystem_.makeDirectory(directory, error)) << error;

    const std::optional<sftp::SftpFileInfo> info = subsystem_.stat(directory, error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_TRUE(info->is_directory);

    ASSERT_TRUE(subsystem_.removeDirectory(directory, error)) << error;
}
