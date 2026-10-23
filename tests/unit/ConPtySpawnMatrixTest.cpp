// Temporary diagnostic (2026-10-24): which spawn shape actually attaches the child to the
// pseudoconsole on the Windows CI runner.
//
// Why this exists: the local shell channel produced no output at all on the Windows runner.
// The child was started, but its stdout was the standard handle it inherited from the test
// process (the CI log pipe) and its stdin was at end of file, so cmd.exe printed its banner
// into the log and exited with code 0. The console host never saw a single byte of it.
//
// Rather than guess which handle arrangement the kernel honours, this runs the same
// pseudoconsole setup with several spawn shapes and reports, for each, whether the child's
// output arrived through the console. The shape that works is the one that goes into the
// production code; this file is deleted afterwards.

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps consoleapi.h out of windows.h, and that is where
// CreatePseudoConsole lives.
#include <consoleapi.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// The three ConPTY entry points are resolved at run time, exactly like the production code
// does: the project targets a Windows version that predates them.
struct ConPtyApi {
    HRESULT (*create)(COORD, HANDLE, HANDLE, DWORD, HPCON*) = nullptr;
    void (*close)(HPCON) = nullptr;
    bool available{false};
};

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
        HMODULE kernel32 = ::LoadLibraryExW(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (kernel32 == nullptr) {
            kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        }
        if (kernel32 == nullptr) {
            return result;
        }
        result.create = resolve<decltype(result.create)>(kernel32, "CreatePseudoConsole");
        result.close = resolve<decltype(result.close)>(kernel32, "ClosePseudoConsole");
        result.available = result.create != nullptr && result.close != nullptr;
        return result;
    }();
    return api;
}

enum class Shape {
    // What the production code does today: no STARTF_USESTDHANDLES, so the child keeps the
    // standard handles it inherits from this process.
    kBaseline,
    // What Windows Terminal does: STARTF_USESTDHANDLES with the standard handles left unset.
    kStartfNullHandles,
    // STARTF_USESTDHANDLES with the pipe ends themselves: the child then talks to the pipes
    // directly and the console host is bypassed.
    kRawPipes,
    // No STARTF_USESTDHANDLES, but this process's own standard handles are blanked for the
    // moment of the spawn.
    kBlankedParentHandles,
};

const char* shapeName(Shape shape) {
    switch (shape) {
        case Shape::kBaseline:
            return "baseline";
        case Shape::kStartfNullHandles:
            return "startf-null-handles";
        case Shape::kRawPipes:
            return "raw-pipes";
        case Shape::kBlankedParentHandles:
            return "blanked-parent-handles";
    }
    return "unknown";
}

struct Outcome {
    bool marker_seen{false};
    std::size_t bytes{0};
    bool child_alive{false};
    unsigned long exit_code{0};
};

Outcome runShape(Shape shape, const std::string& marker) {
    Outcome outcome;

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE input_read = INVALID_HANDLE_VALUE;
    HANDLE input_write = INVALID_HANDLE_VALUE;
    HANDLE output_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;
    if (!::CreatePipe(&input_read, &input_write, &inheritable, 0) ||
        !::CreatePipe(&output_read, &output_write, &inheritable, 0)) {
        std::fprintf(stderr, "[matrix] %s: CreatePipe failed\n", shapeName(shape));
        return outcome;
    }
    ::SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

    COORD size{};
    size.X = 80;
    size.Y = 24;
    HPCON pseudo_console = nullptr;
    const HRESULT created = conPty().create(size, input_read, output_write, 0, &pseudo_console);
    if (FAILED(created)) {
        std::fprintf(stderr, "[matrix] %s: CreatePseudoConsole failed 0x%08lx\n", shapeName(shape),
                     static_cast<unsigned long>(created));
        ::CloseHandle(input_read);
        ::CloseHandle(input_write);
        ::CloseHandle(output_read);
        ::CloseHandle(output_write);
        return outcome;
    }

    SIZE_T attribute_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        std::fprintf(stderr, "[matrix] %s: InitializeProcThreadAttributeList failed\n",
                     shapeName(shape));
        conPty().close(pseudo_console);
        ::CloseHandle(input_read);
        ::CloseHandle(input_write);
        ::CloseHandle(output_read);
        ::CloseHandle(output_write);
        return outcome;
    }
    if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                     pseudo_console, sizeof(HPCON), nullptr, nullptr)) {
        std::fprintf(stderr, "[matrix] %s: UpdateProcThreadAttribute failed\n", shapeName(shape));
        ::DeleteProcThreadAttributeList(attributes);
        conPty().close(pseudo_console);
        ::CloseHandle(input_read);
        ::CloseHandle(input_write);
        ::CloseHandle(output_read);
        ::CloseHandle(output_write);
        return outcome;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;

    if (shape == Shape::kStartfNullHandles || shape == Shape::kRawPipes) {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        if (shape == Shape::kRawPipes) {
            startup.StartupInfo.hStdInput = input_read;
            startup.StartupInfo.hStdOutput = output_write;
            startup.StartupInfo.hStdError = output_write;
        }
    }

    const std::string command = "cmd.exe /c echo " + marker;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    // Blanking this process's own standard handles is only possible for the moment of the
    // spawn; the handles are put back right afterwards.
    HANDLE saved_input = INVALID_HANDLE_VALUE;
    HANDLE saved_output = INVALID_HANDLE_VALUE;
    HANDLE saved_error = INVALID_HANDLE_VALUE;
    if (shape == Shape::kBlankedParentHandles) {
        saved_input = ::GetStdHandle(STD_INPUT_HANDLE);
        saved_output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        saved_error = ::GetStdHandle(STD_ERROR_HANDLE);
        ::SetStdHandle(STD_INPUT_HANDLE, nullptr);
        ::SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
        ::SetStdHandle(STD_ERROR_HANDLE, nullptr);
    }

    PROCESS_INFORMATION process{};
    const BOOL created_process =
        ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                         EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                         nullptr, &startup.StartupInfo, &process);

    if (shape == Shape::kBlankedParentHandles) {
        ::SetStdHandle(STD_INPUT_HANDLE, saved_input);
        ::SetStdHandle(STD_OUTPUT_HANDLE, saved_output);
        ::SetStdHandle(STD_ERROR_HANDLE, saved_error);
    }

    ::DeleteProcThreadAttributeList(attributes);
    ::CloseHandle(input_read);
    ::CloseHandle(output_write);

    if (!created_process) {
        std::fprintf(stderr, "[matrix] %s: CreateProcess failed %lu\n", shapeName(shape),
                     static_cast<unsigned long>(::GetLastError()));
        conPty().close(pseudo_console);
        ::CloseHandle(input_write);
        ::CloseHandle(output_read);
        return outcome;
    }

    // Drain the console output for a few seconds and look for the marker.
    std::string collected;
    char buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!::PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        DWORD read = 0;
        if (!::ReadFile(output_read, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        collected.append(buffer, read);
        if (collected.find(marker) != std::string::npos) {
            break;
        }
    }

    outcome.bytes = collected.size();
    outcome.marker_seen = collected.find(marker) != std::string::npos;
    outcome.child_alive = ::WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT;
    if (!outcome.child_alive) {
        ::GetExitCodeProcess(process.hProcess, &outcome.exit_code);
    }

    std::fprintf(stderr, "[matrix] %-22s marker=%d bytes=%zu alive=%d exit=%lu\n", shapeName(shape),
                 outcome.marker_seen ? 1 : 0, outcome.bytes, outcome.child_alive ? 1 : 0,
                 outcome.exit_code);

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    conPty().close(pseudo_console);
    ::CloseHandle(input_write);
    ::CloseHandle(output_read);
    return outcome;
}

}  // namespace

TEST(ConPtySpawnMatrixTest, ReportsWhichSpawnShapeReachesTheConsole) {
    if (!conPty().available) {
        GTEST_SKIP() << "this system has no ConPTY";
    }

    const Outcome baseline = runShape(Shape::kBaseline, "conpty-marker-baseline");
    const Outcome startf = runShape(Shape::kStartfNullHandles, "conpty-marker-startf");
    const Outcome raw = runShape(Shape::kRawPipes, "conpty-marker-raw");
    const Outcome blanked = runShape(Shape::kBlankedParentHandles, "conpty-marker-blanked");

    // The report is the artefact; the assertions only make a broken run visible.
    EXPECT_TRUE(true) << "baseline=" << baseline.marker_seen << " startf=" << startf.marker_seen
                      << " raw=" << raw.marker_seen << " blanked=" << blanked.marker_seen;
}
