#include "ui/workspace/SftpDualPaneWidget.h"

#include <QtGui/qevent.h>

#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <filesystem>
#include <functional>

namespace openscrt::ui {
namespace {

namespace fs = std::filesystem;

// A tree that says what was dropped on it. The drop is not carried out here: the
// widget asks for a transfer with a signal, and the owner of the subsystem decides
// how to do it.
class DropTree : public QTreeWidget {
 public:
    using QTreeWidget::QTreeWidget;

    std::function<void(const QString&)> onFileDropped;

 protected:
    void dropEvent(QDropEvent* event) override {
        if (onFileDropped) {
            const QList<QTreeWidgetItem*> items = selectedItems();
            if (!items.isEmpty()) {
                onFileDropped(items.first()->text(0));
                event->acceptProposedAction();
                return;
            }
        }
        QTreeWidget::dropEvent(event);
    }
};

std::string fileName(const QString& text) {
    return text.toStdString();
}

}  // namespace

SftpDualPaneWidget::SftpDualPaneWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* local_label = new QLabel(this);
    auto* remote_label = new QLabel(this);
    auto* local_tree = new DropTree(this);
    auto* remote_tree = new DropTree(this);
    local_ = Pane(local_label, local_tree);
    remote_ = Pane(remote_label, remote_tree);

    for (QTreeWidget* tree : {local_tree, remote_tree}) {
        tree->setColumnCount(2);
        tree->setHeaderLabels({tr("Name"), tr("Size")});
        tree->setRootIsDecorated(false);
        tree->setDragEnabled(true);
        tree->setAcceptDrops(true);
        tree->setDragDropMode(QAbstractItemView::DragDrop);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    }

    layout->addWidget(local_label);
    layout->addWidget(local_tree);
    layout->addWidget(remote_label);
    layout->addWidget(remote_tree);

    // A file dragged onto the far side goes up; a file dragged onto this side comes
    // down.
    remote_tree->onFileDropped = [this](const QString& name) {
        const std::string local = (fs::path(local_directory_) / fileName(name)).string();
        emit transferRequested(QString::fromStdString(local),
                               QString::fromStdString(remote_directory_ + "/" + fileName(name)),
                               true);
    };
    local_tree->onFileDropped = [this](const QString& name) {
        const std::string remote = (remote_directory_.empty() || remote_directory_.back() == '/')
                                       ? remote_directory_ + fileName(name)
                                       : remote_directory_ + "/" + fileName(name);
        emit transferRequested(QString::fromStdString(local_directory_ + "/" + fileName(name)),
                               QString::fromStdString(remote), false);
    };

    connect(local_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { onLocalItemActivated(item); });
    connect(remote_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { onRemoteItemActivated(item); });
}

SftpDualPaneWidget::~SftpDualPaneWidget() = default;

void SftpDualPaneWidget::setSubsystem(transfer::sftp::SftpSubsystem* subsystem) {
    subsystem_ = subsystem;
    if (subsystem_ != nullptr) {
        std::string error;
        const std::string home = subsystem_->homeDirectory(error);
        if (!home.empty()) {
            remote_directory_ = home;
        }
    }
    refresh();
}

void SftpDualPaneWidget::setLocalDirectory(const std::string& path) {
    local_directory_ = path;
    refresh();
}

void SftpDualPaneWidget::setRemoteDirectory(const std::string& path) {
    remote_directory_ = path;
    refresh();
}

std::string SftpDualPaneWidget::localDirectory() const {
    return local_directory_;
}

std::string SftpDualPaneWidget::remoteDirectory() const {
    return remote_directory_;
}

void SftpDualPaneWidget::refresh() {
    fillLocal();
    fillRemote();
}

void SftpDualPaneWidget::fillLocal() {
    local_.label->setText(QString::fromStdString(local_directory_));
    local_.tree->clear();

    std::error_code error;
    const fs::path directory(local_directory_);
    if (!fs::is_directory(directory, error)) {
        local_.tree->addTopLevelItem(new QTreeWidgetItem(
            local_.tree, {QStringLiteral("there is no such directory"), QString()}));
        return;
    }
    std::vector<std::pair<std::string, std::uint64_t>> entries;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        std::error_code size_error;
        const auto size = entry.is_regular_file(size_error) ? entry.file_size(size_error) : 0;
        entries.emplace_back(entry.path().filename().string(), size);
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& entry : entries) {
        local_.tree->addTopLevelItem(new QTreeWidgetItem(
            local_.tree, {QString::fromStdString(entry.first), QString::number(entry.second)}));
    }
}

void SftpDualPaneWidget::fillRemote() {
    remote_.label->setText(QString::fromStdString(remote_directory_));
    remote_.tree->clear();
    if (subsystem_ == nullptr) {
        remote_.tree->addTopLevelItem(new QTreeWidgetItem(
            remote_.tree, {QStringLiteral("there is no connection"), QString()}));
        return;
    }
    std::string error;
    const std::vector<transfer::sftp::SftpFileInfo> entries =
        subsystem_->listDirectory(remote_directory_, error);
    if (!error.empty()) {
        remote_.tree->addTopLevelItem(
            new QTreeWidgetItem(remote_.tree, {QString::fromStdString(error), QString()}));
        return;
    }
    for (const transfer::sftp::SftpFileInfo& entry : entries) {
        remote_.tree->addTopLevelItem(new QTreeWidgetItem(
            remote_.tree, {QString::fromStdString(entry.name), QString::number(entry.size)}));
    }
}

void SftpDualPaneWidget::onLocalItemActivated(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const fs::path candidate = fs::path(local_directory_) / fileName(item->text(0));
    std::error_code error;
    if (!fs::is_directory(candidate, error)) {
        return;
    }
    setLocalDirectory(fs::canonical(candidate, error).string());
}

void SftpDualPaneWidget::onRemoteItemActivated(QTreeWidgetItem* item) {
    if (item == nullptr || subsystem_ == nullptr) {
        return;
    }
    std::string error;
    const std::vector<transfer::sftp::SftpFileInfo> entries =
        subsystem_->listDirectory(remote_directory_, error);
    for (const transfer::sftp::SftpFileInfo& entry : entries) {
        if (entry.name == item->text(0).toStdString() && entry.is_directory) {
            setRemoteDirectory(remote_directory_ + "/" + entry.name);
            return;
        }
    }
}

}  // namespace openscrt::ui
