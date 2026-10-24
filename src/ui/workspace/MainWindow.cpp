#include "ui/workspace/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <string>
#include <utility>

#include "config/ConfigManager.h"
#include "core/BuildInfo.h"
#include "protocols/ITransportChannel.h"
#include "ui/workspace/CommandWindow.h"
#include "ui/workspace/QuickConnectDialog.h"
#include "ui/workspace/SessionManagerWidget.h"
#include "ui/workspace/TerminalSession.h"
#include "ui/workspace/WorkspaceTabs.h"

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

    workspace_ = new WorkspaceTabs(this);
    command_window_ = new CommandWindow(this);

    // The command window sits under the tabs, the way SecureCRT puts it at the bottom.
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(workspace_, 1);
    layout->addWidget(command_window_);
    setCentralWidget(central);

    buildDocks();
    buildMenus();
    buildStatusBar();

    connect(workspace_, &WorkspaceTabs::sessionOpened, this,
            [this](const QString&, const QString& title) {
                setStatus(tr("Opened %1").arg(title));
                refreshActiveSessions();
                rebuildCommandTargets();
            });
    connect(workspace_, &WorkspaceTabs::sessionClosed, this, [this](const QString&) {
        refreshActiveSessions();
        rebuildCommandTargets();
    });
    connect(workspace_, &WorkspaceTabs::sessionStateChanged, this,
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
    addPlaceholder(tools_menu, tr("Button Bar / Command Window"), tr("the workspace phase"));

    QMenu* window_menu = menuBar()->addMenu(tr("Window"));
    window_menu->addAction(tr("Next Tab"), QKeySequence(Qt::CTRL | Qt::Key_Tab), this, [this]() {
        const int index = workspace_->currentIndex() + 1;
        if (index < workspace_->count()) {
            workspace_->setCurrentIndex(index);
        }
    });
    window_menu->addAction(tr("Previous Tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                           this, [this]() {
                               const int index = workspace_->currentIndex() - 1;
                               if (index >= 0) {
                                   workspace_->setCurrentIndex(index);
                               }
                           });
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

void MainWindow::openSession(const std::string& id) {
    const config::SessionProfile* profile = tree_.find(id);
    if (profile == nullptr) {
        setStatus(tr("No such session: %1").arg(QString::fromStdString(id)));
        return;
    }
    // The session that goes into the tab carries the *effective* settings: the transport
    // needs plain values, not a chain to walk.
    const config::SessionProfile effective = tree_.effectiveProfile(id);
    std::string error;
    const int index = workspace_->openSession(effective, error);
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
    for (const auto& session : workspace_->sessions()) {
        if (!session) {
            continue;
        }
        const QString id = session->id();
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
    for (int index = 0; index < workspace_->count(); ++index) {
        active_sessions_->addItem(workspace_->tabText(index));
    }
}

QString MainWindow::statusText() const {
    return status_label_ == nullptr ? QString{} : status_label_->text();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSessions();
    QMainWindow::closeEvent(event);
}

}  // namespace openscrt::ui
