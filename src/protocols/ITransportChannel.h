// Frozen transport interface (P1-T9).
//
// Every session type -- SSH2, serial, Telnet, raw TCP, local PTY -- presents the same
// byte-stream surface to the terminal emulator. Anything a channel cannot do (a serial port
// has no window size; local PTY cannot forward X11) is expressed as an unsupported-operation
// result rather than a separate type, so the session manager stays protocol-agnostic.
//
// FRESHNESS CONTRACT: this header is reviewed once, at P1. Downstream phases may add
// *capability* declarations but must not change existing signatures; adding a pure virtual
// after Phase 2 is considered a breaking change.

#ifndef OPENCRT_PROTOCOLS_I_TRANSPORT_CHANNEL_H_
#define OPENCRT_PROTOCOLS_I_TRANSPORT_CHANNEL_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace openscrt::protocols {

enum class TransportState {
    kIdle,
    kConnecting,
    kConnected,
    kReconnecting,
    kDisconnected,
    kFailed,
};

enum class TransportKind {
    kSsh2,
    kSerial,
    kTelnet,
    kRlogin,
    kRawTcp,
    kLocalPty,
};

const char* toString(TransportState state) noexcept;
const char* toString(TransportKind kind) noexcept;

struct TransportResult {
    bool ok{false};
    std::string error;  // Empty when ok.
};

// Geometry advertised to the remote end. Serial and local-PTY channels may ignore rows/cols.
struct WindowSize {
    int rows{24};
    int columns{80};
    int pixels_x{0};
    int pixels_y{0};

    friend bool operator==(const WindowSize& a, const WindowSize& b) noexcept {
        return a.rows == b.rows && a.columns == b.columns && a.pixels_x == b.pixels_x &&
               a.pixels_y == b.pixels_y;
    }
};

class ITransportChannel {
public:
    // Bytes arriving from the remote end. Invoked on the channel's own I/O worker thread,
    // never on the UI thread: the parser worker owns the consuming side.
    using DataCallback = std::function<void(const std::uint8_t* data, std::size_t size)>;
    // State transitions and human-readable diagnostics for the tab header icon / status bar.
    using StateCallback = std::function<void(TransportState state, const std::string& detail)>;

    virtual ~ITransportChannel() = default;

    ITransportChannel(const ITransportChannel&) = delete;
    ITransportChannel& operator=(const ITransportChannel&) = delete;

    [[nodiscard]] virtual TransportKind kind() const noexcept = 0;
    [[nodiscard]] virtual TransportState state() const noexcept = 0;

    virtual void setDataCallback(DataCallback callback) = 0;
    virtual void setStateCallback(StateCallback callback) = 0;

    // Resolves to kConnected, or an error string the dialog can display verbatim.
    virtual TransportResult connectAsync() = 0;
    virtual TransportResult disconnect() = 0;

    // Queues bytes for the remote end. Local echo is handled by the PTY driver, not here.
    virtual void send(const std::uint8_t* data, std::size_t size) = 0;
    virtual void send(std::string_view text) = 0;

    // Triggers the protocol-specific SIGWINCH / NAWS notification. Returns false for
    // transports with no window concept (serial).
    virtual bool resizeWindow(const WindowSize& size) = 0;

    [[nodiscard]] virtual std::string remoteAddress() const = 0;

protected:
    ITransportChannel() = default;
};

using TransportChannelPtr = std::unique_ptr<ITransportChannel>;

// Factory seam so the session manager can build a channel from a saved session descriptor
// without depending on libssh / QSerialPort directly.
class ITransportChannelFactory {
public:
    virtual ~ITransportChannelFactory() = default;
    virtual TransportChannelPtr create() const = 0;
};

}  // namespace openscrt::protocols

#endif  // OPENCRT_PROTOCOLS_I_TRANSPORT_CHANNEL_H_