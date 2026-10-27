// The two-pane file manager of the SFTP tab (P3-T5).
//
// One pane is this machine, the other is the far side, and a file that is dragged
// from one to the other is copied. The panes are the same widget: the only thing
// that differs is which side they talk to.
//
// The widget does not do the transfer itself: it asks for one with a signal, and
// whoever owns the subsystem decides how to carry it out. A file manager that moved
// the files itself would have to know about threads.

#ifndef OPENCRT_UI_WORKSPACE_SFTP_DUAL_PANE_WIDGET_H_
#define OPENCRT_UI_WORKSPACE_SFTP_DUAL_PANE_WIDGET_H_

#include <QWidget>
#include <memory>
#include <string>

#include "transfer/sftp/SftpSubsystem.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace openscrt::ui {

class SftpDualPaneWidget : public QWidget {
    Q_OBJECT

 public:
    explicit SftpDualPaneWidget(QWidget* parent = nullptr);
    ~SftpDualPaneWidget() override;

    SftpDualPaneWidget(const SftpDualPaneWidget&) = delete;
    SftpDualPaneWidget& operator=(const SftpDualPaneWidget&) = delete;

    // The far side. Without one, the remote pane stays empty and says so.
    void setSubsystem(transfer::sftp::SftpSubsystem* subsystem);

    void setLocalDirectory(const std::string& path);
    void setRemoteDirectory(const std::string& path);

    [[nodiscard]] std::string localDirectory() const;
    [[nodiscard]] std::string remoteDirectory() const;

    // Reads both directories again.
    void refresh();

 signals:
    // A file was dragged from one pane to the other. `upload` says which way.
    void transferRequested(const QString& localPath, const QString& remotePath, bool upload);
    // Something worth saying in the status bar.
    void statusMessage(const QString& text);

 private:
    // One side of the window.
    class Pane {
     public:
        Pane(QLabel* label_widget, QTreeWidget* tree_widget)
            : label(label_widget), tree(tree_widget) {}
        QLabel* label{nullptr};
        QTreeWidget* tree{nullptr};
    };

    void fillLocal();
    void fillRemote();
    void onLocalItemActivated(QTreeWidgetItem* item);
    void onRemoteItemActivated(QTreeWidgetItem* item);

    transfer::sftp::SftpSubsystem* subsystem_{nullptr};
    std::string local_directory_;
    std::string remote_directory_;

    Pane local_{nullptr, nullptr};
    Pane remote_{nullptr, nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_SFTP_DUAL_PANE_WIDGET_H_
