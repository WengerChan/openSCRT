// The SSH port forwarders (P3-T8).
//
// The test needs a server, so it skips itself where there is none. In CI the Linux
// leg starts a real sshd and points the test at it, so what is checked here is a real
// forwarding and not a stand-in.
//
// The thing that is forwarded to is the sshd itself: it answers with its version
// string, so a byte that comes back through the tunnel is a byte that went through
// the SSH session.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTcpSocket>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "protocols/ssh/Ssh2Channel.h"
#include "protocols/ssh/SshPortForwarder.h"

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

// Reads what the far end says, with a deadline.
std::string readSome(QTcpSocket& socket, int timeout_ms = 10000) {
    if (!socket.waitForReadyRead(timeout_ms)) {
        return {};
    }
    return socket.readAll().toStdString();
}

}  // namespace

class SshPortForwarderTest : public ::testing::Test {
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

        channel_ = std::make_unique<ssh::Ssh2Channel>(options);
        const openscrt::protocols::TransportResult result = channel_->connectAsync();
        ASSERT_TRUE(result.ok) << result.error;
    }

    void TearDown() override { channel_.reset(); }

    Server server_;
    std::unique_ptr<ssh::Ssh2Channel> channel_;
};

TEST_F(SshPortForwarderTest, ALocalForwardCarriesBytesToTheServer) {
    ssh::ForwardRule rule;
    rule.mode = ssh::ForwardMode::kLocal;
    rule.listen_address = "127.0.0.1";
    rule.listen_port = 0;  // Any free port.
    rule.remote_address = "127.0.0.1";
    rule.remote_port = server_.port;

    ssh::SshPortForwarder forwarder(channel_->nativeSession(), rule);
    std::string error;
    ASSERT_TRUE(forwarder.start(error)) << error;
    ASSERT_GT(forwarder.boundPort(), 0);

    QTcpSocket client;
    client.connectToHost("127.0.0.1", static_cast<quint16>(forwarder.boundPort()));
    ASSERT_TRUE(client.waitForConnected(10000));

    // The sshd answers with its version string. It has to come back through the
    // tunnel.
    client.write("SSH-2.0-openscrt-test\r\n");
    const std::string answer = readSome(client);
    EXPECT_NE(answer.find("SSH-"), std::string::npos) << answer;

    forwarder.stop();
}

TEST_F(SshPortForwarderTest, ADynamicForwardIsASocksProxy) {
    ssh::ForwardRule rule;
    rule.mode = ssh::ForwardMode::kDynamic;
    rule.listen_address = "127.0.0.1";
    rule.listen_port = 0;

    ssh::SshPortForwarder forwarder(channel_->nativeSession(), rule);
    std::string error;
    ASSERT_TRUE(forwarder.start(error)) << error;

    QTcpSocket client;
    client.connectToHost("127.0.0.1", static_cast<quint16>(forwarder.boundPort()));
    ASSERT_TRUE(client.waitForConnected(10000));

    // The greeting: the version, one method, and "no authentication".
    const char greeting[3] = {0x05, 0x01, 0x00};
    client.write(greeting, sizeof(greeting));
    ASSERT_TRUE(client.waitForReadyRead(10000));
    const QByteArray answered = client.read(2);
    ASSERT_EQ(answered.size(), 2);
    EXPECT_EQ(static_cast<std::uint8_t>(answered[0]), 0x05);
    EXPECT_EQ(static_cast<std::uint8_t>(answered[1]), 0x00);

    // The request: connect, an address of four bytes, and a port.
    const unsigned char request[10] = {0x05,
                                       0x01,
                                       0x00,
                                       0x01,
                                       127,
                                       0,
                                       0,
                                       1,
                                       static_cast<unsigned char>((server_.port >> 8) & 0xFF),
                                       static_cast<unsigned char>(server_.port & 0xFF)};
    client.write(reinterpret_cast<const char*>(request), sizeof(request));

    ASSERT_TRUE(client.waitForReadyRead(10000));
    const QByteArray reply = client.read(10);
    ASSERT_EQ(reply.size(), 10);
    EXPECT_EQ(static_cast<std::uint8_t>(reply[0]), 0x05);
    EXPECT_EQ(static_cast<std::uint8_t>(reply[1]), 0x00) << "the proxy refused the connection";

    // And the bytes go through.
    client.write("SSH-2.0-openscrt-test\r\n");
    const std::string answer = readSome(client);
    EXPECT_NE(answer.find("SSH-"), std::string::npos) << answer;

    forwarder.stop();
}

TEST_F(SshPortForwarderTest, ARemoteForwardMakesTheServerListen) {
    ssh::ForwardRule rule;
    rule.mode = ssh::ForwardMode::kRemote;
    rule.listen_address = "127.0.0.1";
    rule.listen_port = 0;  // The server picks a free port.
    // Where the traffic goes when somebody connects to the server's port: back to
    // this machine.
    rule.remote_address = "127.0.0.1";
    rule.remote_port = server_.port;

    ssh::SshPortForwarder forwarder(channel_->nativeSession(), rule);
    std::string error;
    ASSERT_TRUE(forwarder.start(error)) << error;
    ASSERT_GT(forwarder.boundPort(), 0);

    // The server is listening, so a connection to that port reaches us.
    QTcpSocket client;
    client.connectToHost("127.0.0.1", static_cast<quint16>(forwarder.boundPort()));
    ASSERT_TRUE(client.waitForConnected(10000));

    client.write("SSH-2.0-openscrt-test\r\n");
    const std::string answer = readSome(client);
    EXPECT_NE(answer.find("SSH-"), std::string::npos) << answer;

    forwarder.stop();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
