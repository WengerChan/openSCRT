#include "protocols/local/LocalPtyChannel.h"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <utility>

#if defined(_WIN32)
// Already on the command line via the shared warning options; redefining it trips C4005,
// which -Werror turns into a build failure.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps consoleapi.h out of windows.h, and that is where CreatePseudoConsole,
// ResizePseudoConsole and ClosePseudoConsole live.
#include <consoleapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
// macOS and the BSDs ship openpty in libutil, not in <pty.h>.
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
extern char** environ;
#endif

namespace openscrt::protocols::local {
namespace {

std::string defaultShell() {
#if defined(_WIN32)
    if (const char* comspec = std::getenv("COMSPEC")) {
        return comspec;
    }
    return "cmd.exe";
#else
    if (const char* shell = std::getenv("SHELL")) {
        return shell;
    }
    return "/bin/sh";
#endif
}

#if !defined(_WIN32)

// A PTY starts with ECHO on, which would print every local command's own input back at the
// user. Raw mode is what a terminal application is supposed to set up itself.
void applyRawMode(int fd) {
    termios attributes{};
    if (tcgetattr(fd, &attributes) != 0) {
        return;
    }
    attributes.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG);
    // ICRNL stays ON, deliberately. A terminal emulator has to behave like xterm's sane
    // mode: the Enter key sends CR, and the line discipline turns that into NL for the
    // child. Clearing it means a shell waiting for a newline never sees one.
    attributes.c_iflag &= ~static_cast<tcflag_t>(INLCR | ISTRIP);
    attributes.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    // VMIN must be 1, not 0. VMIN=0/VTIME=0 is the setting an *application* uses to poll
    // stdin without blocking; applied to the line discipline it makes every read() return
    // immediately with zero bytes, which a shell reads as EOF and exits on.
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &attributes);
}

bool hasTermEnv() {
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        if (std::string_view(*entry).starts_with("TERM=")) {
            return true;
        }
    }
    return false;
}

#endif  // !_WIN32

}  // namespace

#if defined(_WIN32)

struct LocalPtyChannel::Platform {
    // We write to input_write; the child's stdin reads from it.
    HANDLE input_write{INVALID_HANDLE_VALUE};
    // We read from output_read; the child's stdout/stderr write to it.
    HANDLE output_read{INVALID_HANDLE_VALUE};
    HPCON pseudo_console{nullptr};
    PROCESS_INFORMATION process{};
    std::wstring command_line;
};

#else

struct LocalPtyChannel::Platform {
    int master{-1};
    pid_t child{-1};
};

#endif

LocalPtyChannel::LocalPtyChannel() : LocalPtyChannel(Options{}) {}

LocalPtyChannel::LocalPtyChannel(Options options)
    : options_(std::move(options)), ring_(options_.ring_capacity) {
    if (options_.shell.empty()) {
        options_.shell = defaultShell();
    }
    platform_ = std::make_unique<Platform>();
}

LocalPtyChannel::~LocalPtyChannel() {
    disconnect();
    platform_.reset();
}

void LocalPtyChannel::setState(TransportState state, const std::string& detail) {
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

TransportResult LocalPtyChannel::connectAsync() {
    if (state_.load() == TransportState::kConnected) {
        return TransportResult{true, {}};
    }
    stopping_ = false;
    setState(TransportState::kConnecting, options_.shell);

#if defined(_WIN32)
    // ConPTY: the child attaches to a pseudo console, and the two ends of the pipe are the
    // parent's stdin and stdout. This is what makes PowerShell, cmd and WSL behave like a
    // real console instead of a redirected pipe.
    HANDLE input_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&input_read, &platform_->input_write, nullptr, 0) ||
        !CreatePipe(&platform_->output_read, &output_write, nullptr, 0)) {
        setState(TransportState::kFailed, "CreatePipe failed");
        return TransportResult{false, "CreatePipe failed"};
    }

    COORD size{};
    size.X = static_cast<SHORT>(options_.initial_size.columns);
    size.Y = static_cast<SHORT>(options_.initial_size.rows);
    const HRESULT created =
        CreatePseudoConsole(size, input_read, output_write, 0, &platform_->pseudo_console);
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(created)) {
        setState(TransportState::kFailed, "CreatePseudoConsole failed");
        return TransportResult{false, "CreatePseudoConsole failed"};
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   platform_->pseudo_console, sizeof(HPCON), nullptr, nullptr)) {
        ClosePseudoConsole(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
        setState(TransportState::kFailed, "UpdateProcThreadAttribute failed");
        return TransportResult{false, "UpdateProcThreadAttribute failed"};
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;

    // Build the command line as the shell plus any extra arguments.
    std::wstring command = std::wstring(options_.shell.begin(), options_.shell.end());
    for (const std::string& argument : options_.arguments) {
        command += L" ";
        command += std::wstring(argument.begin(), argument.end());
    }
    platform_->command_line = command;

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    const BOOL created_process =
        CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                       &startup.StartupInfo, &platform_->process);
    DeleteProcThreadAttributeList(attributes);
    if (!created_process) {
        ClosePseudoConsole(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
        setState(TransportState::kFailed, "CreateProcess failed");
        return TransportResult{false, "CreateProcess failed"};
    }

    // The parent must not keep the child's ends open, or the reader never sees EOF.
    CloseHandle(platform_->process.hThread);

    startReader();
    setState(TransportState::kConnected, options_.shell);
    return TransportResult{true, {}};
#else
    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[256]{};
    if (openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) != 0) {
        setState(TransportState::kFailed, "openpty failed");
        return TransportResult{false, "openpty failed"};
    }

    // Apply the geometry before the child inherits the slave, so the shell starts at the right
    // size instead of redrawing on the first resize.
    winsize size{};
    size.ws_row = static_cast<unsigned short>(options_.initial_size.rows);
    size.ws_col = static_cast<unsigned short>(options_.initial_size.columns);
    ioctl(slave_fd, TIOCSWINSZ, &size);

    const pid_t child = fork();
    if (child < 0) {
        ::close(master_fd);
        ::close(slave_fd);
        setState(TransportState::kFailed, "fork failed");
        return TransportResult{false, "fork failed"};
    }

    if (child == 0) {
        // Child: session leader owning the slave as its controlling terminal.
        ::close(master_fd);
        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);
        // The whole point of the PTY: without these the child's stdout stays connected to
        // whatever it inherited, and command output never travels back through the master.
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) {
            ::close(slave_fd);
        }
        if (!options_.working_directory.empty() && chdir(options_.working_directory.c_str()) != 0) {
            _exit(127);
        }
        if (!hasTermEnv()) {
            setenv("TERM", "xterm-256color", 1);
        }

        std::vector<std::string> storage;
        storage.push_back(options_.shell);
        for (const std::string& argument : options_.arguments) {
            storage.push_back(argument);
        }
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& value : storage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);
        execvp(storage.front().c_str(), argv.data());
        _exit(127);
    }

    ::close(slave_fd);
    applyRawMode(master_fd);
    platform_->master = master_fd;
    platform_->child = child;

    startReader();
    setState(TransportState::kConnected, options_.shell);
    return TransportResult{true, {}};
#endif
}

void LocalPtyChannel::startReader() {
    reader_ = std::thread([this] {
        std::uint8_t buffer[4096];
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    break;
                }
            }

            std::size_t size = 0;
#if defined(_WIN32)
            DWORD read = 0;
            if (!ReadFile(platform_->output_read, buffer, sizeof(buffer), &read, nullptr) ||
                read == 0) {
                break;
            }
            size = static_cast<std::size_t>(read);
#else
            pollfd descriptor{platform_->master, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }
            const ssize_t count = ::read(platform_->master, buffer, sizeof(buffer));
            if (count == 0) {
                break;  // EOF: the shell exited.
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            size = static_cast<std::size_t>(count);
#endif

            // Short writes are normal back-pressure; whatever did not fit stays in the
            // socket buffer and the reader picks it up on the next pass.
            ring_.write(buffer, size);

            DataCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = data_callback_;
            }
            if (callback) {
                callback(buffer, size);
            }
            readable_.notify_all();
        }

        // Announce the end before the parser's next read, so it drains what is buffered
        // instead of seeing a closed channel with bytes still pending.
        state_.store(TransportState::kDisconnected);
        readable_.notify_all();
    });
}

void LocalPtyChannel::stopReader() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    readable_.notify_all();
    if (reader_.joinable()) {
        reader_.join();
    }
}

TransportResult LocalPtyChannel::disconnect() {
    if (state_.load() == TransportState::kIdle) {
        return TransportResult{true, {}};
    }
#if defined(_WIN32)
    if (platform_->input_write != INVALID_HANDLE_VALUE) {
        CloseHandle(platform_->input_write);
        platform_->input_write = INVALID_HANDLE_VALUE;
    }
    if (platform_->output_read != INVALID_HANDLE_VALUE) {
        CloseHandle(platform_->output_read);
        platform_->output_read = INVALID_HANDLE_VALUE;
    }
#else
    if (platform_->master >= 0) {
        ::close(platform_->master);
        platform_->master = -1;
    }
#endif
    stopReader();
#if defined(_WIN32)
    if (platform_->pseudo_console != nullptr) {
        ClosePseudoConsole(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
    }
    if (platform_->process.hProcess != nullptr) {
        WaitForSingleObject(platform_->process.hProcess, 2000);
        CloseHandle(platform_->process.hProcess);
        platform_->process.hProcess = nullptr;
    }
#else
    if (platform_->child > 0) {
        int status = 0;
        waitpid(platform_->child, &status, 0);
        platform_->child = -1;
    }
#endif
    setState(TransportState::kDisconnected, "closed");
    return TransportResult{true, {}};
}

std::size_t LocalPtyChannel::readAvailable(std::uint8_t* destination, std::size_t max,
                                           std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (ring_.empty()) {
        if (state_.load() != TransportState::kConnected) {
            // Disconnected *and* drained: there is nothing more coming. Checking the ring
            // before the state is what keeps the final output of a short-lived command from
            // being dropped -- a `sh -c "echo hi; exit"` disconnects within milliseconds.
            break;
        }
        if (readable_.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    lock.unlock();
    return ring_.read(destination, max);
}

std::size_t LocalPtyChannel::readAvailableNow(std::uint8_t* destination, std::size_t max) {
    return ring_.read(destination, max);
}

void LocalPtyChannel::setDataCallback(DataCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    data_callback_ = std::move(callback);
}

void LocalPtyChannel::setStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

void LocalPtyChannel::send(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
#if defined(_WIN32)
    static_cast<void>(size);  // no native transport until ConPTY lands
#endif
#if defined(_WIN32)
    if (platform_->input_write == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(platform_->input_write, data, static_cast<DWORD>(size), &written, nullptr);
#else
    if (platform_->master < 0) {
        return;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(platform_->master, data + written, size - written);
        if (count <= 0) {
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        written += static_cast<std::size_t>(count);
    }
#endif
}

void LocalPtyChannel::send(std::string_view text) {
    send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool LocalPtyChannel::resizeWindow(const WindowSize& size) {
#if defined(_WIN32)
    if (platform_->pseudo_console == nullptr) {
        return false;
    }
    COORD native{};
    native.X = static_cast<SHORT>(size.columns);
    native.Y = static_cast<SHORT>(size.rows);
    return SUCCEEDED(ResizePseudoConsole(platform_->pseudo_console, native));
#else
    if (platform_->master < 0) {
        return false;
    }
    winsize native{};
    native.ws_row = static_cast<unsigned short>(size.rows);
    native.ws_col = static_cast<unsigned short>(size.columns);
    return ioctl(platform_->master, TIOCSWINSZ, &native) == 0;
#endif
}

std::string LocalPtyChannel::remoteAddress() const {
    return "local:" + options_.shell;
}

int LocalPtyChannel::processIdForTesting() const noexcept {
#if defined(_WIN32)
    return platform_ != nullptr ? static_cast<int>(platform_->process.dwProcessId) : -1;
#else
    return platform_ != nullptr ? static_cast<int>(platform_->child) : -1;
#endif
}

}  // namespace openscrt::protocols::local