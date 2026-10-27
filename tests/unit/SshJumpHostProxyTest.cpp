// The jump host proxy (P3-T9).
//
// The test needs a server, so it skips itself where there is none. In CI the Linux
// leg starts a real sshd, and that one machine is both the hop and the target: the
// chain is real even though its two ends are the same address.
//
// The thing that proves the chain works is a byte that comes back through it: the
// sshd answers with its version string, and that string has travelled through the hop
// to reach us.

#include <gtest/gtest.h>
#include <libssh/libssh.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "protocols/ssh/SshJumpHostProxy.h"

namespace ssh = openscrt::protocols::ssh;

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

}  // namespace

class SshJumpHostProxyTest : public ::testing::Test {
 protected:
    void SetUp() override {
        if (!serverFromEnvironment(server_)) {
            GTEST_SKIP() << "no ssh server to talk to";
        }
    }

    Server server_;
};

TEST_F(SshJumpHostProxyTest, TheTargetIsReachedThroughTheHop) {
    ssh::JumpHostChain chain;
    ssh::JumpHostStep hop;
    hop.host = server_.host;
    hop.port = server_.port;
    hop.user = server_.user;
    hop.private_key_path = server_.key;
    chain.hops.push_back(hop);
    chain.target_host = server_.host;
    chain.target_port = server_.port;

    ssh::SshJumpHostProxy proxy(chain);
    const openscrt::protocols::TransportResult result = proxy.connect();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_NE(proxy.targetChannel(), nullptr);

    // The target answers with its version string, and that answer has travelled
    // through the hop.
    auto* channel = static_cast<ssh_channel>(proxy.targetChannel());
    const char request[] = "SSH-2.0-openscrt-test\r\n";
    EXPECT_GT(ssh_channel_write(channel, request, static_cast<std::uint32_t>(sizeof(request))), 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::string answer;
    while (answer.find("SSH-") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        if (ssh_channel_poll_timeout(channel, 100, 0) > 0) {
            char buffer[256];
            const int count = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
            if (count > 0) {
                answer.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    EXPECT_NE(answer.find("SSH-"), std::string::npos) << answer;
}

TEST_F(SshJumpHostProxyTest, AChainWithoutATargetSaysSo) {
    ssh::JumpHostChain chain;
    ssh::SshJumpHostProxy proxy(chain);
    const openscrt::protocols::TransportResult result = proxy.connect();
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(SshJumpHostProxyTest, AHopThatCannotBeReachedIsNamed) {
    ssh::JumpHostChain chain;
    ssh::JumpHostStep hop;
    // A port nothing listens on.
    hop.host = "127.0.0.1";
    hop.port = 1;
    hop.user = server_.user;
    hop.private_key_path = server_.key;
    chain.hops.push_back(hop);
    chain.target_host = server_.host;
    chain.target_port = server_.port;

    ssh::SshJumpHostProxy proxy(chain);
    const openscrt::protocols::TransportResult result = proxy.connect();
    EXPECT_FALSE(result.ok);
    // The message says which hop failed: a chain of three that fails on the second is
    // a different problem from one that fails on the first.
    EXPECT_NE(result.error.find("hop 1"), std::string::npos) << result.error;
}
