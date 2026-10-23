// SSH2 channel tests (P1-T11).
//
// There is no SSH server in CI, so these cover the contract and the failure paths: the
// channel must report its kind, fail cleanly on an unreachable host instead of hanging or
// crashing, and survive being disconnected before or after a failed connect.
//
// Connecting to a real server is the manual half of P1-A1 and stays recorded as such.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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

}  // namespace
}  // namespace openscrt::protocols::ssh