// SSH2 channel tests (P1-T11).
//
// Most of these cover the contract and the failure paths: the channel must report its kind,
// fail cleanly on an unreachable host instead of hanging or crashing, and survive being
// disconnected before or after a failed connect.
//
// The last one is different: it connects to a real SSH server that the CI Linux leg starts
// on a loopback port. Without it, the handshake and the public-key login -- the whole point
// of P1-T11 -- would never be executed by anything.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "protocols/ssh/Ssh2Channel.h"

namespace openscrt::protocols::ssh {
namespace {

SshConnectionOptions unreachableHost() {
    SshConnectionOptions options;
    // Port 1 on loopback is closed on every runner; the connection is refused immediately
    // rather than timing out, so the test stays fast.
    options.host = "127.0.0.1";
    options.port = 1;
    options.user = "nobody";
    options.password = "irrelevant";
    options.auth_method = SshAuthMethod::kPassword;
    options.timeout_seconds = 5;
    return options;
}

TEST(Ssh2ChannelTest, ReportsItsKind) {
    Ssh2Channel channel{unreachableHost()};
    EXPECT_EQ(channel.kind(), TransportKind::kSsh2);
    EXPECT_EQ(channel.state(), TransportState::kIdle);
}

TEST(Ssh2ChannelTest, RemoteAddressIncludesUserAndPort) {
    Ssh2Channel channel{unreachableHost()};
    EXPECT_EQ(channel.remoteAddress(), "nobody@127.0.0.1:1");
}

TEST(Ssh2ChannelTest, RemoteAddressOmitsAnEmptyUserButKeepsThePort) {
    SshConnectionOptions options = unreachableHost();
    options.user.clear();
    Ssh2Channel channel{options};
    EXPECT_EQ(channel.remoteAddress(), "127.0.0.1:1");
}

TEST(Ssh2ChannelTest, ConnectToAnUnreachableHostFailsCleanly) {
    Ssh2Channel channel{unreachableHost()};
    const TransportResult result = channel.connectAsync();
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty()) << "a failure must say why";
    EXPECT_EQ(channel.state(), TransportState::kFailed);
}

TEST(Ssh2ChannelTest, ConnectWithoutAHostFailsWithoutTouchingTheNetwork) {
    SshConnectionOptions options = unreachableHost();
    options.host.clear();
    Ssh2Channel channel{options};
    const TransportResult result = channel.connectAsync();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "no host");
    EXPECT_EQ(channel.state(), TransportState::kFailed);
}

TEST(Ssh2ChannelTest, DisconnectBeforeConnectingIsHarmless) {
    Ssh2Channel channel{unreachableHost()};
    EXPECT_TRUE(channel.disconnect().ok);
    EXPECT_TRUE(channel.disconnect().ok) << "a second disconnect must be a no-op";
}

TEST(Ssh2ChannelTest, DisconnectAfterAFailedConnectIsHarmless) {
    Ssh2Channel channel{unreachableHost()};
    static_cast<void>(channel.connectAsync());
    EXPECT_TRUE(channel.disconnect().ok);
    EXPECT_EQ(channel.state(), TransportState::kDisconnected);
}

TEST(Ssh2ChannelTest, SendingBeforeConnectingIsIgnored) {
    Ssh2Channel channel{unreachableHost()};
    const std::uint8_t bytes[] = {'h', 'e', 'l', 'l', 'o'};
    channel.send(bytes, sizeof(bytes));
    channel.send("hello");
    SUCCEED() << "must not crash without a session";
}

TEST(Ssh2ChannelTest, ResizeBeforeConnectingReportsFailure) {
    Ssh2Channel channel{unreachableHost()};
    EXPECT_FALSE(channel.resizeWindow(WindowSize{40, 120}));
}

TEST(Ssh2ChannelTest, ReadBeforeConnectingReturnsNothing) {
    Ssh2Channel channel{unreachableHost()};
    std::uint8_t buffer[64];
    EXPECT_EQ(channel.readAvailable(buffer, sizeof(buffer), std::chrono::milliseconds(10)), 0u);
}

TEST(Ssh2ChannelTest, StateCallbackFiresOnFailure) {
    Ssh2Channel channel{unreachableHost()};
    bool sawFailure = false;
    channel.setStateCallback([&sawFailure](TransportState state, const std::string&) {
        if (state == TransportState::kFailed) {
            sawFailure = true;
        }
    });
    static_cast<void>(channel.connectAsync());
    EXPECT_TRUE(sawFailure);
}

TEST(Ssh2ChannelTest, ConnectsToARealServerAndRunsACommand) {
    // The one test that actually talks to an SSH server. The CI Linux leg starts an sshd
    // on a loopback port and points these variables at it; every other platform has no
    // server and says so instead of pretending to have passed.
    //
    // Until this existed, every SSH test covered a failure path: the handshake and the
    // public-key login had never been executed anywhere.
    const char* port = std::getenv("OPENCRT_TEST_SSH_PORT");
    const char* user = std::getenv("OPENCRT_TEST_SSH_USER");
    const char* key = std::getenv("OPENCRT_TEST_SSH_KEY");
    if (port == nullptr || user == nullptr || key == nullptr) {
        GTEST_SKIP() << "no SSH server configured for this run";
    }

    SshConnectionOptions options;
    options.host = "127.0.0.1";
    options.port = std::atoi(port);
    options.user = user;
    options.private_key_path = key;
    options.auth_method = SshAuthMethod::kPublicKey;
    options.timeout_seconds = 20;

    Ssh2Channel channel{options};
    const TransportResult result = channel.connectAsync();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(channel.state(), TransportState::kConnected);

    // The marker the shell prints is not the text that was typed: the remote PTY echoes
    // the command back, and a marker identical to the command would match the echo even
    // if the command never ran.
    channel.send("printf 'openscrt-ssh-%s\\n' ok\r");

    std::string accumulated;
    std::uint8_t buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::size_t count = channel.readAvailable(buffer, sizeof(buffer), remaining);
        accumulated.append(reinterpret_cast<char*>(buffer), count);
        if (accumulated.find("openscrt-ssh-ok") != std::string::npos) {
            break;
        }
        if (count == 0 && channel.state() != TransportState::kConnected) {
            break;
        }
    }
    EXPECT_NE(accumulated.find("openscrt-ssh-ok"), std::string::npos)
        << "the command output never came back over the SSH channel";
    EXPECT_TRUE(channel.disconnect().ok);
}

}  // namespace
}  // namespace openscrt::protocols::ssh