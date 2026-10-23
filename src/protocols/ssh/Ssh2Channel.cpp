#include "protocols/ssh/Ssh2Channel.h"

#include <libssh/libssh.h>

#include <utility>

namespace openscrt::protocols::ssh {
namespace {

constexpr std::size_t kReadChunk = 8192;
// How long the reader waits for data before checking the stop flag again.
constexpr int kPollTimeoutMs = 100;

}  // namespace

struct Ssh2Channel::Session {
    ssh_session session{nullptr};
    ssh_channel channel{nullptr};
    ssh_key private_key{nullptr};
    // libssh keeps the pointer for SSH_OPTIONS_PORT and SSH_OPTIONS_TIMEOUT, so the values
    // have to outlive the option_set calls and the connect. Their types are fixed by
    // libssh's options struct: uint16_t for the port, unsigned long for the timeout.
    std::uint16_t port{22};
    unsigned long timeout{15};
    int strict_host_key_check{0};
};

Ssh2Channel::Ssh2Channel(SshConnectionOptions options)
    : options_(std::move(options)), ring_(64 * 1024) {
    session_ = std::make_unique<Session>();
    session_->port = static_cast<std::uint16_t>(options_.port);
    session_->timeout = static_cast<unsigned long>(options_.timeout_seconds);
}

Ssh2Channel::~Ssh2Channel() {
    disconnect();
    session_.reset();
}

void Ssh2Channel::setState(TransportState state, const std::string& detail) {
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

bool Ssh2Channel::authenticate(Session& session, std::string& error) {
    const auto password_only = options_.auth_method == SshAuthMethod::kPassword;
    const auto key_only = options_.auth_method == SshAuthMethod::kPublicKey;
    const auto keyboard_only = options_.auth_method == SshAuthMethod::kKeyboardInteractive;

    // Public key first: it is the only method that works unattended.
    if (!password_only && !options_.private_key_path.empty()) {
        // libssh 0.11 takes the file name and writes the key through an out-parameter; the
        // old ssh_pki_import_privkey_file(session, ...) signature is gone.
        const int imported = ssh_pki_import_privkey_file(
            options_.private_key_path.c_str(),
            options_.private_key_passphrase.empty() ? nullptr
                                                    : options_.private_key_passphrase.c_str(),
            nullptr, nullptr, &session.private_key);
        if (imported == SSH_OK && session.private_key != nullptr) {
            if (ssh_userauth_publickey(session.session, nullptr, session.private_key) ==
                SSH_AUTH_SUCCESS) {
                return true;
            }
        } else if (key_only) {
            error = "could not read the private key file";
            return false;
        }
    }

    if (!key_only && !options_.password.empty()) {
        if (ssh_userauth_password(session.session, nullptr, options_.password.c_str()) ==
            SSH_AUTH_SUCCESS) {
            return true;
        }
    }

    if (!key_only && !password_only && !keyboard_only) {
        // Keyboard-interactive is how most two-factor setups answer. libssh 0.11 replaced the
        // one-shot ssh_userauth_keyboard_interactive with a request/answer exchange:
        // ask for the prompts, fill them in, then send the answers.
        if (ssh_userauth_kbdint(session.session, nullptr, nullptr) == SSH_AUTH_SUCCESS) {
            const int prompts = ssh_userauth_kbdint_getnprompts(session.session);
            for (int index = 0; index < prompts; ++index) {
                char echo = 0;
                static_cast<void>(ssh_userauth_kbdint_getprompt(
                    session.session, static_cast<unsigned int>(index), &echo));
                ssh_userauth_kbdint_setanswer(session.session, static_cast<unsigned int>(index),
                                              options_.password.c_str());
            }
            // The second call is what actually transmits the answers.
            if (ssh_userauth_kbdint(session.session, nullptr, nullptr) == SSH_AUTH_SUCCESS) {
                return true;
            }
        }
    }

    if (error.empty()) {
        error = "authentication failed";
    }
    return false;
}

TransportResult Ssh2Channel::connectAsync() {
    if (state_.load() == TransportState::kConnected) {
        return TransportResult{true, {}};
    }
    if (options_.host.empty()) {
        setState(TransportState::kFailed, "no host");
        return TransportResult{false, "no host"};
    }

    stopping_ = false;
    setState(TransportState::kConnecting, options_.host);

    Session& session = *session_;
    session.session = ssh_new();
    if (session.session == nullptr) {
        setState(TransportState::kFailed, "ssh_new failed");
        return TransportResult{false, "ssh_new failed"};
    }

    ssh_options_set(session.session, SSH_OPTIONS_HOST, options_.host.c_str());
    ssh_options_set(session.session, SSH_OPTIONS_USER,
                    options_.user.empty() ? nullptr : options_.user.c_str());
    ssh_options_set(session.session, SSH_OPTIONS_PORT, &session.port);
    ssh_options_set(session.session, SSH_OPTIONS_TIMEOUT, &session.timeout);
    // The host key manager lands in Phase 4 (P4-T8). Until then the host key is not
    // verified, which is stated here rather than left as a silent default.
    ssh_options_set(session.session, SSH_OPTIONS_STRICTHOSTKEYCHECK,
                    &session.strict_host_key_check);

    const int connected = ssh_connect(session.session);
    if (connected != SSH_OK) {
        const std::string reason = ssh_get_error(session.session);
        ssh_free(session.session);
        session.session = nullptr;
        setState(TransportState::kFailed, reason);
        return TransportResult{false, reason.empty() ? "ssh_connect failed" : reason};
    }

    std::string error;
    if (!authenticate(session, error)) {
        ssh_disconnect(session.session);
        ssh_free(session.session);
        session.session = nullptr;
        setState(TransportState::kFailed, error);
        return TransportResult{false, error};
    }

    session.channel = ssh_channel_new(session.session);
    if (session.channel == nullptr) {
        ssh_disconnect(session.session);
        ssh_free(session.session);
        session.session = nullptr;
        setState(TransportState::kFailed, "ssh_channel_new failed");
        return TransportResult{false, "ssh_channel_new failed"};
    }
    if (ssh_channel_open_session(session.channel) != SSH_OK) {
        const std::string reason = ssh_get_error(session.session);
        ssh_channel_free(session.channel);
        ssh_disconnect(session.session);
        ssh_free(session.session);
        session.channel = nullptr;
        session.session = nullptr;
        setState(TransportState::kFailed, reason);
        return TransportResult{false, reason.empty() ? "ssh_channel_open_session failed" : reason};
    }

    ssh_channel_request_pty_size(session.channel, options_.terminal_type.c_str(),
                                 options_.initial_size.columns, options_.initial_size.rows);
    if (ssh_channel_request_shell(session.channel) != SSH_OK) {
        const std::string reason = ssh_get_error(session.session);
        ssh_channel_free(session.channel);
        ssh_disconnect(session.session);
        ssh_free(session.session);
        session.channel = nullptr;
        session.session = nullptr;
        setState(TransportState::kFailed, reason);
        return TransportResult{false, reason.empty() ? "ssh_channel_request_shell failed" : reason};
    }

    startReader();
    setState(TransportState::kConnected, options_.host);
    return TransportResult{true, {}};
}

void Ssh2Channel::startReader() {
    reader_ = std::thread([this] {
        std::uint8_t buffer[kReadChunk];
        Session& session = *session_;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_ || session.channel == nullptr) {
                    break;
                }
            }
            // poll_timeout blocks for a bounded interval, so the reader notices a stop
            // request without spinning. libssh 0.11 has no SSH_STATUS_* enum; the poll
            // return value is the only signal.
            const int ready = ssh_channel_poll_timeout(session.channel, kPollTimeoutMs, 0);
            if (ready < 0) {
                break;  // EOF or error: the remote closed the channel.
            }
            if (ready == 0) {
                continue;  // Timed out; check the stop flag and wait again.
            }
            const int count = ssh_channel_read(session.channel, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            ring_.write(buffer, static_cast<std::size_t>(count));
            DataCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = data_callback_;
            }
            if (callback) {
                callback(buffer, static_cast<std::size_t>(count));
            }
            readable_.notify_all();
        }
        state_.store(TransportState::kDisconnected);
        readable_.notify_all();
    });
}

void Ssh2Channel::stopReader() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    readable_.notify_all();
    if (reader_.joinable()) {
        reader_.join();
    }
}

TransportResult Ssh2Channel::disconnect() {
    if (state_.load() == TransportState::kIdle) {
        return TransportResult{true, {}};
    }
    Session& session = *session_;
    // Closing the channel unblocks a reader that is waiting for data.
    if (session.channel != nullptr) {
        ssh_channel_close(session.channel);
        ssh_channel_free(session.channel);
        session.channel = nullptr;
    }
    stopReader();
    if (session.private_key != nullptr) {
        ssh_key_free(session.private_key);
        session.private_key = nullptr;
    }
    if (session.session != nullptr) {
        ssh_disconnect(session.session);
        ssh_free(session.session);
        session.session = nullptr;
    }
    setState(TransportState::kDisconnected, "closed");
    return TransportResult{true, {}};
}

void Ssh2Channel::send(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    Session& session = *session_;
    if (session.channel == nullptr) {
        return;
    }
    ssh_channel_write(session.channel, data, static_cast<std::uint32_t>(size));
}

void Ssh2Channel::send(std::string_view text) {
    send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool Ssh2Channel::resizeWindow(const WindowSize& size) {
    Session& session = *session_;
    if (session.channel == nullptr) {
        return false;
    }
    return ssh_channel_request_pty_size(session.channel, options_.terminal_type.c_str(),
                                        size.columns, size.rows) == SSH_OK;
}

std::string Ssh2Channel::remoteAddress() const {
    return options_.user.empty()
               ? options_.host
               : options_.user + "@" + options_.host + ":" + std::to_string(options_.port);
}

std::size_t Ssh2Channel::readAvailable(std::uint8_t* destination, std::size_t max,
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

}  // namespace openscrt::protocols::ssh