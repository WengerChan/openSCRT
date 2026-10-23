#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDockWidget>
#include <QMenuBar>
#include <QTemporaryDir>

#include "config/ModuleInfo.h"
#include "config/SessionProfile.h"
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
#include "ui/workspace/MainWindow.h"
#include "ui/workspace/WorkspaceTabs.h"

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

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], flag) == 0) {
            return true;
        }
    }
    return false;
}

#if defined(_WIN32)

// Explorer gives a double-clicked console program a brand-new console that contains nothing but
// that process, which is how "launched by double-click" is recognised: GetConsoleProcessList
// reports 1. Launched from a terminal it is always at least 2, because the shell is attached too.
// Reported as exactly 1 and not "<= 1" on purpose: with no console at all -- a service or a CI
// agent -- the count is 0, and this must stay a no-op there rather than block on a message box.
bool launchedByDoubleClick() {
    return ::GetConsoleProcessList(nullptr, 0) == 1;
}

// A GUI application should not leave a console window behind it. The console is only hidden
// when the program was double-clicked: started from a terminal, the user may want the output
// and the shell prompt back.
void hideConsoleIfDoubleClicked() {
    if (launchedByDoubleClick()) {
        HWND console = ::GetConsoleWindow();
        if (console != nullptr) {
            ::ShowWindow(console, SW_HIDE);
        }
    }
}

#endif  // _WIN32

// Builds the window, shows it and opens a tab, without touching the user's real
// configuration. It is the smoke test the release pipeline runs: it proves the shell
// assembles on a machine with no display.
int runSelfTest(QApplication& app) {
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::fprintf(stderr, "self-test: cannot create a scratch directory\n");
        return 1;
    }

    openscrt::ui::MainWindow window;
    window.setConfigPath(std::string(scratch.path().toStdString()) + "/sessions.ini");
    window.loadSessions();
    window.show();
    app.processEvents();

    if (window.menuBar()->actions().isEmpty()) {
        std::fprintf(stderr, "self-test: the window has no menu bar\n");
        return 1;
    }
    if (window.findChildren<QDockWidget*>().size() < 2) {
        std::fprintf(stderr,
                     "self-test: the Session Manager and Active Sessions docks are missing\n");
        return 1;
    }
    if (window.sessionManager() == nullptr || window.workspace() == nullptr) {
        std::fprintf(stderr, "self-test: the workspace did not assemble\n");
        return 1;
    }

    // A tab has to come up. The transport behind it is deliberately not connected here: an
    // SSH session without a host fails immediately and deterministically on every
    // platform, which is what this check needs -- the tab is the workspace's own work, and
    // the connection belongs to the unit tests.
    openscrt::config::SessionProfile profile;
    profile.id = "self-test";
    profile.name = "self-test";
    profile.protocol = openscrt::config::Protocol::kSsh2;
    std::string error;
    if (window.workspace()->openSession(profile, error) < 0) {
        std::fprintf(stderr, "self-test: the workspace refused a tab: %s\n", error.c_str());
        return 1;
    }

    std::printf("self-test: window assembled, %d docks, %d menus, %d tab(s)\n",
                static_cast<int>(window.findChildren<QDockWidget*>().size()),
                static_cast<int>(window.menuBar()->actions().size()),
                window.workspace()->sessionCount());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // --version is answered before any Qt object exists: a bug report has to be
    // collectable from a machine with no display and no session bus.
    if (hasFlag(argc, argv, "--version") || hasFlag(argc, argv, "-v")) {
        printVersion();
        return 0;
    }

    const bool self_test = hasFlag(argc, argv, "--self-test");
    if (self_test) {
        // The offscreen platform plugin renders into a buffer, so the window really is built
        // and laid out -- without needing a display server.
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("openSCRT"));
    QCoreApplication::setApplicationName(QStringLiteral("openSCRT"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(openscrt::core::kProjectVersion));

    if (self_test) {
        return runSelfTest(app);
    }

#if defined(_WIN32)
    hideConsoleIfDoubleClicked();
#endif

    openscrt::ui::MainWindow window;
    window.loadSessions();
    window.show();
    return app.exec();
}
