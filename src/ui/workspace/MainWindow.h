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
#include <QString>
#include <memory>
#include <optional>
#include <string>

#include "config/SessionProfile.h"
#include "core/SecureByteArray.h"
#include "credential/SecretStore.h"
#include "credential/kdbx/KdbxVault.h"
#include "credential/keepasshttp/KeePassHttpProvider.h"

class QCloseEvent;
class QLabel;
class QListWidget;

namespace openscrt::ui {

class ButtonBar;
class CommandWindow;
class SessionManagerWidget;
class WorkspacePanes;

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

    // Writes the button bar back to disk. Returns false when the file could not be
    // written.
    bool saveButtonBar();

    // Opens a saved session in a new tab.
    void openSession(const std::string& id);

    // Clears every explicit override in the tree, so all sessions follow the Default
    // Session template again. This is the roadmap's "apply the template to every existing
    // session" action.
    void applyDefaultSessionToAll();

    // Binds a session to a KeePass entry, by uuid, and writes the tree back.
    //
    // The picker is the dialog; this is the part that writes the binding down, so it can
    // be driven without a human in front of the screen.
    bool bindSessionToKeePassEntry(const std::string& session_id, const QString& entry_uuid);

    // Imports the sessions of a SecureCRT Sessions folder into the session tree.
    void importSecureCrtSessions();

    // Pairs this program with a running KeePass over the KeePassHTTP protocol. The
    // plugin asks the user to confirm, in KeePass itself.
    void pairWithKeePass();

    [[nodiscard]] WorkspacePanes* workspace() const noexcept { return workspace_; }
    [[nodiscard]] ButtonBar* buttonBar() const noexcept { return button_bar_; }
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
    // Opens the KeePass database the sessions take their credentials from.
    void openCredentialStore();
    // Picks a KeePass entry for the selected session and binds it.
    void chooseKeePassEntry();
    // The password the session needs, taken from wherever its binding says. An unset
    // result means the user cancelled and the session must not open; an empty one means
    // "the server will ask", which is a legitimate way to connect.
    std::optional<core::SecureByteArray> credentialFor(const config::SessionProfile& profile,
                                                       std::string& error);

    std::string config_path_;
    config::SessionTree tree_;
    bool tree_loaded_{false};

    WorkspacePanes* workspace_{nullptr};
    ButtonBar* button_bar_{nullptr};
    CommandWindow* command_window_{nullptr};
    // The KeePass database the sessions take their credentials from. It holds the
    // master key while it is open and nothing once it is locked.
    credential::kdbx::KdbxVault vault_;
    // The other half of the dual mode: a running KeePass, asked over the KeePassHTTP
    // protocol. It is null until the program is paired with one.
    std::unique_ptr<credential::keepasshttp::KeePassHttpProvider> http_provider_;
    // Where the pairing is kept. It holds a key, so it goes into the store of the
    // system and not into a file of ours.
    std::unique_ptr<credential::ISecretStore> secret_store_;
    SessionManagerWidget* session_manager_{nullptr};
    QListWidget* active_sessions_{nullptr};
    QLabel* status_label_{nullptr};
    int quick_connect_counter_{0};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_MAIN_WINDOW_H_
