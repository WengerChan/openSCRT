#include "protocols/telnet/SocketChannel.h"

#include <QAbstractEventDispatcher>
#include <QAbstractSocket>
#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QtGlobal>
#include <utility>

namespace openscrt::protocols::telnet {
namespace {

// How often the reader looks at the things that are not the socket's signals: the
// bytes waiting to go out, the window size, and the keep-alive.
constexpr int kTickMs = 5;

constexpr std::size_t kRingCapacity = 64 * 1024;

}  // namespace

// The reader thread and the socket that lives in it.
//
// The socket is a Qt socket, so it is used the way Qt means it: it lives in a thread
// with an event loop, and its signals do the work. The other way round -- a plain
// thread and a call that waits -- looks simpler and is not: a Qt socket waits for a
// readable descriptor through a notifier, and a notifier needs an event dispatcher in
// the thread it lives in. A plain thread has none, so the wait times out every time.
// The symptom is strange and very telling: the bytes go out fine, and nothing ever
// comes back.
class SocketChannel::Worker {
 public:
    explicit Worker(SocketChannel* owner) : owner_(owner) {}

    ~Worker() { stop(); }

    // Starts the thread and moves the socket into it.
    void start(const TcpOptions& options) {
        // The socket is created here and moved into the reader thread: a socket has to
        // be created and used in the same thread, and the one exception is a socket
        // that has not been used yet.
        socket_ = new QSslSocket();
        socket_->moveToThread(&thread_);
        thread_.start();
        // The socket is the context of this call and it lives in the reader thread, so
        // the code below runs there.
        QMetaObject::invokeMethod(
            socket_, [this, options] { open(options); }, Qt::QueuedConnection);
    }

    void stop() {
        if (!thread_.isRunning()) {
            return;
        }
        // The socket belongs to the reader thread, so it is deleted there. The delete
        // is posted before the quit, so it runs before the loop ends.
        if (socket_ != nullptr) {
            socket_->deleteLater();
        }
        thread_.quit();
        thread_.wait();
        socket_ = nullptr;
    }

 private:
    // Runs in the reader thread.
    void open(const TcpOptions& options) {
        if (options.use_tls) {
            socket_->setProtocol(QSsl::TlsV1_2OrLater);
            QSslConfiguration configuration = socket_->sslConfiguration();
            configuration.setPeerVerifyMode(options.verify_peer ? QSslSocket::VerifyPeer
                                                                : QSslSocket::VerifyNone);
            socket_->setSslConfiguration(configuration);
        }

        // The socket is the context of every one of these, because the socket lives in
        // the reader thread: a connection whose context is the worker would run its
        // code in the thread that owns the worker instead.
        connect(socket_, &QAbstractSocket::connected, socket_,
                [this] { owner_->onSocketConnected(*socket_); });
        connect(socket_, &QAbstractSocket::disconnected, socket_,
                [this] { owner_->onSocketClosed(*socket_); });
        connect(socket_, &QIODevice::readyRead, socket_,
                [this] { owner_->onSocketReadable(*socket_); });

        timer_ = new QTimer(socket_);
        connect(timer_, &QTimer::timeout, socket_, [this] { owner_->onSocketTick(*socket_); });
        timer_->start(kTickMs);

        if (options.use_tls) {
            socket_->connectToHostEncrypted(QString::fromStdString(options.host),
                                            static_cast<quint16>(options.port));
        } else {
            socket_->connectToHost(QString::fromStdString(options.host),
                                   static_cast<quint16>(options.port));
        }
    }

    SocketChannel* owner_{nullptr};
    QThread thread_;
    QSslSocket* socket_{nullptr};
    QTimer* timer_{nullptr};
};

SocketChannel::SocketChannel(TcpOptions options, SocketMode mode)
    : options_(std::move(options)), mode_(mode), ring_(kRingCapacity) {
    window_size_ = options_.initial_size;
    // Only rlogin starts with a handshake to get through.
    handshake_done_ = mode_ != SocketMode::kRlogin;
    last_activity_ = std::chrono::steady_clock::now();
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

    if (worker_) {
        worker_->stop();
        worker_.reset();
    }
    worker_ = std::make_unique<Worker>(this);
    worker_->start(options_);
    return TransportResult{true, {}};
}

void SocketChannel::onSocketConnected(QSslSocket& socket) {
    std::vector<std::uint8_t> to_send;
    std::string handshake_error;
    onConnected(to_send, handshake_error);
    if (!handshake_error.empty()) {
        setState(TransportState::kFailed, handshake_error);
        socket.disconnectFromHost();
        return;
    }
    if (!to_send.empty()) {
        writeToSocket(socket, to_send.data(), to_send.size());
    }
    setState(TransportState::kConnected, options_.host);
    last_activity_ = std::chrono::steady_clock::now();
}

void SocketChannel::onSocketReadable(QSslSocket& socket) {
    const QByteArray chunk = socket.readAll();
    if (chunk.isEmpty()) {
        return;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.constData());
    const auto size = static_cast<std::size_t>(chunk.size());

    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> replies;
    if (!handshake_done_) {
        // The first bytes of a rlogin connection are the answer to the handshake, not
        // something to show.
        std::string error;
        if (!onEarlyBytes(bytes, size, text, error)) {
            setState(TransportState::kFailed, error);
            socket.disconnectFromHost();
            return;
        }
        handshake_done_ = true;
    } else {
        onIncoming(bytes, size, text, replies);
    }
    if (!replies.empty()) {
        writeToSocket(socket, replies.data(), replies.size());
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
    last_activity_ = std::chrono::steady_clock::now();
}

void SocketChannel::onSocketTick(QSslSocket& socket) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
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
                writeToSocket(socket, report.data(), report.size());
            }
        }
    }

    // The keep-alive: a peer that has gone quiet without a word is a peer whose
    // connection died, and a NAT that has dropped the mapping sends nothing at all.
    if (options_.keep_alive_seconds > 0) {
        const auto idle = std::chrono::steady_clock::now() - last_activity_;
        if (idle >= std::chrono::seconds(options_.keep_alive_seconds)) {
            const std::vector<std::uint8_t> probe = TelnetCodec::keepAlive();
            writeToSocket(socket, probe.data(), probe.size());
            last_activity_ = std::chrono::steady_clock::now();
        }
    }
}

void SocketChannel::onSocketClosed(QSslSocket& socket) {
    const TransportState current = state_.load();
    if (current == TransportState::kConnecting) {
        setState(TransportState::kFailed, socket.errorString().toStdString());
    } else {
        setState(TransportState::kDisconnected, socket.errorString().toStdString());
    }
    readable_.notify_all();
}

void SocketChannel::writeToSocket(QSslSocket& socket, const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    socket.write(reinterpret_cast<const char*>(data), static_cast<qint64>(size));
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

void SocketChannel::stopWorker() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    readable_.notify_all();
    if (worker_) {
        worker_->stop();
    }
}

TransportResult SocketChannel::disconnect() {
    const TransportState current = state_.load();
    if (current == TransportState::kIdle) {
        return TransportResult{true, {}};
    }
    // The reader is stopped before anything else: it owns the socket.
    stopWorker();
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