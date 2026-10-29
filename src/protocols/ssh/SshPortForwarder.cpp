#include "protocols/ssh/SshPortForwarder.h"

#include <libssh/libssh.h>

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include "protocols/ssh/SshSessionLock.h"

namespace openscrt::protocols::ssh {
namespace {

// How long the worker waits for something to happen. The wait is short because the
// worker has two things to look at: the socket and the channel.
constexpr int kPollTimeoutMs = 2;

// The SOCKS5 reply codes.
constexpr std::uint8_t kSocksSuccess = 0x00;
constexpr std::uint8_t kSocksGeneralFailure = 0x01;
constexpr std::uint8_t kSocksCommandNotSupported = 0x07;

std::uint16_t readBigEndian16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

}  // namespace

SshPortForwarder::SshPortForwarder(SshNativeSession session, ForwardRule rule)
    : session_(std::move(session)), rule_(std::move(rule)) {}

SshPortForwarder::~SshPortForwarder() {
    stop();
}

bool SshPortForwarder::start(std::string& error) {
    if (!session_.valid()) {
        error = "the ssh session is not connected";
        return false;
    }
    auto* native = static_cast<ssh_session>(session_.get());

    if (rule_.mode == ForwardMode::kRemote) {
        // The server is asked to listen. The address is the one on the server.
        int bound = 0;
        int answer = 0;
        {
            // libssh is not thread-safe: the session is locked while the request
            // is made, and the request reads the socket itself.
            const std::lock_guard<std::mutex> lock(sshSessionLock());
            answer = ssh_channel_listen_forward(native, rule_.listen_address.c_str(),
                                                rule_.listen_port, &bound);
        }
        if (answer != SSH_OK) {
            const char* reason = ssh_get_error(native);
            error = std::string("the server refused to listen: ") +
                    (reason != nullptr && *reason != '\0' ? reason : "no reason given");
            return false;
        }
        bound_port_.store(bound);
        return true;
    }

    // A local or a dynamic forward: the listener is on this machine.
    server_ = std::make_unique<QTcpServer>();
    if (!server_->listen(QHostAddress(rule_.listen_address.c_str()),
                         static_cast<quint16>(rule_.listen_port))) {
        error = std::string("could not listen on ") + rule_.listen_address + ":" +
                std::to_string(rule_.listen_port) + ": " + server_->errorString().toStdString();
        return false;
    }
    bound_port_.store(static_cast<int>(server_->serverPort()));

    stopping_.store(false);
    worker_ = std::thread([this] { run(); });
    return true;
}

void SshPortForwarder::stop() {
    stopping_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    for (Connection& connection : connections_) {
        if (connection.channel != nullptr) {
            const std::lock_guard<std::mutex> lock(sshSessionLock());
            ssh_channel_free(static_cast<ssh_channel>(connection.channel));
        }
        if (connection.socket != nullptr) {
            connection.socket->close();
            delete connection.socket;
        }
    }
    connections_.clear();
    if (server_) {
        server_->close();
    }
}

bool SshPortForwarder::acceptPending() {
    if (!server_ || !server_->hasPendingConnections()) {
        return false;
    }
    QTcpSocket* socket = server_->nextPendingConnection();
    if (socket == nullptr) {
        return false;
    }
    auto* native = static_cast<ssh_session>(session_.get());

    Connection connection;
    connection.socket = socket;

    if (rule_.mode == ForwardMode::kDynamic) {
        // The client says where it wants to go. Until it has said, there is no
        // channel to open.
        connections_.push_back(connection);
        std::string error;
        if (!readSocksRequest(connections_.back(), error)) {
            Connection& failed = connections_.back();
            if (failed.socket != nullptr) {
                failed.socket->close();
                delete failed.socket;
            }
            connections_.pop_back();
        }
        return true;
    }

    ssh_channel channel = nullptr;
    int opened = 0;
    {
        // libssh is not thread-safe: the session is locked while the channel is
        // opened, and the opening reads the socket itself.
        const std::lock_guard<std::mutex> lock(sshSessionLock());
        channel = ssh_channel_new(native);
        if (channel != nullptr) {
            opened = ssh_channel_open_forward(channel, rule_.remote_address.c_str(),
                                              rule_.remote_port, "127.0.0.1", 0);
        }
    }
    if (channel == nullptr) {
        socket->close();
        delete socket;
        return false;
    }
    if (opened != SSH_OK) {
        ssh_channel_free(channel);
        socket->close();
        delete socket;
        return false;
    }
    connection.channel = channel;
    connection.target_host = rule_.remote_address;
    connection.target_port = rule_.remote_port;
    connections_.push_back(connection);
    return true;
}

bool SshPortForwarder::acceptRemote() {
    // The server is listening, and somebody connected to its port. The server asks us
    // to open the other end of it.
    auto* native = static_cast<ssh_session>(session_.get());
    int destination_port = 0;
    int originator_port = 0;
    char* originator = nullptr;
    ssh_channel channel = nullptr;
    {
        // libssh is not thread-safe: the session is locked while the request is
        // made, and the request reads the socket itself.
        const std::lock_guard<std::mutex> lock(sshSessionLock());
        channel = ssh_channel_open_forward_port(native, 50, &destination_port, &originator,
                                                &originator_port);
    }
    if (channel == nullptr) {
        return false;
    }
    if (originator != nullptr) {
        // The string is a plain copy, so it goes back with the plain free.
        free(originator);
    }
    auto* socket = new QTcpSocket();
    socket->connectToHost(QString::fromStdString(rule_.remote_address),
                          static_cast<quint16>(rule_.remote_port));
    if (!socket->waitForConnected(5000)) {
        ssh_channel_free(channel);
        socket->close();
        delete socket;
        return false;
    }
    Connection connection;
    connection.socket = socket;
    connection.channel = channel;
    connection.target_host = rule_.remote_address;
    connection.target_port = rule_.remote_port;
    connections_.push_back(connection);
    return true;
}

bool SshPortForwarder::readSocksRequest(Connection& connection, std::string& error) {
    QTcpSocket* socket = connection.socket;
    // The greeting: the version, the number of methods, and the methods.
    if (!socket->waitForReadyRead(5000)) {
        error = "the socks client said nothing";
        return false;
    }
    QByteArray greeting = socket->read(2);
    if (greeting.size() < 2 || static_cast<std::uint8_t>(greeting[0]) != 0x05) {
        error = "the socks client did not speak socks5";
        return false;
    }
    const int method_count = static_cast<std::uint8_t>(greeting[1]);
    if (method_count <= 0) {
        error = "the socks client offered no method";
        return false;
    }
    if (!socket->waitForReadyRead(5000)) {
        error = "the socks client said nothing";
        return false;
    }
    socket->read(method_count);
    // No authentication: the connection is already inside the SSH session.
    const char answer[2] = {0x05, 0x00};
    socket->write(answer, sizeof(answer));

    // The request: the version, the command, a byte that is always zero, the kind of
    // address, the address and the port.
    if (!socket->waitForReadyRead(5000)) {
        error = "the socks client said nothing";
        return false;
    }
    QByteArray request = socket->read(4);
    if (request.size() < 4 || static_cast<std::uint8_t>(request[0]) != 0x05) {
        error = "the socks request was malformed";
        return false;
    }
    const std::uint8_t command = static_cast<std::uint8_t>(request[1]);
    const std::uint8_t kind = static_cast<std::uint8_t>(request[3]);

    std::string host;
    if (kind == 0x01) {
        // Four bytes of address.
        if (!socket->waitForReadyRead(5000)) {
            error = "the socks request was malformed";
            return false;
        }
        const QByteArray address = socket->read(4);
        if (address.size() < 4) {
            error = "the socks request was malformed";
            return false;
        }
        // Four bytes of address. The constructor of QHostAddress takes them as an
        // array, not one by one.
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(address[0]), static_cast<unsigned char>(address[1]),
            static_cast<unsigned char>(address[2]), static_cast<unsigned char>(address[3])};
        host = QHostAddress(bytes).toString().toStdString();
    } else if (kind == 0x03) {
        // A name: a length and then the name.
        if (!socket->waitForReadyRead(5000)) {
            error = "the socks request was malformed";
            return false;
        }
        const QByteArray length = socket->read(1);
        if (length.size() < 1) {
            error = "the socks request was malformed";
            return false;
        }
        const int name_length = static_cast<std::uint8_t>(length[0]);
        if (!socket->waitForReadyRead(5000)) {
            error = "the socks request was malformed";
            return false;
        }
        const QByteArray name = socket->read(name_length);
        if (name.size() < name_length) {
            error = "the socks request was malformed";
            return false;
        }
        host = name.toStdString();
    } else {
        error = "the socks request used an address kind this proxy does not know";
        return false;
    }

    if (!socket->waitForReadyRead(5000)) {
        error = "the socks request was malformed";
        return false;
    }
    const QByteArray port_bytes = socket->read(2);
    if (port_bytes.size() < 2) {
        error = "the socks request was malformed";
        return false;
    }
    const int port = static_cast<int>(
        readBigEndian16(reinterpret_cast<const std::uint8_t*>(port_bytes.constData())));

    if (command != 0x01) {
        writeSocksReply(connection, kSocksCommandNotSupported);
        error = "the socks client asked for something this proxy does not do";
        return false;
    }

    connection.target_host = host;
    connection.target_port = port;

    auto* native = static_cast<ssh_session>(session_.get());
    ssh_channel channel = nullptr;
    bool opened = false;
    {
        // libssh is not thread-safe: the session is locked while the channel is
        // opened, and the opening reads the socket itself.
        const std::lock_guard<std::mutex> lock(sshSessionLock());
        channel = ssh_channel_new(native);
        if (channel != nullptr) {
            opened =
                ssh_channel_open_forward(channel, host.c_str(), port, "127.0.0.1", 0) == SSH_OK;
        }
    }
    if (channel == nullptr) {
        writeSocksReply(connection, kSocksGeneralFailure);
        error = "the channel could not be opened";
        return false;
    }
    if (!opened) {
        ssh_channel_free(channel);
        writeSocksReply(connection, kSocksGeneralFailure);
        error = "the server refused the connection";
        return false;
    }
    connection.channel = channel;
    writeSocksReply(connection, kSocksSuccess);
    return true;
}

void SshPortForwarder::writeSocksReply(Connection& connection, std::uint8_t code) {
    if (connection.socket == nullptr) {
        return;
    }
    // The version, the answer, a byte that is always zero, the kind of address (four
    // bytes of it), the address and the port.
    const unsigned char reply[10] = {0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    connection.socket->write(reinterpret_cast<const char*>(reply), sizeof(reply));
}

bool SshPortForwarder::pump(Connection& connection) {
    // One turn of the two directions. The channel is polled and the socket is waited
    // for, and both waits are short: the worker has to come back to the other
    // connections and notice that it should stop.
    if (connection.socket == nullptr || connection.channel == nullptr) {
        return false;
    }
    if (connection.socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    auto* channel = static_cast<ssh_channel>(connection.channel);

    if (connection.socket->waitForReadyRead(kPollTimeoutMs)) {
        const QByteArray chunk = connection.socket->readAll();
        if (!chunk.isEmpty()) {
            const std::lock_guard<std::mutex> lock(sshSessionLock());
            ssh_channel_write(channel, chunk.constData(), static_cast<std::uint32_t>(chunk.size()));
        }
    }

    int ready = 0;
    int count = 0;
    char buffer[8192];
    {
        // libssh is not thread-safe: the session is locked while the channel is
        // read.
        const std::lock_guard<std::mutex> lock(sshSessionLock());
        ready = ssh_channel_poll_timeout(channel, kPollTimeoutMs, 0);
        if (ready > 0) {
            count = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
        }
    }
    if (ready > 0) {
        if (count > 0) {
            connection.socket->write(buffer, count);
        } else if (count == 0) {
            return false;  // The far end closed.
        }
    } else if (ready < 0) {
        return false;
    }
    return true;
}

void SshPortForwarder::run() {
    while (!stopping_.load()) {
        if (rule_.mode == ForwardMode::kRemote) {
            static_cast<void>(acceptRemote());
        } else {
            static_cast<void>(acceptPending());
        }
        for (std::size_t index = 0; index < connections_.size();) {
            Connection& connection = connections_[index];
            if (pump(connection)) {
                ++index;
                continue;
            }
            if (connection.channel != nullptr) {
                ssh_channel_free(static_cast<ssh_channel>(connection.channel));
            }
            if (connection.socket != nullptr) {
                connection.socket->close();
                delete connection.socket;
            }
            connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

}  // namespace openscrt::protocols::ssh
