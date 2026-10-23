// Quick Connect dialog (P1-T12).
//
// The one dialog SecureCRT users open most often: type a host, get a terminal. It produces
// an unsaved profile -- whether the session is written to the tree is the caller's
// decision, not the dialog's.

#ifndef OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_
#define OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_

#include <QDialog>

#include "config/SessionProfile.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace openscrt::ui {

class QuickConnectDialog : public QDialog {
    Q_OBJECT

 public:
    explicit QuickConnectDialog(QWidget* parent = nullptr);

    // The profile the dialog describes. Its id is empty on purpose: a quick-connect session
    // has not been saved under a name yet.
    [[nodiscard]] config::SessionProfile profile() const;

    // Why the dialog refuses to accept, or an empty string when the input is usable.
    [[nodiscard]] QString validationError() const;

    void setHost(const QString& host);
    void setPort(int port);
    void setUser(const QString& user);
    void setProtocol(config::Protocol protocol);
    void setPrivateKeyPath(const QString& path);

 public slots:
    // Overridden so the dialog refuses to close on an unusable host instead of handing a
    // broken session to the workspace.
    void accept() override;

 private:
    [[nodiscard]] config::Protocol selectedProtocol() const;

    QLineEdit* host_edit_{nullptr};
    QSpinBox* port_spin_{nullptr};
    QLineEdit* user_edit_{nullptr};
    QComboBox* protocol_combo_{nullptr};
    QLineEdit* key_edit_{nullptr};
    QLabel* error_label_{nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_
