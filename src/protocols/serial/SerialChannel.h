// Serial transport channel (P3-T1).
//
// A QSerialPort presented as an ITransportChannel, so a device on the other end of a
// USB-to-serial adapter is the same thing to the session layer as an SSH server.
//
// The port is owned by the reader thread and by nobody else. QSerialPort is not
// thread-safe, and the UI thread must not touch it: the outbound bytes go into a queue
// that the reader drains, and the reader is the only one who ever calls into Qt.
//
// The reader waits on the port with a bounded timeout rather than on an event loop.
// The wait has a deadline, so a stop request is noticed within one poll interval
// instead of waiting for a byte that may never arrive.

#ifndef OPENCRT_PROTOCOLS_SERIAL_SERIAL_CHANNEL_H_
#define OPENCRT_PROTOCOLS_SERIAL_SERIAL_CHANNEL_H_

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

#include "core/SpscRingBuffer.h"
#include "protocols/ITransportChannel.h"
#include "protocols/serial/SerialSettings.h"

// Forward declared so the reader thread can be handed the port it owns without this
// header pulling in Qt. The port is a Qt class; the channel's own surface is not.
class QSerialPort;

namespace openscrt::protocols::serial {

class SerialChannel final : public ITransportChannel {
 public:
    explicit SerialChannel(SerialSettings settings);
    ~SerialChannel() override;

    SerialChannel(const SerialChannel&) = delete;
    SerialChannel& operator=(const SerialChannel&) = delete;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kSerial; }
    [[nodiscard]] TransportState state() const noexcept override;
    void setDataCallback(DataCallback callback) override;
    void setStateCallback(StateCallback callback) override;
    TransportResult connectAsync() override;
    TransportResult disconnect() override;
    void send(const std::uint8_t* data, std::size_t size) override;
    void send(std::string_view text) override;
    // A serial port has no window, so there is nothing to tell the other end about.
    bool resizeWindow(const WindowSize& size) override;
    [[nodiscard]] std::string remoteAddress() const override;

    // Pull up to `max` bytes, waiting at most `timeout` for the first one. This is the
    // same shape the local shell channel offers, so the parser worker does not have to
    // know which transport it is reading from.
    [[nodiscard]] std::size_t readAvailable(std::uint8_t* destination, std::size_t max,
                                            std::chrono::milliseconds timeout);

    // Sends a break to the device: the line is held in the break condition for
    // `duration_ms`, which is how a serial console interrupts a device that is not
    // listening for an escape character.
    //
    // The request is queued and carried out by the reader thread, because the port
    // belongs to that thread. Returns false when the duration is out of range; the
    // device's own refusal is reported through the state callback.
    bool sendBreak(int duration_ms) override;

    [[nodiscard]] const SerialSettings& settings() const noexcept { return settings_; }

 private:
    // The outcome of opening the port, handed from the reader thread back to the
    // caller of connectAsync().
    struct OpenOutcome {
        std::mutex mutex;
        std::condition_variable signal;
        bool finished{false};
        bool ok{false};
        std::string error;
    };

    void runReader(std::shared_ptr<OpenOutcome> outcome);
    void stopReader() noexcept;
    void setState(TransportState state, const std::string& detail);
    void drainOutbox(QSerialPort& port);
    void applyPendingBreak(QSerialPort& port);

    SerialSettings settings_;
    core::ByteRingBuffer ring_;

    std::atomic<TransportState> state_{TransportState::kIdle};

    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::thread reader_;

    // The bytes waiting to go out, and the reader thread's own copy of the port.
    // Guarded by outbox_mutex_ because the UI thread writes and the reader reads.
    std::mutex outbox_mutex_;
    std::deque<std::uint8_t> outbox_;
    // A break that was asked for and has not been carried out yet. It travels with the
    // same lock as the outbound bytes: both are requests from the UI thread to the
    // reader thread.
    bool break_requested_{false};
    int break_duration_ms_{0};

    std::mutex callback_mutex_;
    DataCallback data_callback_;
    StateCallback state_callback_;

    bool stopping_{false};
};

}  // namespace openscrt::protocols::serial

#endif  // OPENCRT_PROTOCOLS_SERIAL_SERIAL_CHANNEL_H_
