// The socket transports: raw TCP, Telnet and rlogin (P3-T3).
//
// All three are the same machinery with a different conversation on top of it, so
// they live together: a socket that carries bytes, and a mode that says what those
// bytes mean.
//
// The socket belongs to the reader thread and to nobody else. Qt sockets are not
// thread-safe, so the UI thread never touches one: the bytes it wants to send go
// into a queue that the reader drains, and the reader is the only one who calls into
// Qt. The reader waits with a bounded timeout rather than on an event loop, so a
// stop request is noticed within one poll interval.

#ifndef OPENCRT_PROTOCOLS_TELNET_SOCKET_CHANNEL_H_
#define OPENCRT_PROTOCOLS_TELNET_SOCKET_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/SecureByteArray.h"
#include "core/SpscRingBuffer.h"
#include "protocols/ITransportChannel.h"
#include "protocols/telnet/TelnetCodec.h"

class QSslSocket;

namespace openscrt::protocols::telnet {

struct TcpOptions {
    std::string host;
    int port{23};

    // Telnet over TLS. The certificate is checked against the system's trust store
    // unless the caller says otherwise.
    bool use_tls{false};
    bool verify_peer{true};

    std::string terminal_type{"xterm-256color"};
    WindowSize initial_size{24, 80};

    int connect_timeout_seconds{15};

    // How often a keep-alive goes out when nothing else is happening. Zero turns it
    // off: a connection that has gone quiet without a word is a connection that died,
    // and a firewalled path may not want the traffic.
    int keep_alive_seconds{60};

    // rlogin only. The protocol itself carries no password: the server trusts the
    // client, which is why rlogin is only used between machines that already know
    // each other.
    std::string user;
    std::string server_user;
    core::SecureByteArray password;
};

enum class SocketMode {
    // The bytes are the bytes: no protocol at all.
    kRaw,
    // RFC 854, with the negotiation and the window size.
    kTelnet,
    // rlogin: a handshake with a user and a password, and then a shell.
    kRlogin,
};

class SocketChannel : public ITransportChannel {
 public:
    SocketChannel(TcpOptions options, SocketMode mode);
    ~SocketChannel() override;

    SocketChannel(const SocketChannel&) = delete;
    SocketChannel& operator=(const SocketChannel&) = delete;

    [[nodiscard]] TransportState state() const noexcept override;
    void setDataCallback(DataCallback callback) override;
    void setStateCallback(StateCallback callback) override;
    TransportResult connectAsync() override;
    TransportResult disconnect() override;
    void send(const std::uint8_t* data, std::size_t size) override;
    void send(std::string_view text) override;
    bool resizeWindow(const WindowSize& size) override;
    [[nodiscard]] std::string remoteAddress() const override;

    [[nodiscard]] std::size_t readAvailable(std::uint8_t* destination, std::size_t max,
                                            std::chrono::milliseconds timeout);

 protected:
    // The three concrete channels differ in one thing: which conversation they hold
    // with the far end. The machinery below is the same for all of them.
    [[nodiscard]] SocketMode mode() const noexcept { return mode_; }
    [[nodiscard]] const TcpOptions& options() const noexcept { return options_; }

    // Hands the bytes that arrived to the conversation, and collects what has to go
    // back. The raw conversation shows them and says nothing.
    virtual void onIncoming(const std::uint8_t* data, std::size_t size,
                            std::vector<std::uint8_t>& text, std::vector<std::uint8_t>& replies);
    // Called once the socket is up. rlogin uses it to say hello.
    virtual void onConnected(std::vector<std::uint8_t>& to_send, std::string& error);
    // Called with the bytes of a socket that is not connected yet -- the reply of the
    // rlogin handshake. Returns true when the conversation may start.
    virtual bool onEarlyBytes(const std::uint8_t* data, std::size_t size,
                              std::vector<std::uint8_t>& text, std::string& error);
    // Called when the window changed and the peer has to be told. The raw
    // conversation has no window to report.
    virtual void onWindowSizeChanged(const WindowSize& size, std::vector<std::uint8_t>& to_send);

    void setState(TransportState state, const std::string& detail);

    // The settings and the conversation of this channel. They are protected because
    // the three channels below are the same machinery with a different conversation,
    // and they need to read them.
    TcpOptions options_;
    SocketMode mode_;

 private:
    void runReader();
    void stopReader() noexcept;
    void drainOutbox(QSslSocket& socket);

    core::ByteRingBuffer ring_;
    std::atomic<TransportState> state_{TransportState::kIdle};

    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::thread reader_;

    std::mutex outbox_mutex_;
    std::deque<std::uint8_t> outbox_;
    // The window size the UI thread asked for, waiting to be told to the peer.
    bool window_size_pending_{false};
    WindowSize window_size_{24, 80};

    std::mutex callback_mutex_;
    DataCallback data_callback_;
    StateCallback state_callback_;

    bool stopping_{false};
};

// The three conversations.

class RawTcpChannel final : public SocketChannel {
 public:
    explicit RawTcpChannel(TcpOptions options);
    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kRawTcp; }
};

class TelnetChannel final : public SocketChannel {
 public:
    explicit TelnetChannel(TcpOptions options);
    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kTelnet; }

 protected:
    void onIncoming(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& text,
                    std::vector<std::uint8_t>& replies) override;
    void onWindowSizeChanged(const WindowSize& size, std::vector<std::uint8_t>& to_send) override;

 private:
    TelnetCodec codec_;
};

class RloginChannel final : public SocketChannel {
 public:
    explicit RloginChannel(TcpOptions options);
    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kRlogin; }

 protected:
    void onConnected(std::vector<std::uint8_t>& to_send, std::string& error) override;
    bool onEarlyBytes(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& text,
                      std::string& error) override;
    void onWindowSizeChanged(const WindowSize& size, std::vector<std::uint8_t>& to_send) override;

 private:
    bool handshake_done_{false};
    std::string handshake_error_;
};

}  // namespace openscrt::protocols::telnet

#endif  // OPENCRT_PROTOCOLS_TELNET_SOCKET_CHANNEL_H_
