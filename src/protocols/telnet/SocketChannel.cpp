#include "protocols/telnet/SocketChannel.h"

#include <QSslConfiguration>
#include <QSslSocket>
#include <QtGlobal>
#include <utility>

namespace openscrt::protocols::telnet {
namespace {

// How long the reader waits for the socket before it looks at the stop flag again.
constexpr int kPollTimeoutMs = 100;

constexpr std::size_t kRingCapacity = 64 * 1024;

}  // namespace

SocketChannel::SocketChannel(TcpOptions options, SocketMode mode)
    : options_(std::move(options)), mode_(mode), ring_(kRingCapacity) {
    window_size_ = options_.initial_size;
}

SocketChannel::~SocketChannel() {
    disconnect();
}

TransportState SocketChannel::state() const noexcept {
    return state_.load();
}

void SocketChannel::setState(TransportState state, const std::string& detail) {
    state_.store(state);
    StateCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = state_callback_;
    }
    if (callback) {
        callback(state, detail);
    }
}

void SocketChannel::setDataCallback(DataCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    data_callback_ = std::move(callback);
}

void SocketChannel::setStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

void SocketChannel::onIncoming(const std::uint8_t* data, std::size_t size,
                               std::vector<std::uint8_t>& text,
                               std::vector<std::uint8_t>& replies) {
    // The raw conversation shows the bytes and says nothing back.
    text.assign(data, data + size);
    static_cast<void>(replies);
}

void SocketChannel::onConnected(std::vector<std::uint8_t>& to_send, std::string& error) {
    static_cast<void>(to_send);
    static_cast<void>(error);
}

bool SocketChannel::onEarlyBytes(const std::uint8_t* data, std::size_t size,
                                 std::vector<std::uint8_t>& text, std::string& error) {
    // Without a handshake there is no early phase: the bytes are the conversation.
    static_cast<void>(data);
    static_cast<void>(size);
    static_cast<void>(text);
    static_cast<void>(error);
    return true;
}

void SocketChannel::onWindowSizeChanged(const WindowSize& size,
                                        std::vector<std::uint8_t>& to_send) {
    // The raw conversation has no window to report.
    static_cast<void>(size);
    static_cast<void>(to_send);
}

TransportResult SocketChannel::connectAsync() {
    const TransportState current = state_.load();
    if (current == TransportState::kConnected || current == TransportState::kConnecting) {
        return TransportResult{false, "the connection is already open"};
    }
    if (options_.host.empty()) {
        const std::string message = "the connection has no host";
        setState(TransportState::kFailed, message);
        return TransportResult{false, message};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    setState(TransportState::kConnecting, options_.host);

    if (reader_.joinable()) {
        reader_.join();
    }
    reader_ = std::thread([this] { runReader(); });
    return TransportResult{true, {}};
}

void SocketChannel::runReader() {
    // The socket is created here and destroyed here. It is never touched by another
    // thread, which is the only way Qt sockets stay predictable.
    QSslSocket socket;
    if (options_.use_tls) {
        socket.setProtocol(QSsl::TlsV1_2OrLater);
        QSslConfiguration configuration = socket.sslConfiguration();
        configuration.setPeerVerifyMode(options_.verify_peer ? QSslSocket::VerifyPeer
                                                             : QSslSocket::VerifyNone);
        socket.setSslConfiguration(configuration);
        socket.connectToHostEncrypted(QString::fromStdString(options_.host),
                                      static_cast<quint16>(options_.port));
    } else {
        socket.connectToHost(QString::fromStdString(options_.host),
                             static_cast<quint16>(options_.port));
    }

    const int timeout_ms = options_.connect_timeout_seconds * 1000;
    if (!socket.waitForConnected(timeout_ms)) {
        const std::string message = "could not connect to " + options_.host + ":" +
                                    std::to_string(options_.port) + ": " +
                                    socket.errorString().toStdString();
        setState(TransportState::kFailed, message);
        return;
    }

    std::vector<std::uint8_t> to_send;
    std::string handshake_error;
    onConnected(to_send, handshake_error);
    if (!handshake_error.empty()) {
        setState(TransportState::kFailed, handshake_error);
        return;
    }
    if (!to_send.empty()) {
        socket.write(reinterpret_cast<const char*>(to_send.data()),
                     static_cast<qint64>(to_send.size()));
    }

    setState(TransportState::kConnected, options_.host);

    bool handshake_done = mode_ == SocketMode::kRlogin ? false : true;
    auto last_activity = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                break;
            }
        }

        drainOutbox(socket);

        // A window size the UI thread asked for goes to the peer now.
        {
            WindowSize changed{};
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(outbox_mutex_);
                if (window_size_pending_) {
                    window_size_pending_ = false;
                    changed = window_size_;
                    pending = true;
                }
            }
            if (pending) {
                std::vector<std::uint8_t> report;
                onWindowSizeChanged(changed, report);
                if (!report.empty()) {
                    socket.write(reinterpret_cast<const char*>(report.data()),
                                 static_cast<qint64>(report.size()));
                }
            }
        }

        if (socket.waitForReadyRead(kPollTimeoutMs)) {
            const QByteArray chunk = socket.readAll();
            if (!chunk.isEmpty()) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.constData());
                const auto size = static_cast<std::size_t>(chunk.size());
                std::vector<std::uint8_t> text;
                std::vector<std::uint8_t> replies;
                if (!handshake_done) {
                    std::string error;
                    if (!onEarlyBytes(bytes, size, text, error)) {
                        setState(TransportState::kFailed, error);
                        break;
                    }
                    handshake_done = true;
                } else {
                    onIncoming(bytes, size, text, replies);
                }
                if (!replies.empty()) {
                    socket.write(reinterpret_cast<const char*>(replies.data()),
                                 static_cast<qint64>(replies.size()));
                }
                if (!text.empty()) {
                    ring_.write(text.data(), text.size());
                    DataCallback callback;
                    {
                        std::lock_guard<std::mutex> lock(callback_mutex_);
                        callback = data_callback_;
                    }
                    if (callback) {
                        callback(text.data(), text.size());
                    }
                    readable_.notify_all();
                }
                last_activity = std::chrono::steady_clock::now();
            }
            continue;
        }

        if (socket.state() != QAbstractSocket::ConnectedState) {
            setState(TransportState::kDisconnected, socket.errorString().toStdString());
            break;
        }

        // The keep-alive: a peer that has gone quiet without a word is a peer whose
        // connection died, and a NAT that has dropped the mapping sends nothing at
        // all.
        if (options_.keep_alive_seconds > 0) {
            const auto idle = std::chrono::steady_clock::now() - last_activity;
            if (idle >= std::chrono::seconds(options_.keep_alive_seconds)) {
                const std::vector<std::uint8_t> probe = TelnetCodec::keepAlive();
                socket.write(reinterpret_cast<const char*>(probe.data()),
                             static_cast<qint64>(probe.size()));
                last_activity = std::chrono::steady_clock::now();
            }
        }
    }

    socket.disconnectFromHost();
    setState(TransportState::kDisconnected, "closed");
    readable_.notify_all();
}

void SocketChannel::drainOutbox(QSslSocket& socket) {
    std::vector<std::uint8_t> pending;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        if (outbox_.empty()) {
            return;
        }
        pending.assign(outbox_.begin(), outbox_.end());
        outbox_.clear();
    }
    const auto written = static_cast<std::size_t>(socket.write(
        reinterpret_cast<const char*>(pending.data()), static_cast<qint64>(pending.size())));
    if (written == 0 || written >= pending.size()) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_.insert(outbox_.begin(), pending.begin() + static_cast<std::ptrdiff_t>(written),
                   pending.end());
}

void SocketChannel::stopReader() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    readable_.notify_all();
    if (reader_.joinable()) {
        reader_.join();
    }
}

TransportResult SocketChannel::disconnect() {
    const TransportState current = state_.load();
    if (current == TransportState::kIdle) {
        return TransportResult{true, {}};
    }
    // The reader is stopped before anything else: it owns the socket.
    stopReader();
    const TransportState after = state_.load();
    if (after == TransportState::kConnected || after == TransportState::kConnecting) {
        setState(TransportState::kDisconnected, "closed");
    }
    return TransportResult{true, {}};
}

void SocketChannel::send(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_.insert(outbox_.end(), data, data + size);
}

void SocketChannel::send(std::string_view text) {
    send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool SocketChannel::resizeWindow(const WindowSize& size) {
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    window_size_ = size;
    window_size_pending_ = true;
    return true;
}

std::string SocketChannel::remoteAddress() const {
    return options_.host + ":" + std::to_string(options_.port);
}

std::size_t SocketChannel::readAvailable(std::uint8_t* destination, std::size_t max,
                                         std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (ring_.empty()) {
        if (state_.load() != TransportState::kConnected) {
            break;
        }
        if (readable_.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    lock.unlock();
    return ring_.read(destination, max);
}

// --------------------------------------------------------------------------- raw

RawTcpChannel::RawTcpChannel(TcpOptions options)
    : SocketChannel(std::move(options), SocketMode::kRaw) {}

// ------------------------------------------------------------------------ telnet

TelnetChannel::TelnetChannel(TcpOptions options)
    : SocketChannel(std::move(options), SocketMode::kTelnet) {
    codec_.setWindowSize(options_.initial_size);
    codec_.setTerminalType(options_.terminal_type);
}

void TelnetChannel::onIncoming(const std::uint8_t* data, std::size_t size,
                               std::vector<std::uint8_t>& text,
                               std::vector<std::uint8_t>& replies) {
    const TelnetCodec::Result result = codec_.feed(data, size);
    text = result.text;
    replies = result.replies;
}

void TelnetChannel::onWindowSizeChanged(const WindowSize& size,
                                        std::vector<std::uint8_t>& to_send) {
    codec_.setWindowSize(size);
    to_send = codec_.windowSizeReport();
}

// ----------------------------------------------------------------------- rlogin

RloginChannel::RloginChannel(TcpOptions options)
    : SocketChannel(std::move(options), SocketMode::kRlogin) {}

void RloginChannel::onConnected(std::vector<std::uint8_t>& to_send, std::string& error) {
    // RFC 1282: four null-terminated strings. The first is empty, and the other three
    // are the name on this side, the name on the far side, and the terminal with its
    // speed.
    //
    //   <null> client-user<null> server-user<null> terminal-type/speed<null>
    const std::string terminal = options_.terminal_type + "/9600";
    const std::string request = std::string(1, '\0') + options_.user + std::string(1, '\0') +
                                options_.server_user + std::string(1, '\0') + terminal +
                                std::string(1, '\0');
    to_send.assign(request.begin(), request.end());
    static_cast<void>(error);
}

bool RloginChannel::onEarlyBytes(const std::uint8_t* data, std::size_t size,
                                 std::vector<std::uint8_t>& text, std::string& error) {
    // The server answers with one byte: zero means the strings arrived and the
    // connection is now a shell. The bytes after that byte are already the shell's
    // output, so they are passed on rather than swallowed.
    if (size == 0) {
        return false;
    }
    if (data[0] == 0) {
        text.assign(data + 1, data + size);
        return true;
    }
    handshake_error_ = "the server refused the login";
    error = handshake_error_;
    return false;
}

void RloginChannel::onWindowSizeChanged(const WindowSize& size,
                                        std::vector<std::uint8_t>& to_send) {
    // RFC 1282: twelve bytes. The magic cookie, two lower-case "s", and then the four
    // 16-bit numbers: the rows, the columns, and the pixels in each direction.
    to_send.push_back(0xFF);
    to_send.push_back(0xFF);
    to_send.push_back('s');
    to_send.push_back('s');
    const auto rows = static_cast<std::uint16_t>(size.rows);
    const auto columns = static_cast<std::uint16_t>(size.columns);
    const auto pixels_x = static_cast<std::uint16_t>(size.pixels_x);
    const auto pixels_y = static_cast<std::uint16_t>(size.pixels_y);
    const std::uint16_t numbers[4] = {rows, columns, pixels_x, pixels_y};
    for (const std::uint16_t number : numbers) {
        to_send.push_back(static_cast<std::uint8_t>((number >> 8) & 0xFF));
        to_send.push_back(static_cast<std::uint8_t>(number & 0xFF));
    }
}

}  // namespace openscrt::protocols::telnet
