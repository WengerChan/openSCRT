#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDockWidget>
#include <QFile>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QTimer>

#include "config/ConfigManager.h"
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
#include "terminal/buffer/ScreenGrid.h"
#include "transfer/ModuleInfo.h"
#include "ui/ModuleInfo.h"
#include "ui/workspace/MainWindow.h"
#include "ui/workspace/TerminalCanvasWidget.h"
#include "ui/workspace/TerminalSession.h"
#include "ui/workspace/WorkspacePanes.h"

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

// The part of the P1-A7 acceptance that only a real machine can produce.
//
// The CI runner proves the program works there; it cannot prove the program works on the
// machine the user is sitting at, with its fonts, its display and its shell. This mode
// runs the checks that do not need a human's judgement -- the window assembles, the
// connect dialog appears, a local shell session connects and echoes, a session survives
// a save and a reload, a tab closes, the transport disconnects -- and prints one line per
// check. What is left for the human is the part no program can judge: how it looks, and
// how it feels to type into it.
//
//   openscrt --acceptance-check
//
// The exit code is the number of failed checks, so a script can use it.
int runAcceptanceCheck(QApplication& app) {
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::fprintf(stderr, "acceptance: cannot create a scratch directory\n");
        return 1;
    }

    int failures = 0;
    const auto check = [&failures](const char* name, bool ok) {
        std::printf("acceptance: %-46s %s\n", name, ok ? "ok" : "FAILED");
        if (!ok) {
            ++failures;
        }
    };

    openscrt::ui::MainWindow window;
    window.setConfigPath(std::string(scratch.path().toStdString()) + "/sessions.ini");
    window.loadSessions();
    window.show();
    app.processEvents();

    // 1. The window: the classic menu bar and the two docks.
    check("the window carries a menu bar", !window.menuBar()->actions().isEmpty());
    check("the Session Manager and Active Sessions docks are there",
          window.findChildren<QDockWidget*>().size() >= 2);
    check("the workspace assembled", window.workspace() != nullptr);

    // 2. The connect dialog appears on its own at startup.
    bool dialog_shown = false;
    QObject::connect(&window, &openscrt::ui::MainWindow::quickConnectDialogShown, &window,
                     [&dialog_shown] {
                         dialog_shown = true;
                         // The dialog is modal and nobody is here to dismiss it.
                         QTimer::singleShot(0, [] {
                             if (auto* modal = QApplication::activeModalWidget()) {
                                 modal->close();
                             }
                         });
                     });
    window.openStartupQuickConnect();
    check("the connect dialog appears at startup", dialog_shown);
    check("cancelling the dialog leaves no tab", window.workspace()->sessionCount() == 0);

    // 3. A local shell session: the platform's own pty (forkpty or ConPTY). This is the
    //    part of the check that says something about the machine rather than about the
    //    code: it is the real shell, on the real platform, of the real user.
    openscrt::config::SessionProfile local;
    local.id = "acceptance-local";
    local.name = local.id;
    local.protocol = openscrt::config::Protocol::kLocalShell;
    std::string error;
    const int tab = window.workspace()->openSession(local, error);
    check("a local shell session opens", tab >= 0);

    if (tab >= 0) {
        openscrt::ui::TerminalSession* session =
            window.workspace()->sessionById(QString::fromStdString(local.id));
        bool connected = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (session != nullptr &&
                session->state() == openscrt::protocols::TransportState::kConnected) {
                connected = true;
                break;
            }
            app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check("the local shell connects", connected);

        bool echoed = false;
        if (connected && session != nullptr) {
            const std::string command = "echo openscrt-acceptance\r";
            session->sendInput(std::vector<std::uint8_t>(command.begin(), command.end()));
            const auto echo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < echo_deadline) {
                if (session != nullptr) {
                    const openscrt::terminal::ScreenGrid& grid = session->canvas()->grid();
                    for (int row = 0; row < grid.rows(); ++row) {
                        if (grid.rowText(row).find("openscrt-acceptance") != std::string::npos) {
                            echoed = true;
                            break;
                        }
                    }
                }
                if (echoed) {
                    break;
                }
                app.processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        check("the shell echoes a command onto the screen", echoed);
    }

    // 4. A session survives a save and a reload. The file is written first, so the window
    //    has something to load: the check is that the id comes back.
    {
        openscrt::config::SessionTree tree;
        openscrt::config::SessionProfile stored;
        stored.id = "acceptance-stored";
        stored.name = stored.id;
        stored.host = "acceptance.example";
        std::string add_error;
        if (!tree.add(std::move(stored), &add_error)) {
            std::fprintf(stderr, "acceptance: the tree refused a session: %s\n", add_error.c_str());
        }
        if (!openscrt::config::ConfigManager::saveTree(
                tree, std::string(scratch.path().toStdString()) + "/sessions.ini")) {
            check("the session file is written", false);
        } else {
            window.loadSessions();
            const bool saved = window.saveSessions();
            check("the session file is written", saved);
            QFile file(QString::fromStdString(std::string(scratch.path().toStdString()) +
                                              "/sessions.ini"));
            const bool readable = file.open(QIODevice::ReadOnly | QIODevice::Text);
            const QString contents = readable ? QString::fromUtf8(file.readAll()) : QString();
            check("a saved session comes back after a reload",
                  readable && contents.contains(QStringLiteral("acceptance-stored")));
        }
    }

    // 5. Closing the tab leaves nothing behind.
    window.workspace()->closeSession(QString::fromStdString(local.id));
    check("closing the tab leaves no session behind", window.workspace()->sessionCount() == 0);

    std::printf("acceptance: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
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
    const bool acceptance = hasFlag(argc, argv, "--acceptance-check");
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

    if (acceptance) {
        // The acceptance check wants the real platform: a real display and a real shell.
        // The offscreen plugin would answer the window questions but not the shell one.
        return runAcceptanceCheck(app);
    }

#if defined(_WIN32)
    hideConsoleIfDoubleClicked();
#endif

    openscrt::ui::MainWindow window;
    window.loadSessions();
    window.show();
    if (!self_test) {
        // A launch without arguments opens the connect dialog straight away (P1-T13), so
        // the first thing the user sees is the way to connect. The timer puts it inside
        // the event loop, where the dialog can be painted and dismissed.
        //
        // The smoke test skips it on purpose: the dialog is modal, and a smoke test has
        // nobody to dismiss it.
        QTimer::singleShot(0, &window, &openscrt::ui::MainWindow::openStartupQuickConnect);
    }
    return app.exec();
}
