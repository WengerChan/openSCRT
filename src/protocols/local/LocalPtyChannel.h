// Local shell channel (P1-T10).
//
// Windows uses ConPTY so that PowerShell, cmd and WSL all behave like a real console;
// macOS and Linux use forkpty(). Either way the shape is the same: one reader thread
// pumping master-side bytes into a lock-free ring, and writes going straight to the slave.
//
// The ring exists for two consumers with different needs. The parser worker wants a blocking
// pull with a timeout; the UI layer wants to drain everything available right now. Both read
// the same bytes, so the callback and the ring cannot drift apart.

#ifndef OPENCRT_PROTOCOLS_LOCAL_LOCAL_PTY_CHANNEL_H_
#define OPENCRT_PROTOCOLS_LOCAL_LOCAL_PTY_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/SpscRingBuffer.h"
#include "protocols/ITransportChannel.h"

namespace openscrt::protocols::local {

class LocalPtyChannel final : public ITransportChannel {
 public:
    struct Options {
        // Empty means $SHELL, then /bin/sh. Passing an explicit path keeps the channel
        // deterministic for tests and for session files that pin a shell.
        std::string shell;
        std::vector<std::string> arguments;  // Extra argv after argv[0].
        std::string working_directory;
        WindowSize initial_size{24, 80};
        std::size_t ring_capacity{64 * 1024};
    };

    explicit LocalPtyChannel(Options options = Options{});
    ~LocalPtyChannel() override;

    LocalPtyChannel(const LocalPtyChannel&) = delete;
    LocalPtyChannel& operator=(const LocalPtyChannel&) = delete;

    // Pull up to `max` bytes, waiting at most `timeout` for the first one. Returns 0 on
    // timeout or after disconnect. This is what the parser worker calls.
    [[nodiscard]] std::size_t readAvailable(std::uint8_t* destination, std::size_t max,
                                            std::chrono::milliseconds timeout);

    // Drain whatever is buffered without waiting.
    [[nodiscard]] std::size_t readAvailableNow(std::uint8_t* destination, std::size_t max);

    // ITransportChannel
    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kLocalPty; }
    [[nodiscard]] TransportState state() const noexcept override { return state_.load(); }
    void setDataCallback(DataCallback callback) override;
    void setStateCallback(StateCallback callback) override;
    TransportResult connectAsync() override;
    TransportResult disconnect() override;
    void send(const std::uint8_t* data, std::size_t size) override;
    void send(std::string_view text) override;
    // ConPTY and forkpty both forward the size to the child, so this is supported here.
    bool resizeWindow(const WindowSize& size) override;
    [[nodiscard]] std::string remoteAddress() const override;

    // Child pid, or -1. Exists so the tests can assert that a second connect() does not spawn
    // a second shell; production code should not need it.
    [[nodiscard]] int processIdForTesting() const noexcept;

 private:
    void startReader();
    void stopReader() noexcept;
    void setState(TransportState state, const std::string& detail);

    struct Platform;  // Owns the native handles; defined per platform in the .cpp.
    std::unique_ptr<Platform> platform_;

    Options options_;
    core::ByteRingBuffer ring_;

    std::atomic<TransportState> state_{TransportState::kIdle};
    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::thread reader_;

    std::mutex callback_mutex_;
    DataCallback data_callback_;
    StateCallback state_callback_;

    bool stopping_{false};
};

}  // namespace openscrt::protocols::local

#endif  // OPENCRT_PROTOCOLS_LOCAL_LOCAL_PTY_CHANNEL_H_