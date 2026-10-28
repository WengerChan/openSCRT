// The socket transports, end to end against a real server (P3-T3).
//
// The server is a QTcpServer in this same test, so the bytes that cross the socket
// are the bytes the other side really wrote. There is no stand-in in the middle.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "protocols/telnet/SocketChannel.h"

namespace telnet = openscrt::protocols::telnet;

namespace {

// A server that listens on a free port of the loopback and waits for one client.
class TestServer {
 public:
    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }

    [[nodiscard]] quint16 port() const { return server_.serverPort(); }

    // Waits for a client. The wait has a deadline: a test that waits forever is a
    // test that hangs the job instead of failing it.
    QTcpSocket* accept(int timeout_ms = 10000) {
        if (!server_.waitForNewConnection(timeout_ms)) {
            return nullptr;
        }
        return server_.nextPendingConnection();
    }

 private:
    QTcpServer server_;
};

std::string readSome(QTcpSocket* socket, int timeout_ms = 5000) {
    if (!socket->waitForReadyRead(timeout_ms)) {
        return {};
    }
    return socket->readAll().toStdString();
}

// Writes to the peer and makes sure the bytes actually left it.
//
// A socket that has no event loop of its own keeps what it cannot write in its
// own buffer, and that buffer is only flushed when the socket is told it may
// write -- which needs the event loop. This test has no event loop of its own,
// so the bytes are flushed here, and the probe says whether they left.
void writeToPeer(QTcpSocket* peer, const char* data, int size) {
    const qint64 written = peer->write(data, size);
    const bool flushed = peer->waitForBytesWritten(2000);
    std::cerr << "probe: peer wrote " << written << " of " << size << ", flushed " << flushed
              << ", state " << peer->state() << ", error " << peer->errorString().toStdString()
              << ", local " << peer->localAddress().toString().toStdString() << ", far "
              << peer->peerAddress().toString().toStdString() << std::endl;
    EXPECT_EQ(written, size);
    EXPECT_TRUE(flushed);
}

std::string readExactly(QTcpSocket* socket, std::size_t count, int timeout_ms = 10000) {
    std::string out;
    while (out.size() < count) {
        if (!socket->waitForReadyRead(timeout_ms)) {
            break;
        }
        out += socket->readAll().toStdString();
    }
    return out;
}

}  // namespace

TEST(SocketChannelTest, RawBytesTravelInBothDirections) {
    TestServer server;
    ASSERT_TRUE(server.listen());

    telnet::TcpOptions options;
    options.host = "127.0.0.1";
    options.port = static_cast<int>(server.port());
    options.keep_alive_seconds = 0;

    telnet::RawTcpChannel channel(options);
    ASSERT_TRUE(channel.connectAsync().ok);

    QTcpSocket* peer = server.accept();
    ASSERT_NE(peer, nullptr) << "the client never arrived";

    channel.send(std::string_view("ping"));
    EXPECT_EQ(readSome(peer), "ping");

    writeToPeer(peer, "pong", 4);
    std::vector<std::uint8_t> buffer(4);
    const std::size_t count =
        channel.readAvailable(buffer.data(), buffer.size(), std::chrono::seconds(10));
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "pong");
}

TEST(SocketChannelTest, TheTelnetChannelAnswersTheNegotiation) {
    TestServer server;
    ASSERT_TRUE(server.listen());

    telnet::TcpOptions options;
    options.host = "127.0.0.1";
    options.port = static_cast<int>(server.port());
    options.keep_alive_seconds = 0;
    options.initial_size = openscrt::protocols::WindowSize{40, 120};

    telnet::TelnetChannel channel(options);
    ASSERT_TRUE(channel.connectAsync().ok);

    QTcpSocket* peer = server.accept();
    ASSERT_NE(peer, nullptr) << "the client never arrived";

    // IAC DO NAWS.
    const char request[] = {static_cast<char>(255), static_cast<char>(253), static_cast<char>(31)};
    writeToPeer(peer, request, sizeof(request));

    // The answer is IAC WILL NAWS followed by the size we have.
    const std::string answer = readSome(peer);
    ASSERT_GE(answer.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(answer[0]), 255);
    EXPECT_EQ(static_cast<unsigned char>(answer[1]), 251);
    EXPECT_EQ(static_cast<unsigned char>(answer[2]), 31);

    // And then the text that follows the negotiation is text.
    writeToPeer(peer, "after", 5);
    std::vector<std::uint8_t> buffer(5);
    const std::size_t count =
        channel.readAvailable(buffer.data(), buffer.size(), std::chrono::seconds(10));
    ASSERT_EQ(count, 5u);
    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "after");
}

TEST(SocketChannelTest, TheRloginChannelSaysHelloTheWayTheProtocolSays) {
    TestServer server;
    ASSERT_TRUE(server.listen());

    telnet::TcpOptions options;
    options.host = "127.0.0.1";
    options.port = static_cast<int>(server.port());
    options.keep_alive_seconds = 0;
    options.user = "client";
    options.server_user = "server";
    options.terminal_type = "vt100";

    telnet::RloginChannel channel(options);
    ASSERT_TRUE(channel.connectAsync().ok);

    QTcpSocket* peer = server.accept();
    ASSERT_NE(peer, nullptr) << "the client never arrived";

    // RFC 1282: four null-terminated strings, the first one empty.
    const std::string expected = std::string(1, '\0') + "client" + std::string(1, '\0') + "server" +
                                 std::string(1, '\0') + "vt100/9600" + std::string(1, '\0');
    EXPECT_EQ(readExactly(peer, expected.size()), expected);

    // The server says the strings arrived, and the connection is now a shell.
    writeToPeer(peer, "\0", 1);
    writeToPeer(peer, "shell", 5);
    std::vector<std::uint8_t> buffer(5);
    const std::size_t count =
        channel.readAvailable(buffer.data(), buffer.size(), std::chrono::seconds(10));
    ASSERT_EQ(count, 5u);
    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "shell");
}

TEST(SocketChannelTest, AServerThatIsNotThereIsSaidSo) {
    telnet::TcpOptions options;
    options.host = "127.0.0.1";
    // A port nothing listens on: the connection has to fail with a message, not hang.
    options.port = 1;
    options.connect_timeout_seconds = 5;
    options.keep_alive_seconds = 0;

    telnet::RawTcpChannel channel(options);
    ASSERT_TRUE(channel.connectAsync().ok);

    // Wait for the failure to be noticed, with a deadline.
    for (int attempt = 0;
         attempt < 100 && channel.state() != openscrt::protocols::TransportState::kFailed;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(channel.state(), openscrt::protocols::TransportState::kFailed);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
