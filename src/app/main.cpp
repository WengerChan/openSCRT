#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "config/ModuleInfo.h"
#include "core/BuildInfo.h"
#include "core/ModuleInfo.h"
#include "core/SecureByteArray.h"
#include "credential/ModuleInfo.h"
#include "protocols/ITransportChannel.h"
#include "protocols/ModuleInfo.h"
#include "scripting/ModuleInfo.h"
#include "terminal/ModuleInfo.h"
#include "transfer/ModuleInfo.h"
#include "ui/ModuleInfo.h"

namespace {

void printVersion() {
    std::printf("openscrt %s\n", openscrt::core::kProjectVersion);
    std::printf("  %-20s %s\n", openscrt::core::moduleName(), openscrt::core::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::terminal::moduleName(),
                openscrt::terminal::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::protocols::moduleName(),
                openscrt::protocols::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::transfer::moduleName(),
                openscrt::transfer::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::credential::moduleName(),
                openscrt::credential::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::scripting::moduleName(),
                openscrt::scripting::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::config::moduleName(), openscrt::config::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::ui::moduleName(), openscrt::ui::moduleVersion());

    // Report the memory-protection degradation state explicitly: P2-A7 requires the user to
    // be told when mlock could not be acquired instead of silently running unprotected.
    openscrt::core::SecureByteArray probe;
    probe.resize(1);
    if (!probe.locked()) {
        std::printf(
            "  note: mlock unavailable (RLIMIT_MEMLOCK); credential buffers will be "
            "zeroed but not pinned\n");
    }
    probe.wipe();
}

#if defined(_WIN32)

// Explorer gives a double-clicked console program a brand-new console that contains nothing but
// that process, which is how "launched by double-click" is recognised: GetConsoleProcessList
// reports 1. Launched from a terminal it is always at least 2, because the shell is attached too.
// Reported as exactly 1 and not "<= 1" on purpose: with no console at all -- a service or a CI
// agent -- the count is 0, and this must stay a no-op there rather than block on a message box.
// Without any of this the 0.1.x build prints two lines and exits, so a double-click shows a
// console window for a few milliseconds and the packaged application looks like it does nothing.
bool launchedByDoubleClick() {
    return ::GetConsoleProcessList(nullptr, 0) == 1;
}

void explainInADialog() {
    ::MessageBoxA(
        nullptr,
        "openSCRT 0.1.0 is a command-line program.\n"
        "\n"
        "This release has no window yet - it is the Phase 0 core, and the Qt workspace shell "
        "arrives in P1-T12. Double-clicking printed a console line that closed immediately, which "
        "is why nothing appeared to happen.\n"
        "\n"
        "Open PowerShell or cmd and run:\n"
        "    openscrt --version\n"
        "\n"
        "Installed at: C:\\Program Files\\openSCRT\\bin\\openscrt.exe",
        "openSCRT", MB_OK | MB_ICONINFORMATION);
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    // Command line parsing compatible with SecureCRT's /S, /SSH2, /SCRIPT switches lands in
    // P1-T12 together with the session manager.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        }
    }

    std::printf("openscrt %s - terminal emulator bootstrap (Phase 0)\n",
                openscrt::core::kProjectVersion);
    std::printf(
        "The Qt workspace shell arrives in P1-T12; run with --version for build details.\n");

#if defined(_WIN32)
    if (launchedByDoubleClick()) {
        explainInADialog();
    }
#endif

    return 0;
}