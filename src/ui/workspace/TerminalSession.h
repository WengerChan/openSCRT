// Binds a saved session to a live transport and a terminal canvas (P1-T12).
//
// The transport calls back on its own reader thread, and the screen grid may only be driven
// from the thread that owns it. This class is the seam between the two: bytes that arrive
// off the wire are handed to the UI thread through a queued invocation instead of being
// parsed where they land.
//
// The connect handshake runs on a worker thread as well. libssh's connect blocks for the
// whole connection timeout, and the roadmap is explicit that the UI thread never waits on
// the network.

#ifndef OPENCRT_UI_WORKSPACE_TERMINAL_SESSION_H_
#define OPENCRT_UI_WORKSPACE_TERMINAL_SESSION_H_

#include <QObject>
#include <QString>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config/SessionProfile.h"
#include "protocols/ITransportChannel.h"

namespace openscrt::ui {

class TerminalCanvasWidget;

class TerminalSession : public QObject {
    Q_OBJECT

 public:
    // The canvas is not owned by the session: the tab widget parents it, so closing a tab
    // destroys the widget exactly once. The session only points at it.
    TerminalSession(config::SessionProfile profile, TerminalCanvasWidget* canvas,
                    QObject* parent = nullptr);
    ~TerminalSession() override;

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    [[nodiscard]] const config::SessionProfile& profile() const noexcept { return profile_; }
    [[nodiscard]] QString id() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] protocols::TransportState state() const noexcept;
    [[nodiscard]] TerminalCanvasWidget* canvas() const noexcept { return canvas_; }

    // Builds the channel for the profile's protocol and starts connecting it. Returns false
    // with `error` filled in when the protocol has no implementation yet. A connection that
    // fails afterwards is reported through stateChanged(), not through the return value.
    bool connectSession(std::string& error);

    void sendInput(const std::vector<std::uint8_t>& bytes);
    void close();

 signals:
    // The transport state as an int, because a scoped enum in a signal would drag the
    // protocols header's metatype rules into every consumer of this class.
    void stateChanged(int state, const QString& detail);

 private:
    // Keeps the session alive while a transport callback is running.
    //
    // The transport copies its callback under its own mutex and then invokes it *outside*
    // that mutex, so clearing the callback does not stop a callback that is already on its
    // way. Without this guard the reader thread could post to a session that is being
    // destroyed.
    struct CallbackGuard {
        std::mutex mutex;
        std::condition_variable idle;
        int in_flight{0};
        bool alive{true};
    };

    // The completion flag of the connect handshake. It is shared with the worker thread
    // rather than living in the session, because the worker may outlive the session.
    struct ConnectState {
        std::mutex mutex;
        std::condition_variable signal;
        bool finished{false};
    };

    void onTransportData(const std::uint8_t* data, std::size_t size);
    void onTransportState(protocols::TransportState state, const std::string& detail);

    // Called at the two ends of a transport callback. enterCallback() refuses to start
    // once the session is tearing down, and leaveCallback() is what the destructor waits
    // for. They take the guard by value because the transport may have copied the callback
    // before the session was told to stop: the shared guard is then the only thing that is
    // guaranteed to still be alive.
    [[nodiscard]] static bool enterCallback(const std::shared_ptr<CallbackGuard>& guard);
    static void leaveCallback(const std::shared_ptr<CallbackGuard>& guard);

    config::SessionProfile profile_;
    TerminalCanvasWidget* canvas_{nullptr};

    // Shared with the connect worker: a session torn down mid-handshake lets go of its
    // reference, and the worker keeps the channel alive until the handshake ends.
    std::shared_ptr<protocols::ITransportChannel> channel_;
    std::shared_ptr<CallbackGuard> guard_;
    std::shared_ptr<ConnectState> connect_state_;
    std::thread connect_worker_;
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_TERMINAL_SESSION_H_
