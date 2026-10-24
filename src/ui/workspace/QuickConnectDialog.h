// Quick Connect dialog (P1-T12).
//
// The one dialog SecureCRT users open most often: type a host, get a terminal. It produces
// an unsaved profile -- whether the session is written to the tree is the caller's
// decision, not the dialog's.

#ifndef OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_
#define OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_

#include <QDialog>

#include "config/SessionProfile.h"
#include "core/SecureByteArray.h"

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
    //
    // The profile carries no password, and never will: a profile is written to the INI,
    // and a password in the INI is a password on disk.
    [[nodiscard]] config::SessionProfile profile() const;

    // The password the user typed, in a locked buffer. It is handed to the transport
    // instead of to the profile, and the buffer wipes itself when it goes out of scope.
    [[nodiscard]] core::SecureByteArray password() const;

    // Why the dialog refuses to accept, or an empty string when the input is usable.
    [[nodiscard]] QString validationError() const;

    void setHost(const QString& host);
    void setPort(int port);
    void setUser(const QString& user);
    void setProtocol(config::Protocol protocol);
    void setPrivateKeyPath(const QString& path);
    void setPassword(const QString& password);

 public slots:
    // Overridden so the dialog refuses to close on an unusable host instead of handing a
    // broken session to the workspace.
    void accept() override;

    // Overridden to take the password out of the line edit as soon as the dialog is done,
    // whichever way it was closed. Qt does not clear strings for us, so this is the best
    // that can be done from here.
    void done(int result) override;

 private:
    [[nodiscard]] config::Protocol selectedProtocol() const;

    QLineEdit* host_edit_{nullptr};
    QSpinBox* port_spin_{nullptr};
    QLineEdit* user_edit_{nullptr};
    QComboBox* protocol_combo_{nullptr};
    QLineEdit* key_edit_{nullptr};
    QLineEdit* password_edit_{nullptr};
    QLabel* error_label_{nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_QUICK_CONNECT_DIALOG_H_
