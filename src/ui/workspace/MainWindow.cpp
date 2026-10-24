#include "ui/workspace/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "config/ConfigManager.h"
#include "config/importers/SecureCrtConfigImporter.h"
#include "core/BuildInfo.h"
#include "credential/kdbx/KdbxCredentialResolver.h"
#include "protocols/ITransportChannel.h"
#include "ui/workspace/ButtonBar.h"
#include "ui/workspace/CommandWindow.h"
#include "ui/workspace/KeePassSelectorDialog.h"
#include "ui/workspace/QuickConnectDialog.h"
#include "ui/workspace/SessionManagerWidget.h"
#include "ui/workspace/TerminalSession.h"
#include "ui/workspace/WorkspacePanes.h"

namespace openscrt::ui {
namespace {

// A menu entry that belongs to a later phase. It is shown greyed out rather than hidden,
// because the roadmap's menu bar is part of the shape users expect, and an empty menu would
// look like something was forgotten.
QAction* addPlaceholder(QMenu* menu, const QString& text, const QString& phase) {
    auto* action = menu->addAction(text);
    action->setEnabled(false);
    action->setToolTip(QStringLiteral("Arrives with %1").arg(phase));
    return action;
}

QString defaultConfigPath() {
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath() + QStringLiteral("/.openscrt");
    }
    return directory + QStringLiteral("/sessions.ini");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("openSCRT"));
    resize(1100, 720);
    config_path_ = defaultConfigPath().toStdString();

    workspace_ = new WorkspacePanes(this);
    button_bar_ = new ButtonBar(this);
    command_window_ = new CommandWindow(this);

    // The button bar sits under the workspace and the command window under that, the way
    // SecureCRT puts them at the bottom.
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(workspace_, 1);
    layout->addWidget(button_bar_);
    layout->addWidget(command_window_);
    setCentralWidget(central);

    buildDocks();
    buildMenus();
    buildStatusBar();

    connect(workspace_, &WorkspacePanes::sessionOpened, this,
            [this](const QString&, const QString& title) {
                setStatus(tr("Opened %1").arg(title));
                refreshActiveSessions();
                rebuildCommandTargets();
            });
    connect(workspace_, &WorkspacePanes::sessionClosed, this, [this](const QString&) {
        refreshActiveSessions();
        rebuildCommandTargets();
    });
    connect(workspace_, &WorkspacePanes::sessionStateChanged, this,
            [this](const QString& id, int state, const QString& detail) {
                const auto transport_state = static_cast<protocols::TransportState>(state);
                QString text =
                    tr("%1: %2").arg(id, QString::fromUtf8(protocols::toString(transport_state)));
                if (!detail.isEmpty()) {
                    text += QStringLiteral(" - ") + detail;
                }
                setStatus(text);
            });
    connect(session_manager_, &SessionManagerWidget::sessionActivated, this,
            [this](const QString& id) { openSession(id.toStdString()); });
    connect(session_manager_, &SessionManagerWidget::sessionSelected, this,
            [this](const QString& id) {
                if (!id.isEmpty()) {
                    setStatus(tr("Selected session %1").arg(id));
                }
            });

    // A button sends to the session that is in front when it is pressed. The session is
    // looked up at that moment and not when the button was built, so a button follows
    // the user from one session to the next.
    //
    // The buffer is not called "data": QWidget has a protected member of that name, and
    // a local with the same name shadows it.
    button_bar_->setSender([this](const std::uint8_t* payload, std::size_t size) {
        TerminalSession* target = workspace_->sessionById(workspace_->activeSessionId());
        if (target != nullptr) {
            target->sendInput(std::vector<std::uint8_t>(payload, payload + size));
        }
    });
    connect(button_bar_, &ButtonBar::openSessionRequested, this,
            [this](const QString& id) { openSession(id.toStdString()); });
    connect(button_bar_, &ButtonBar::scriptRequested, this, [this](const QString& path) {
        setStatus(tr("Running scripts arrives with the scripting phase (%1)").arg(path));
    });
    connect(button_bar_, &ButtonBar::menuFunctionRequested, this, [this](const QString& path) {
        setStatus(tr("Menu functions arrive with the scripting phase (%1)").arg(path));
    });
    connect(button_bar_, &ButtonBar::modelChanged, this, [this]() { saveButtonBar(); });

    setStatus(tr("Ready"));
}

MainWindow::~MainWindow() = default;

void MainWindow::buildDocks() {
    auto* manager_dock = new QDockWidget(tr("Session Manager"), this);
    manager_dock->setObjectName(QStringLiteral("sessionManagerDock"));
    session_manager_ = new SessionManagerWidget(manager_dock);
    manager_dock->setWidget(session_manager_);
    addDockWidget(Qt::LeftDockWidgetArea, manager_dock);

    auto* active_dock = new QDockWidget(tr("Active Sessions"), this);
    active_dock->setObjectName(QStringLiteral("activeSessionsDock"));
    active_sessions_ = new QListWidget(active_dock);
    active_sessions_->setSelectionMode(QAbstractItemView::SingleSelection);
    active_dock->setWidget(active_sessions_);
    addDockWidget(Qt::LeftDockWidgetArea, active_dock);
}

void MainWindow::buildMenus() {
    // Qt 6.4 deprecated the old addAction(text, receiver, slot, shortcut) order in favour of
    // addAction(text, shortcut, receiver, slot). The old one still compiles, but it carries
    // a [[deprecated]] attribute, and this project builds with -Werror.
    QMenu* file_menu = menuBar()->addMenu(tr("File"));
    file_menu->addAction(tr("New Session (Quick Connect)..."),
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this,
                         &MainWindow::openQuickConnect);
    file_menu->addAction(tr("Connect Selected Session"), this, &MainWindow::connectSelectedSession);
    file_menu->addSeparator();
    file_menu->addAction(tr("Close Tab"), QKeySequence(Qt::CTRL | Qt::Key_W), this,
                         &MainWindow::closeCurrentTab);
    file_menu->addAction(tr("Save Sessions"), QKeySequence(Qt::CTRL | Qt::Key_S), this,
                         [this]() { saveSessions(); });
    file_menu->addSeparator();
    file_menu->addAction(tr("Import SecureCRT Sessions..."), this,
                         &MainWindow::importSecureCrtSessions);
    file_menu->addSeparator();
    file_menu->addAction(tr("Exit"), QKeySequence(Qt::CTRL | Qt::Key_Q), this, &QWidget::close);

    QMenu* edit_menu = menuBar()->addMenu(tr("Edit"));
    addPlaceholder(edit_menu, tr("Copy / Paste / Clear Screen"), tr("the clipboard phase"));

    QMenu* view_menu = menuBar()->addMenu(tr("View"));
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        view_menu->addAction(dock->toggleViewAction());
    }

    QMenu* options_menu = menuBar()->addMenu(tr("Options"));
    options_menu->addAction(tr("Apply Default Session to All Sessions..."), this,
                            [this]() { applyDefaultSessionToAll(); });
    addPlaceholder(options_menu, tr("Session Properties..."), tr("the session editor phase"));

    QMenu* transfer_menu = menuBar()->addMenu(tr("Transfer"));
    addPlaceholder(transfer_menu, tr("SFTP / Zmodem"), tr("the transfer phase"));

    QMenu* script_menu = menuBar()->addMenu(tr("Script"));
    addPlaceholder(script_menu, tr("Record / Run Script"), tr("the scripting phase"));

    QMenu* tools_menu = menuBar()->addMenu(tr("Tools"));
    tools_menu->addAction(tr("Open KeePass Database..."), this, &MainWindow::openCredentialStore);
    tools_menu->addAction(tr("Bind Session to KeePass Entry..."), this,
                          [this]() { chooseKeePassEntry(); });

    QMenu* window_menu = menuBar()->addMenu(tr("Window"));
    window_menu->addAction(tr("Next Tab"), QKeySequence(Qt::CTRL | Qt::Key_Tab), this,
                           [this]() { workspace_->nextTab(); });
    window_menu->addAction(tr("Previous Tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                           this, [this]() { workspace_->previousTab(); });
    window_menu->addSeparator();
    window_menu->addAction(tr("Split Right"), this,
                           [this]() { workspace_->split(Qt::Horizontal); });
    window_menu->addAction(tr("Split Down"), this, [this]() { workspace_->split(Qt::Vertical); });
    window_menu->addAction(tr("Tile Panes"), this, [this]() { workspace_->tile(); });
    window_menu->addAction(tr("Close Pane"), this, [this]() { workspace_->closeActivePane(); });
    window_menu->addSeparator();
    window_menu->addAction(tr("Close All Tabs"), this, [this]() { workspace_->closeAll(); });

    QMenu* help_menu = menuBar()->addMenu(tr("Help"));
    help_menu->addAction(tr("About openSCRT"), this, [this]() {
        QMessageBox::about(this, tr("About openSCRT"),
                           tr("openSCRT %1\n\nA SecureCRT-alike native cross-platform terminal.")
                               .arg(QString::fromUtf8(core::kProjectVersion)));
    });
}

void MainWindow::buildStatusBar() {
    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);
}

void MainWindow::setStatus(const QString& text) {
    if (status_label_ == nullptr) {
        return;
    }
    status_label_->setText(text);
}

void MainWindow::setConfigPath(const std::string& path) {
    config_path_ = path;
}

void MainWindow::loadSessions() {
    tree_ = config::ConfigManager::loadTree(config_path_);
    tree_loaded_ = true;
    session_manager_->setTree(tree_);
    // The buttons the user built are part of the same file, so they come back with the
    // sessions.
    button_bar_->setModel(config::ConfigManager::loadButtonBar(config_path_));

    QSettings settings(QString::fromStdString(config_path_), QSettings::IniFormat);
    const std::string geometry = config::ConfigManager::loadWindowGeometry(settings);
    if (!geometry.empty()) {
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
    }
    setStatus(tr("Loaded %1 saved sessions").arg(tree_.size()));
}

bool MainWindow::saveSessions() {
    if (!tree_loaded_) {
        // Nothing was ever read, so there is nothing worth writing -- and writing an empty
        // tree over a file the user never asked to touch would destroy their sessions.
        return false;
    }
    const bool ok = config::ConfigManager::saveTree(tree_, config_path_);
    if (ok) {
        QSettings settings(QString::fromStdString(config_path_), QSettings::IniFormat);
        config::ConfigManager::saveWindowGeometry(settings,
                                                  saveGeometry().toBase64().toStdString());
        settings.sync();
        setStatus(tr("Saved %1 sessions").arg(tree_.size()));
    } else {
        setStatus(tr("Could not write %1").arg(QString::fromStdString(config_path_)));
    }
    return ok;
}

bool MainWindow::saveButtonBar() {
    if (!tree_loaded_) {
        // The file was never read, so it is not ours to write.
        return false;
    }
    return config::ConfigManager::saveButtonBar(button_bar_->model(), config_path_);
}

void MainWindow::importSecureCrtSessions() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Open the SecureCRT Sessions Folder"), QString(), QFileDialog::ShowDirsOnly);
    if (directory.isEmpty()) {
        return;
    }

    const config::SecureCrtImportResult result =
        config::SecureCrtConfigImporter::importTree(directory.toStdString());
    if (!result.ok) {
        QMessageBox::warning(this, tr("SecureCRT"), QString::fromStdString(result.error));
        return;
    }

    // The parents come before their children, so the sessions can be added one after
    // another. A session whose id is already taken is left out and said so.
    int imported = 0;
    QStringList problems;
    for (const config::SessionProfile& profile : result.sessions) {
        if (tree_.add(profile)) {
            ++imported;
            continue;
        }
        problems << tr("%1: a session with that id is already there")
                        .arg(QString::fromStdString(profile.name));
    }
    for (const config::SecureCrtSkipped& skipped : result.skipped) {
        problems << tr("%1: %2").arg(QString::fromStdString(skipped.name),
                                     QString::fromStdString(skipped.reason));
    }
    for (const std::string& note : result.notes) {
        problems << QString::fromStdString(note);
    }

    session_manager_->setTree(tree_);
    saveSessions();
    setStatus(tr("Imported %1 sessions from SecureCRT").arg(imported));
    if (!problems.isEmpty()) {
        QMessageBox::information(this, tr("SecureCRT"), problems.join(QLatin1Char('\n')));
    }
}

void MainWindow::openCredentialStore() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open KeePass Database"), QString(),
                                     tr("KeePass databases (*.kdbx);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString master =
        QInputDialog::getText(this, tr("Master Password"), tr("Master password for %1").arg(path),
                              QLineEdit::Password, QString(), &accepted);
    if (!accepted) {
        return;
    }

    // The derivation of the key of a KDBX 4 file takes a second or more, and it is done
    // on purpose. It happens on a worker thread, so the window keeps answering while
    // the database is being unlocked.
    QProgressDialog progress(tr("Unlocking the database..."), QString(), 0, 0, this);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    bool opened = false;
    QString message;
    // The dialog is the context of the connection, so the connection goes away with it
    // and a second unlock does not pile up on the first.
    connect(&vault_, &KdbxVault::opened, &progress, [&](bool ok, const QString& text) {
        opened = ok;
        message = text;
        progress.close();
    });

    const QByteArray master_utf8 = master.toUtf8();
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(master_utf8.constData());
    vault_.openAsync(path, bytes, static_cast<std::size_t>(master_utf8.size()), {});

    // The dialog runs a nested event loop, so the window stays alive and the unlock
    // finishes in the background.
    progress.exec();

    if (!opened) {
        QMessageBox::warning(this, tr("KeePass"), message);
        return;
    }
    setStatus(tr("Opened the KeePass database with %1 entries")
                  .arg(static_cast<qulonglong>(vault_.index().entries.size())));
}

std::optional<core::SecureByteArray> MainWindow::credentialFor(
    const config::SessionProfile& profile, std::string& error) {
    using credential::kdbx::KdbxBinding;
    using credential::kdbx::KdbxBindingSource;
    using credential::kdbx::KdbxResolutionOutcome;

    // The buffer lives in its optional from the start. SecureByteArray cannot be copied,
    // and the implicit move that turns a plain return value into one only applies when
    // the expression has the same type as the return type -- here it does not, so the
    // optional is built once and moved as a whole.
    //
    // An empty buffer inside the optional means "the server asks"; an unset optional
    // means the session must not open at all.
    std::optional<core::SecureByteArray> password{core::SecureByteArray{}};
    if (!profile.credential_source) {
        // The session asks the server itself.
        return password;
    }
    KdbxBinding binding;
    if (!credential::kdbx::kdbxBindingSourceFromString(*profile.credential_source,
                                                       binding.source)) {
        // A binding this build cannot read is a configuration error, not a missing
        // password: connecting without it would fail later in a way that looks like the
        // server's fault.
        error = "the credential binding of this session is not one this build understands";
        return std::nullopt;
    }
    if (binding.source == KdbxBindingSource::kSessionSaved) {
        // The session's own password store is not written yet, so the server asks.
        return password;
    }
    if (binding.source == KdbxBindingSource::kPromptEveryTime) {
        bool accepted = false;
        const QString typed = QInputDialog::getText(
            this, tr("Password"), tr("Password for %1").arg(QString::fromStdString(profile.id)),
            QLineEdit::Password, QString(), &accepted);
        if (!accepted) {
            error = "the password prompt was cancelled";
            return std::nullopt;
        }
        const QByteArray utf8 = typed.toUtf8();
        password->assign(reinterpret_cast<const std::uint8_t*>(utf8.constData()),
                         static_cast<std::size_t>(utf8.size()));
        return password;
    }

    if (!vault_.isOpen()) {
        error = "the KeePass database is not open";
        return std::nullopt;
    }
    binding.entry_uuid = profile.credential_entry_uuid.value_or(std::string());
    binding.match_host = profile.credential_match_host.value_or(std::string());
    const std::string host = profile.host.value_or(std::string());
    const credential::kdbx::KdbxResolution resolution =
        credential::kdbx::resolveKdbxBinding(vault_.index(), binding, host);
    if (resolution.outcome != KdbxResolutionOutcome::kFound) {
        error = "the KeePass entry for this session was not found";
        return std::nullopt;
    }
    const credential::kdbx::KdbxEntry* entry = resolution.entry(vault_.index());
    if (entry == nullptr || !vault_.revealPassword(*entry, *password, error)) {
        return std::nullopt;
    }
    return password;
}

bool MainWindow::bindSessionToKeePassEntry(const std::string& session_id,
                                           const QString& entry_uuid) {
    const config::SessionProfile* existing = tree_.find(session_id);
    if (existing == nullptr) {
        setStatus(tr("No such session: %1").arg(QString::fromStdString(session_id)));
        return false;
    }
    if (entry_uuid.isEmpty()) {
        setStatus(tr("No KeePass entry was picked"));
        return false;
    }

    config::SessionProfile updated = *existing;
    updated.credential_source =
        std::string(credential::kdbx::toString(credential::kdbx::KdbxBindingSource::kKeePassUuid));
    updated.credential_entry_uuid = entry_uuid.toStdString();
    // The uuid is the whole binding now. A leftover match host would be a second,
    // contradicting answer to the same question.
    updated.credential_match_host.reset();

    std::string error;
    if (!tree_.update(std::move(updated), &error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    session_manager_->setTree(tree_);
    saveSessions();
    setStatus(tr("Session %1 now takes its password from the KeePass entry %2")
                  .arg(QString::fromStdString(session_id), entry_uuid));
    return true;
}

void MainWindow::chooseKeePassEntry() {
    if (!vault_.isOpen()) {
        QMessageBox::information(
            this, tr("KeePass"),
            tr("Open a KeePass database first (Tools > Open KeePass Database...)."));
        return;
    }
    const std::string selected = session_manager_->selectedSessionId();
    if (selected.empty()) {
        QMessageBox::information(this, tr("KeePass"),
                                 tr("Select a session in the Session Manager first."));
        return;
    }

    KeePassSelectorDialog dialog(this);
    dialog.setEntries(vault_.index());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    bindSessionToKeePassEntry(selected, dialog.selectedEntryUuid());
}

void MainWindow::openSession(const std::string& id) {
    const config::SessionProfile* profile = tree_.find(id);
    if (profile == nullptr) {
        setStatus(tr("No such session: %1").arg(QString::fromStdString(id)));
        return;
    }
    // The session that goes into the tab carries the *effective* settings: the transport
    // needs plain values, not a chain to walk.
    const config::SessionProfile effective = tree_.effectiveProfile(id);
    // The password comes from wherever the session's binding says it comes from. It is
    // handed to the transport and forgotten; it never becomes part of the profile.
    std::string credential_error;
    std::optional<core::SecureByteArray> password = credentialFor(effective, credential_error);
    if (!password.has_value()) {
        // The user cancelled, or the credential could not be produced. Connecting anyway
        // would fail with a password prompt nobody asked for.
        setStatus(QString::fromStdString(credential_error));
        return;
    }
    std::string error;
    const int index = workspace_->openSession(effective, error, std::move(*password));
    if (index < 0) {
        setStatus(QString::fromStdString(error));
    }
}

void MainWindow::applyDefaultSessionToAll() {
    // Every explicit override goes away, so every session follows the Default Session
    // template again. The template itself keeps its own values.
    for (const config::SessionProfile& profile : tree_.profiles()) {
        if (!profile.parent_id.empty()) {
            continue;
        }
        const std::string id = profile.id;
        tree_.inheritEverythingBelow(id);
    }
    session_manager_->setTree(tree_);
    setStatus(tr("All sessions now follow the Default Session template"));
}

void MainWindow::openStartupQuickConnect() {
    // The startup path and the File menu path show the same dialog; the only difference
    // is that at startup the window is already behind it.
    openQuickConnect();
}

void MainWindow::openQuickConnect() {
    QuickConnectDialog dialog(this);
    emit quickConnectDialogShown();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    config::SessionProfile profile = dialog.profile();
    profile.id = "quick-" + std::to_string(++quick_connect_counter_);
    if (profile.name.empty()) {
        profile.name = profile.id;
    }

    // The password goes to the transport beside the profile, never into it: the profile
    // is what gets written to the session file.
    core::SecureByteArray password = dialog.password();

    std::string error;
    const int index = workspace_->openSession(profile, error, std::move(password));
    if (index < 0) {
        setStatus(QString::fromStdString(error));
        return;
    }
    setStatus(tr("Connected to %1").arg(QString::fromStdString(profile.host.value_or(""))));
}

void MainWindow::connectSelectedSession() {
    const std::string id = session_manager_->selectedSessionId();
    if (id.empty()) {
        setStatus(tr("No session is selected"));
        return;
    }
    openSession(id);
}

void MainWindow::closeCurrentTab() {
    const QString id = workspace_->activeSessionId();
    if (id.isEmpty()) {
        return;
    }
    workspace_->closeSession(id);
}

void MainWindow::rebuildCommandTargets() {
    // The sinks look their session up by id when the command is sent, not when the
    // target is set: a session that is closed in between must not leave a dangling
    // pointer behind.
    std::vector<CommandWindow::Sink> sinks;
    for (const WorkspacePanes::SessionRef& ref : workspace_->sessions()) {
        if (ref.session == nullptr) {
            continue;
        }
        const QString id = ref.id;
        sinks.push_back([this, id](const std::vector<std::uint8_t>& bytes) {
            TerminalSession* target = workspace_->sessionById(id);
            if (target != nullptr) {
                target->sendInput(bytes);
            }
        });
    }
    command_window_->setBroadcastSinks(std::move(sinks));

    command_window_->setCurrentSink([this](const std::vector<std::uint8_t>& bytes) {
        TerminalSession* target = workspace_->sessionById(workspace_->activeSessionId());
        if (target != nullptr) {
            target->sendInput(bytes);
        }
    });
}

void MainWindow::refreshActiveSessions() {
    if (active_sessions_ == nullptr) {
        return;
    }
    active_sessions_->clear();
    // Every session of every pane, in the order they were opened.
    for (const QString& title : workspace_->sessionTitles()) {
        active_sessions_->addItem(title);
    }
}

QString MainWindow::statusText() const {
    return status_label_ == nullptr ? QString{} : status_label_->text();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSessions();
    saveButtonBar();
    QMainWindow::closeEvent(event);
}

}  // namespace openscrt::ui
