// The main window (P1-T12).
//
// Assembles the pieces Phase 1 built into the shell SecureCRT users recognise: the classic
// menu bar, the Session Manager dock on the left, the tabbed workspace in the middle, and
// the status bar that reports what the session under the cursor is doing.
//
// The window owns the session tree. Everything the user changes goes through here, and the
// tree is written back to disk when the window closes.

#ifndef OPENCRT_UI_WORKSPACE_MAIN_WINDOW_H_
#define OPENCRT_UI_WORKSPACE_MAIN_WINDOW_H_

#include <QMainWindow>
#include <string>

#include "config/SessionProfile.h"

class QCloseEvent;
class QLabel;
class QListWidget;

namespace openscrt::ui {

class CommandWindow;
class SessionManagerWidget;
class WorkspaceTabs;

class MainWindow : public QMainWindow {
    Q_OBJECT

 public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // The file the session tree is read from and written to.
    void setConfigPath(const std::string& path);
    [[nodiscard]] const std::string& configPath() const noexcept { return config_path_; }

    // Reads the saved sessions into the Session Manager.
    void loadSessions();

    // Writes the session tree back to disk. Returns false when the file could not be
    // written.
    bool saveSessions();

    // Opens a saved session in a new tab.
    void openSession(const std::string& id);

    // Clears every explicit override in the tree, so all sessions follow the Default
    // Session template again. This is the roadmap's "apply the template to every existing
    // session" action.
    void applyDefaultSessionToAll();

    [[nodiscard]] WorkspaceTabs* workspace() const noexcept { return workspace_; }
    [[nodiscard]] CommandWindow* commandWindow() const noexcept { return command_window_; }
    [[nodiscard]] SessionManagerWidget* sessionManager() const noexcept { return session_manager_; }
    [[nodiscard]] QString statusText() const;

    // Opens the connect dialog the way a launch without arguments does (P1-T13): the
    // window is already up, and the dialog is on top of it. The File menu path uses
    // openQuickConnect(), which is the same dialog.
    void openStartupQuickConnect();

 public slots:
    void openQuickConnect();
    void connectSelectedSession();
    void closeCurrentTab();

 signals:
    // Emitted once the Quick Connect dialog has been built, before it is shown. The
    // startup path and the File menu path both go through here, so a test can observe
    // that the dialog really appears without waiting for a human to close it.
    void quickConnectDialogShown();

 protected:
    void closeEvent(QCloseEvent* event) override;

 private:
    void buildMenus();
    void buildDocks();
    void buildStatusBar();
    void setStatus(const QString& text);
    void refreshActiveSessions();
    // Points the command window at the sessions that are open, and at the one in front.
    void rebuildCommandTargets();

    std::string config_path_;
    config::SessionTree tree_;
    bool tree_loaded_{false};

    WorkspaceTabs* workspace_{nullptr};
    CommandWindow* command_window_{nullptr};
    SessionManagerWidget* session_manager_{nullptr};
    QListWidget* active_sessions_{nullptr};
    QLabel* status_label_{nullptr};
    int quick_connect_counter_{0};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_MAIN_WINDOW_H_
