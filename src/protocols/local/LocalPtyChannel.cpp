#include "protocols/local/LocalPtyChannel.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
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

namespace {

// The three ConPTY entry points, resolved at run time.
//
// CreatePseudoConsole, ResizePseudoConsole and ClosePseudoConsole only exist from
// Windows 10 1809. Calling them directly puts them in the program's import table, and a
// machine without them then dies in the loader -- before main(), with a dialog that says
// "the procedure entry point ... could not be located" and nothing else. Resolving them
// here keeps the program startable everywhere: on an older system the local shell reports
// that it needs a newer Windows, and the rest of the application still runs.
//
// The floor this leaves behind is not ours to decide alone: the Qt libraries in the
// package import symbols of their own. See the import table gate in CI, which measures
// the real floor rather than trusting a documented one.
struct ConPtyApi {
    HRESULT (*create)(COORD, HANDLE, HANDLE, DWORD, HPCON*) = nullptr;
    HRESULT (*resize)(HPCON, COORD) = nullptr;
    void (*close)(HPCON) = nullptr;
    bool available{false};
};

// MSVC warns when a FARPROC is cast to a differently typed function pointer, and the
// project builds with warnings as errors. Converting through a union says the same thing
// without the cast the warning is about.
template <typename Function>
Function resolve(HMODULE module, const char* name) {
    union {
        FARPROC generic;
        Function specific;
    } converter;
    converter.generic = ::GetProcAddress(module, name);
    return converter.specific;
}

const ConPtyApi& conPty() {
    static const ConPtyApi api = [] {
        ConPtyApi result;
        // kernel32.dll is a known DLL, so it is always the one in System32; asking for it
        // by name with the system32 search says so explicitly rather than relying on it.
        HMODULE kernel32 = ::LoadLibraryExW(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (kernel32 == nullptr) {
            kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        }
        if (kernel32 == nullptr) {
            return result;
        }
        result.create = resolve<decltype(result.create)>(kernel32, "CreatePseudoConsole");
        result.resize = resolve<decltype(result.resize)>(kernel32, "ResizePseudoConsole");
        result.close = resolve<decltype(result.close)>(kernel32, "ClosePseudoConsole");
        result.available =
            result.create != nullptr && result.resize != nullptr && result.close != nullptr;
        return result;
    }();
    return api;
}

}  // namespace

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
    if (!conPty().available) {
        setState(TransportState::kFailed,
                 "the local shell needs Windows 10 1809 or later: this system has no ConPTY");
        return TransportResult{
            false, "the local shell needs Windows 10 1809 or later: this system has no ConPTY"};
    }
    // The console host runs in its own process and reaches these two ends by handle
    // inheritance, so they have to be inheritable. The parent's own ends are marked
    // non-inheritable right after, or the child would keep them open and the reader
    // would never see the end of the stream.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE input_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&input_read, &platform_->input_write, &inheritable, 0) ||
        !CreatePipe(&platform_->output_read, &output_write, &inheritable, 0)) {
        setState(TransportState::kFailed, "CreatePipe failed");
        return TransportResult{false, "CreatePipe failed"};
    }
    SetHandleInformation(platform_->input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(platform_->output_read, HANDLE_FLAG_INHERIT, 0);

    COORD size{};
    size.X = static_cast<SHORT>(options_.initial_size.columns);
    size.Y = static_cast<SHORT>(options_.initial_size.rows);
    const HRESULT created =
        conPty().create(size, input_read, output_write, 0, &platform_->pseudo_console);
    if (FAILED(created)) {
        CloseHandle(input_read);
        CloseHandle(output_write);
        setState(TransportState::kFailed, "CreatePseudoConsole failed");
        return TransportResult{false, "CreatePseudoConsole failed"};
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    // The list has to be initialised before it can be filled in. Asking for the size is
    // not the same thing as initialising it, and an uninitialised list makes
    // UpdateProcThreadAttribute fail -- which is exactly what it did here, so the local
    // shell channel never started on Windows at all.
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        CloseHandle(input_read);
        CloseHandle(output_write);
        conPty().close(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
        setState(TransportState::kFailed, "InitializeProcThreadAttributeList failed");
        return TransportResult{false, "InitializeProcThreadAttributeList failed"};
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   platform_->pseudo_console, sizeof(HPCON), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(input_read);
        CloseHandle(output_write);
        conPty().close(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
        setState(TransportState::kFailed, "UpdateProcThreadAttribute failed");
        return TransportResult{false, "UpdateProcThreadAttribute failed"};
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    // STARTF_USESTDHANDLES with the standard handles left unset is what Windows Terminal
    // does, and it is what makes the difference between a child that joins the console and
    // a child that keeps the handles it inherited from this process. Without the flag the
    // child inherits the parent's standard handles, so its output goes wherever the parent's
    // output goes -- into the CI log, in the runner's case -- and its input arrives from the
    // parent's input, which is already at end of file. The console then sees nothing at all.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

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
    // The pseudoconsole keeps its own copies of the pipe ends. Releasing ours only now
    // is what the documentation asks for: closing them before the child is created
    // leaves the console host without a working channel, the child then fails to attach
    // and silently runs with the parent's inherited handles instead of the console.
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (!created_process) {
        conPty().close(platform_->pseudo_console);
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
            // PeekNamedPipe reports what is waiting without blocking, which is what the
            // POSIX branch gets from poll. A blocking read would leave the thread stuck in
            // the kernel for the whole shutdown, and the join in stopReader() would then
            // never return.
            DWORD available = 0;
            if (!PeekNamedPipe(platform_->output_read, nullptr, 0, nullptr, &available, nullptr)) {
                break;  // The pipe is gone: the console is closed.
            }
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
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
    // The order of these steps is load-bearing, and the documentation says why:
    //
    //   "The act of closing the pseudoconsole session may emit a final frame update to
    //    hOutput which should be drained from the communications channel buffer."
    //
    // The console host writes that last frame while it shuts down, and ClosePseudoConsole
    // waits for that write to go through. A reader that has already stopped turns the
    // shutdown into a deadlock -- which is exactly what this path did: the test binary
    // hung inside ClosePseudoConsole, with the reader gone and nobody left to drain the
    // pipe. So the console is closed while the reader is still running, and only then is
    // the reader stopped; by then the console host is gone, the output pipe is at end of
    // file, and the reader leaves on its own.
    if (platform_->input_write != INVALID_HANDLE_VALUE) {
        CloseHandle(platform_->input_write);
        platform_->input_write = INVALID_HANDLE_VALUE;
    }
    if (platform_->pseudo_console != nullptr) {
        conPty().close(platform_->pseudo_console);
        platform_->pseudo_console = nullptr;
    }
    stopReader();
    if (platform_->output_read != INVALID_HANDLE_VALUE) {
        CloseHandle(platform_->output_read);
        platform_->output_read = INVALID_HANDLE_VALUE;
    }
    if (platform_->process.hProcess != nullptr) {
        WaitForSingleObject(platform_->process.hProcess, 2000);
        CloseHandle(platform_->process.hProcess);
        platform_->process.hProcess = nullptr;
    }
#else
    if (platform_->master >= 0) {
        ::close(platform_->master);
        platform_->master = -1;
    }
    stopReader();
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
    if (platform_->input_write == INVALID_HANDLE_VALUE) {
        return;
    }
    // The console reads the input as a byte stream of key presses. Microsoft's own ConPTY
    // sample writes plain bytes into this pipe, so that is what the console expects.
    DWORD written = 0;
    // A short write means the console's input queue was full; the rest of the input is
    // dropped, the same as any terminal whose input queue is full.
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
    return SUCCEEDED(conPty().resize(platform_->pseudo_console, native));
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