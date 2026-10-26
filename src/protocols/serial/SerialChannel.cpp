#include "protocols/serial/SerialChannel.h"

#include <QByteArray>
#include <QSerialPort>
#include <QString>
#include <cstddef>
#include <utility>
#include <vector>

#include "protocols/serial/SerialPortSettings.h"

namespace openscrt::protocols::serial {
namespace {

// How long the reader waits for the port before it looks at the stop flag again. The
// wait has a deadline on purpose: a port that never delivers a byte must not keep the
// reader from shutting down.
constexpr int kPollTimeoutMs = 100;

// How long connectAsync() waits for the port to be opened. Opening a device is a local
// operation, so this is generous; it exists so a device that hangs in the kernel cannot
// hang the caller.
constexpr std::chrono::seconds kOpenTimeout{5};

constexpr std::size_t kRingCapacity = 64 * 1024;

// The longest break QSerialPort accepts, and the same limit the hardware has.
constexpr int kMaxBreakMilliseconds = 1000;

}  // namespace

SerialChannel::SerialChannel(SerialSettings settings)
    : settings_(std::move(settings)), ring_(kRingCapacity) {}

SerialChannel::~SerialChannel() {
    disconnect();
}

TransportState SerialChannel::state() const noexcept {
    return state_.load();
}

void SerialChannel::setState(TransportState state, const std::string& detail) {
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

void SerialChannel::setDataCallback(DataCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    data_callback_ = std::move(callback);
}

void SerialChannel::setStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

TransportResult SerialChannel::connectAsync() {
    const TransportState current = state_.load();
    if (current == TransportState::kConnected || current == TransportState::kConnecting) {
        return TransportResult{false, "the serial port is already open"};
    }

    const std::string problem = settings_.validationError();
    if (!problem.empty()) {
        setState(TransportState::kFailed, problem);
        return TransportResult{false, problem};
    }

    // The outcome of the open travels from the reader thread back to this function
    // through a shared handle. It is a local, not a member: a second attempt gets a
    // second handle, so a reader from an earlier attempt can never report into the
    // outcome of a later one.
    std::shared_ptr<OpenOutcome> outcome = std::make_shared<OpenOutcome>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    setState(TransportState::kConnecting, settings_.port_name);

    // A previous attempt may have left a thread behind: assigning to a joinable thread
    // ends the process, so the old one is reaped first. It has already finished -- a
    // failed attempt is the only way to get here with a thread still around.
    if (reader_.joinable()) {
        reader_.join();
    }
    reader_ = std::thread([this, outcome] { runReader(outcome); });

    // Wait for the port to be opened, with a deadline. The wait cannot hang: the reader
    // reports either a result or a failure, and the timeout is the backstop.
    //
    // The lock is released before anything else happens. The reader thread takes this
    // same lock to report its outcome, so joining it while the lock is held would
    // deadlock: the reader would be waiting for a lock the joiner is holding.
    bool reported = false;
    bool opened = false;
    std::string failure;
    {
        std::unique_lock<std::mutex> lock(outcome->mutex);
        reported =
            outcome->signal.wait_for(lock, kOpenTimeout, [outcome] { return outcome->finished; });
        if (reported) {
            opened = outcome->ok;
            failure = outcome->error;
        }
    }
    if (!reported) {
        stopReader();
        const std::string message = "the serial port did not answer in time";
        setState(TransportState::kFailed, message);
        return TransportResult{false, message};
    }
    if (!opened) {
        const std::string message =
            failure.empty() ? "the serial port could not be opened" : failure;
        setState(TransportState::kFailed, message);
        return TransportResult{false, message};
    }
    return TransportResult{true, {}};
}

void SerialChannel::runReader(std::shared_ptr<OpenOutcome> outcome) {
    QSerialPort port;
    port.setPortName(QString::fromStdString(settings_.port_name));

    if (!port.open(QIODevice::ReadWrite)) {
        std::string message = port.errorString().toStdString();
        if (message.empty()) {
            message = "the serial port could not be opened";
        }
        {
            std::lock_guard<std::mutex> lock(outcome->mutex);
            outcome->ok = false;
            outcome->error = message;
            outcome->finished = true;
        }
        outcome->signal.notify_all();
        setState(TransportState::kFailed, message);
        return;
    }

    const std::string refusals = applySerialSettings(port, settings_);
    if (!refusals.empty()) {
        // The port took some of the settings and refused others. Say so instead of
        // pretending the device is running the parameters the session asked for.
        const std::string message = "the serial port refused " + refusals;
        {
            std::lock_guard<std::mutex> lock(outcome->mutex);
            outcome->ok = false;
            outcome->error = message;
            outcome->finished = true;
        }
        outcome->signal.notify_all();
        port.close();
        setState(TransportState::kFailed, message);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        outcome->ok = true;
        outcome->finished = true;
    }
    outcome->signal.notify_all();
    setState(TransportState::kConnected, settings_.port_name);

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                break;
            }
        }

        // Whatever the UI thread queued goes out before the next wait, so a keystroke
        // is not held back by a poll interval.
        drainOutbox(port);
        applyPendingBreak(port);

        if (port.waitForReadyRead(kPollTimeoutMs)) {
            const QByteArray chunk = port.readAll();
            if (!chunk.isEmpty()) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.constData());
                const auto size = static_cast<std::size_t>(chunk.size());
                ring_.write(bytes, size);
                DataCallback callback;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback = data_callback_;
                }
                if (callback) {
                    callback(bytes, size);
                }
                readable_.notify_all();
            }
            continue;
        }

        // A timeout is not an error. Everything else is: the adapter was unplugged, the
        // device was reset, or the port went away underneath us.
        const QSerialPort::SerialPortError port_error = port.error();
        if (port_error != QSerialPort::NoError && port_error != QSerialPort::TimeoutError) {
            setState(TransportState::kDisconnected, port.errorString().toStdString());
            break;
        }
    }

    port.close();
    setState(TransportState::kDisconnected, "closed");
    readable_.notify_all();
}

void SerialChannel::drainOutbox(QSerialPort& port) {
    // The bytes are copied out under the lock rather than written straight from the
    // queue: the UI thread appends to the queue while this runs, and writing from a
    // container another thread is modifying is a race whatever the container promises
    // about its elements.
    std::vector<std::uint8_t> pending;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        if (outbox_.empty()) {
            return;
        }
        pending.assign(outbox_.begin(), outbox_.end());
        outbox_.clear();
    }
    // QSerialPort::write takes the bytes into its own buffer and writes them out from
    // there. A short write is normal back-pressure; the rest is put back and goes out
    // on the next pass.
    const auto written = static_cast<std::size_t>(port.write(
        reinterpret_cast<const char*>(pending.data()), static_cast<qint64>(pending.size())));
    if (written == 0 || written >= pending.size()) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_.insert(outbox_.begin(), pending.begin() + static_cast<std::ptrdiff_t>(written),
                   pending.end());
}

void SerialChannel::applyPendingBreak(QSerialPort& port) {
    int duration = 0;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        if (!break_requested_) {
            return;
        }
        break_requested_ = false;
        duration = break_duration_ms_;
    }
    // Qt 6.7 and 6.8 have no timed break: the line is put into the break condition and
    // stays there until it is taken out again. So the break gets its length here, by
    // holding the condition for the asked time and then letting go. The wait has a
    // deadline -- it is the length of the break, and a break cannot be longer than a
    // second.
    if (!port.setBreakEnabled(true)) {
        // The state does not change: the port is still open and still usable. The
        // detail says what happened, which is what the status bar shows.
        setState(state_.load(), "the serial port refused the break signal");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    port.setBreakEnabled(false);
}

void SerialChannel::stopReader() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    readable_.notify_all();
    if (reader_.joinable()) {
        reader_.join();
    }
}

TransportResult SerialChannel::disconnect() {
    const TransportState current = state_.load();
    if (current == TransportState::kIdle) {
        return TransportResult{true, {}};
    }
    // The reader is stopped before the port is touched: the reader owns the port, and
    // closing a device underneath a thread that is blocked reading it is the race the
    // local shell channel had to be taught about.
    stopReader();
    // The reader sets kDisconnected when it leaves its loop. If it never got that far
    // -- the port could not be opened -- the state stays what it was, because a failed
    // connection is a different thing from a closed one.
    const TransportState after = state_.load();
    if (after == TransportState::kConnected || after == TransportState::kConnecting) {
        setState(TransportState::kDisconnected, "closed");
    }
    return TransportResult{true, {}};
}

void SerialChannel::send(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_.insert(outbox_.end(), data, data + size);
}

void SerialChannel::send(std::string_view text) {
    send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool SerialChannel::sendBreak(int duration_ms) {
    if (duration_ms < 0 || duration_ms > kMaxBreakMilliseconds) {
        return false;
    }
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    break_requested_ = true;
    break_duration_ms_ = duration_ms;
    return true;
}

bool SerialChannel::resizeWindow(const WindowSize& size) {
    // A serial port has no window. The parameter is named so the unused one does not
    // trip the warning about an unused parameter.
    Q_UNUSED(size);
    return false;
}

std::string SerialChannel::remoteAddress() const {
    return settings_.port_name;
}

std::size_t SerialChannel::readAvailable(std::uint8_t* destination, std::size_t max,
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

}  // namespace openscrt::protocols::serial
