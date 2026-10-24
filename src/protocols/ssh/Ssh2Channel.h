// SSH2 transport channel (P1-T11).
//
// Wraps libssh in the ITransportChannel contract: one thread reading from the SSH channel
// into the lock-free ring, writes going straight out. Authentication covers the three
// methods the roadmap names -- password, public key, and keyboard-interactive for
// two-factor setups.
//
// libssh's own types stay behind this header; nothing above it needs to know the library
// exists.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH2_CHANNEL_H_
#define OPENCRT_PROTOCOLS_SSH_SSH2_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/SecureByteArray.h"
#include "core/SpscRingBuffer.h"
#include "protocols/ITransportChannel.h"

namespace openscrt::protocols::ssh {

enum class SshAuthMethod {
    kPassword,
    kPublicKey,
    kKeyboardInteractive,
    // Try public key first, then password, then keyboard-interactive.
    kAutomatic,
};

struct SshConnectionOptions {
    std::string host;
    int port{22};
    std::string user;

    // The password never goes into a profile: a profile is written to the INI. It travels
    // in this locked buffer instead, and the buffer is wiped when the attempt is over.
    core::SecureByteArray password;
    // OpenSSH or PEM private key. For OpenSSH format keys the public half is derived from
    // the same file.
    std::string private_key_path;
    std::string private_key_passphrase;

    SshAuthMethod auth_method{SshAuthMethod::kAutomatic};
    WindowSize initial_size{24, 80};
    int timeout_seconds{15};
    std::string terminal_type{"xterm-256color"};
};

class Ssh2Channel final : public ITransportChannel {
 public:
    explicit Ssh2Channel(SshConnectionOptions options);
    ~Ssh2Channel() override;

    Ssh2Channel(const Ssh2Channel&) = delete;
    Ssh2Channel& operator=(const Ssh2Channel&) = delete;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::kSsh2; }
    [[nodiscard]] TransportState state() const noexcept override { return state_.load(); }
    void setDataCallback(DataCallback callback) override;
    void setStateCallback(StateCallback callback) override;
    TransportResult connectAsync() override;
    TransportResult disconnect() override;
    void send(const std::uint8_t* data, std::size_t size) override;
    void send(std::string_view text) override;
    // Sends a NAWS request so the remote application learns the new size.
    bool resizeWindow(const WindowSize& size) override;
    [[nodiscard]] std::string remoteAddress() const override;

    [[nodiscard]] std::size_t readAvailable(std::uint8_t* destination, std::size_t max,
                                            std::chrono::milliseconds timeout);

 private:
    struct Session;  // Owns the libssh handles; defined in the .cpp.

    void startReader();
    void stopReader() noexcept;
    void setState(TransportState state, const std::string& detail);
    [[nodiscard]] bool authenticate(Session& session, std::string& error);

    SshConnectionOptions options_;
    std::unique_ptr<Session> session_;
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

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH2_CHANNEL_H_