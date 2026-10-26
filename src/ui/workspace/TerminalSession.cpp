#include "ui/workspace/TerminalSession.h"

#include <QMetaObject>
#include <chrono>
#include <utility>

#include "protocols/local/LocalPtyChannel.h"
#include "protocols/serial/SerialChannel.h"
#include "protocols/serial/SerialSettings.h"
#include "protocols/ssh/Ssh2Channel.h"
#include "ui/workspace/TerminalCanvasWidget.h"

namespace openscrt::ui {
namespace {

// How long the destructor waits for a connect handshake that is still running. Past that the
// worker is detached: it owns a reference to the channel, so it can finish on its own
// without the session pulling the channel out from under it.
constexpr auto kConnectDrainTimeout = std::chrono::milliseconds(500);
// The wait for a callback that is already on its way. It has a deadline like every
// other wait in this repository: a callback that never comes back must not freeze the
// window. The callback only copies bytes and posts an event, so half a second is
// already far longer than it can need.
constexpr auto kCallbackDrainTimeout = std::chrono::milliseconds(500);
// How long the break signal lasts. A tenth of a second is long enough for the device
// to notice it and short enough that nobody waits for it.
constexpr int kBreakMilliseconds = 100;

// Builds the channel for the profile's protocol. Returns null and fills `error` for the
// protocols that have no implementation yet: Telnet arrives with the Telnet phase, and
// a dialog that silently does nothing is worse than one that says so.
std::shared_ptr<protocols::ITransportChannel> makeChannel(const config::SessionProfile& profile,
                                                          core::SecureByteArray password,
                                                          std::string& error) {
    const config::Protocol protocol = profile.protocol.value_or(config::Protocol::kSsh2);
    switch (protocol) {
        case config::Protocol::kLocalShell:
            return std::make_shared<protocols::local::LocalPtyChannel>();
        case config::Protocol::kSsh2: {
            protocols::ssh::SshConnectionOptions options;
            options.host = profile.host.value_or("");
            options.port = profile.port.value_or(22);
            options.user = profile.user.value_or("");
            options.private_key_path = profile.private_key_path.value_or("");
            // The password travels beside the profile, never inside it: a profile is
            // written to the INI, and a password in the INI is a password on disk.
            options.password = std::move(password);
            if (!options.password.empty() && options.private_key_path.empty()) {
                options.auth_method = protocols::ssh::SshAuthMethod::kPassword;
            }
            return std::make_shared<protocols::ssh::Ssh2Channel>(std::move(options));
        }
        case config::Protocol::kTelnet:
            error = "Telnet is not implemented yet (it arrives with the Telnet phase)";
            return nullptr;
        case config::Protocol::kSerial: {
            protocols::serial::SerialSettings settings;
            settings.port_name = profile.serial_port.value_or("");
            settings.baud_rate = profile.serial_baud_rate.value_or(9600);
            // A session file that names a value this build does not know falls back to
            // the default rather than to something else: a wrong baud rate is a wrong
            // baud rate, and a refused one is a refused one.
            if (profile.serial_data_bits) {
                const auto bits =
                    protocols::serial::serialDataBitsFromString(*profile.serial_data_bits);
                if (bits) {
                    settings.data_bits = *bits;
                }
            }
            if (profile.serial_parity) {
                const auto parity =
                    protocols::serial::serialParityFromString(*profile.serial_parity);
                if (parity) {
                    settings.parity = *parity;
                }
            }
            if (profile.serial_stop_bits) {
                const auto stops =
                    protocols::serial::serialStopBitsFromString(*profile.serial_stop_bits);
                if (stops) {
                    settings.stop_bits = *stops;
                }
            }
            if (profile.serial_flow_control) {
                const auto flow =
                    protocols::serial::serialFlowControlFromString(*profile.serial_flow_control);
                if (flow) {
                    settings.flow_control = *flow;
                }
            }
            return std::make_shared<protocols::serial::SerialChannel>(std::move(settings));
        }
    }
    error = "unknown protocol";
    return nullptr;
}

}  // namespace

TerminalSession::TerminalSession(config::SessionProfile profile, TerminalCanvasWidget* canvas,
                                 core::SecureByteArray password, QObject* parent)
    : QObject(parent),
      profile_(std::move(profile)),
      password_(std::move(password)),
      canvas_(canvas),
      guard_(std::make_shared<CallbackGuard>()),
      connect_state_(std::make_shared<ConnectState>()) {
    if (canvas_ != nullptr) {
        canvas_->setInputSink([this](const std::vector<std::uint8_t>& bytes) { sendInput(bytes); });
        // Ctrl+C is a session action, not text: the widget reports it instead of encoding
        // it, because a terminal sends the interrupt byte to the far end.
        connect(canvas_, &TerminalCanvasWidget::interruptRequested, this,
                [this]() { sendInput(std::vector<std::uint8_t>{0x03}); });
        // Alt+B is a break on the line. Only a serial port has one, and the transport
        // says so by refusing; the channel is looked up when the key is pressed, not
        // now, because it may not exist yet.
        connect(canvas_, &TerminalCanvasWidget::breakRequested, this, [this]() { sendBreak(); });
        // The canvas is owned by the tab widget, not by the session. When the tab goes away
        // the pointer has to stop being valid, or the next byte would be fed to a deleted
        // widget.
        connect(canvas_, &QObject::destroyed, this, [this]() { canvas_ = nullptr; });
    }
}

TerminalSession::~TerminalSession() {
    close();
}

QString TerminalSession::id() const {
    return QString::fromStdString(profile_.id);
}

QString TerminalSession::title() const {
    if (!profile_.name.empty()) {
        return QString::fromStdString(profile_.name);
    }
    if (profile_.host) {
        return QString::fromStdString(*profile_.host);
    }
    return QStringLiteral("session");
}

protocols::TransportState TerminalSession::state() const noexcept {
    if (!channel_) {
        return protocols::TransportState::kIdle;
    }
    return channel_->state();
}

bool TerminalSession::connectSession(std::string& error) {
    if (channel_) {
        error = "the session is already open";
        return false;
    }

    // The guard and the completion flag belong to a connection attempt, not to the
    // session: a session that was closed and is opened again needs fresh ones, or every
    // callback would be rejected and the next close would join a worker that already
    // finished.
    {
        std::lock_guard lock(guard_->mutex);
        if (!guard_->alive) {
            guard_ = std::make_shared<CallbackGuard>();
            connect_state_ = std::make_shared<ConnectState>();
        }
    }

    // The password is handed to the transport and then given up: it is not part of the
    // profile, so nothing that gets written to disk ever sees it.
    std::shared_ptr<protocols::ITransportChannel> channel =
        makeChannel(profile_, std::move(password_), error);
    if (!channel) {
        return false;
    }
    channel_ = channel;

    std::shared_ptr<CallbackGuard> guard = guard_;
    channel_->setDataCallback([this, guard](const std::uint8_t* data, std::size_t size) {
        if (!enterCallback(guard)) {
            return;
        }
        onTransportData(data, size);
        leaveCallback(guard);
    });
    channel_->setStateCallback(
        [this, guard](protocols::TransportState state, const std::string& detail) {
            if (!enterCallback(guard)) {
                return;
            }
            onTransportState(state, detail);
            leaveCallback(guard);
        });

    // The handshake runs off the UI thread: libssh's connect blocks for the whole
    // connection timeout, and the UI thread never waits on the network.
    //
    // The worker shares both the channel and the completion flag with the session. It
    // touches neither the session nor its members, so a session that is destroyed while the
    // handshake is still running cannot be pulled out from under the worker.
    std::shared_ptr<ConnectState> state = connect_state_;
    connect_worker_ = std::thread([channel, state]() {
        channel->connectAsync();
        {
            std::lock_guard lock(state->mutex);
            state->finished = true;
        }
        state->signal.notify_all();
    });
    return true;
}

void TerminalSession::sendInput(const std::vector<std::uint8_t>& bytes) {
    if (!channel_ || bytes.empty()) {
        return;
    }
    channel_->send(bytes.data(), bytes.size());
}

void TerminalSession::sendBreak() {
    if (!channel_) {
        return;
    }
    // The transport that has no break refuses it, and says so by returning false.
    channel_->sendBreak(kBreakMilliseconds);
}

void TerminalSession::close() {
    // The canvas outlives the session for a moment -- the tab widget deletes it on the next
    // event loop turn. Its input sink points at this session, so it goes first, and it goes
    // first even when there was never a channel to close.
    if (canvas_ != nullptr) {
        canvas_->setInputSink(nullptr);
    }
    if (!channel_) {
        return;
    }

    // Clearing the callbacks stops new ones from starting, and the guard makes the ones
    // already on their way wait for the session to finish tearing down. The wait has a
    // deadline: a callback that never comes back must not freeze the window.
    channel_->setDataCallback(nullptr);
    channel_->setStateCallback(nullptr);
    {
        std::unique_lock lock(guard_->mutex);
        guard_->alive = false;
        guard_->idle.wait_for(lock, kCallbackDrainTimeout,
                              [this] { return guard_->in_flight == 0; });
    }

    bool worker_done = true;
    {
        std::unique_lock lock(connect_state_->mutex);
        worker_done = connect_state_->signal.wait_for(lock, kConnectDrainTimeout,
                                                      [this] { return connect_state_->finished; });
    }

    if (connect_worker_.joinable()) {
        if (worker_done) {
            connect_worker_.join();
        } else {
            // The handshake is still running -- an unreachable host can hold it for the
            // whole timeout. The worker owns a reference to the channel and tears it down
            // when it finishes, so the session lets go without touching it.
            connect_worker_.detach();
        }
    }

    if (worker_done && channel_) {
        channel_->disconnect();
    }
    channel_.reset();
}

void TerminalSession::onTransportData(const std::uint8_t* data, std::size_t size) {
    // The transport's reader thread is not the UI thread. The bytes are copied into a queued
    // invocation, so the emulator is only ever driven from the thread that owns the grid.
    // The canvas is the invocation's context: when the tab closes, the pending event goes
    // with it.
    TerminalCanvasWidget* canvas = canvas_;
    if (canvas == nullptr) {
        return;
    }
    const std::vector<std::uint8_t> bytes(data, data + size);
    QMetaObject::invokeMethod(
        canvas, [canvas, bytes]() { canvas->feed(bytes.data(), bytes.size()); },
        Qt::QueuedConnection);
}

void TerminalSession::onTransportState(protocols::TransportState state, const std::string& detail) {
    const int value = static_cast<int>(state);
    const QString text = QString::fromStdString(detail);
    QMetaObject::invokeMethod(
        this, [this, value, text]() { emit stateChanged(value, text); }, Qt::QueuedConnection);
}

bool TerminalSession::enterCallback(const std::shared_ptr<CallbackGuard>& guard) {
    std::lock_guard lock(guard->mutex);
    if (!guard->alive) {
        return false;
    }
    ++guard->in_flight;
    return true;
}

void TerminalSession::leaveCallback(const std::shared_ptr<CallbackGuard>& guard) {
    {
        std::lock_guard lock(guard->mutex);
        --guard->in_flight;
    }
    guard->idle.notify_all();
}

}  // namespace openscrt::ui
