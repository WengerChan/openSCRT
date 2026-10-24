#include "ui/workspace/QuickConnectDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace openscrt::ui {
namespace {

void addProtocol(QComboBox* combo, config::Protocol protocol, const QString& label) {
    combo->addItem(label, static_cast<int>(protocol));
}

}  // namespace

QuickConnectDialog::QuickConnectDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Quick Connect"));

    host_edit_ = new QLineEdit(this);
    host_edit_->setPlaceholderText(tr("host name or IP address"));

    port_spin_ = new QSpinBox(this);
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(22);

    user_edit_ = new QLineEdit(this);
    user_edit_->setPlaceholderText(tr("user name"));

    protocol_combo_ = new QComboBox(this);
    addProtocol(protocol_combo_, config::Protocol::kSsh2, QStringLiteral("SSH2"));
    addProtocol(protocol_combo_, config::Protocol::kTelnet, QStringLiteral("Telnet"));
    addProtocol(protocol_combo_, config::Protocol::kSerial, QStringLiteral("Serial"));
    addProtocol(protocol_combo_, config::Protocol::kLocalShell, QStringLiteral("Local Shell"));

    key_edit_ = new QLineEdit(this);
    key_edit_->setPlaceholderText(tr("private key file (optional)"));

    // The password is used for this connection only. It is not part of the profile, so it
    // never reaches the session file.
    password_edit_ = new QLineEdit(this);
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setPlaceholderText(tr("password (optional, not saved)"));

    error_label_ = new QLabel(this);
    error_label_->setStyleSheet(QStringLiteral("color: #b00020;"));
    error_label_->setWordWrap(true);
    error_label_->hide();

    // The hint is not an error: connecting without a password is legitimate when the
    // server asks for it itself.
    hint_label_ = new QLabel(this);
    hint_label_->setStyleSheet(QStringLiteral("color: #666666;"));
    hint_label_->setWordWrap(true);
    hint_label_->hide();

    auto* form = new QFormLayout;
    form->addRow(tr("Host:"), host_edit_);
    form->addRow(tr("Port:"), port_spin_);
    form->addRow(tr("Username:"), user_edit_);
    form->addRow(tr("Protocol:"), protocol_combo_);
    form->addRow(tr("Private key:"), key_edit_);
    form->addRow(tr("Password:"), password_edit_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QuickConnectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QuickConnectDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint_label_);
    layout->addWidget(error_label_);
    layout->addWidget(buttons);

    // The hint follows the fields: it is only true while neither a password nor a key
    // has been given.
    const auto refresh_hint = [this] {
        const QString hint = authenticationHint();
        hint_label_->setText(hint);
        hint_label_->setVisible(!hint.isEmpty());
    };
    connect(key_edit_, &QLineEdit::textChanged, this, refresh_hint);
    connect(password_edit_, &QLineEdit::textChanged, this, refresh_hint);
    refresh_hint();

    resize(420, sizeHint().height());
}

config::Protocol QuickConnectDialog::selectedProtocol() const {
    const auto value = protocol_combo_->currentData().toInt();
    return static_cast<config::Protocol>(value);
}

QString QuickConnectDialog::validationError() const {
    if (host_edit_->text().trimmed().isEmpty()) {
        return tr("A host name is required.");
    }
    return {};
}

QString QuickConnectDialog::authenticationHint() const {
    if (selectedProtocol() != config::Protocol::kSsh2) {
        return {};
    }
    if (!key_edit_->text().trimmed().isEmpty() || !password_edit_->text().isEmpty()) {
        return {};
    }
    return tr("No password and no private key: the server will ask for the password itself.");
}

config::SessionProfile QuickConnectDialog::profile() const {
    config::SessionProfile value;
    const std::string host = host_edit_->text().trimmed().toStdString();
    value.name = host;
    value.host = host;
    value.port = port_spin_->value();
    value.user = user_edit_->text().trimmed().toStdString();
    value.protocol = selectedProtocol();
    const std::string key = key_edit_->text().trimmed().toStdString();
    if (!key.empty()) {
        value.private_key_path = key;
    }
    return value;
}

void QuickConnectDialog::setHost(const QString& host) {
    host_edit_->setText(host);
}

void QuickConnectDialog::setPort(int port) {
    port_spin_->setValue(port);
}

void QuickConnectDialog::setUser(const QString& user) {
    user_edit_->setText(user);
}

void QuickConnectDialog::setProtocol(config::Protocol protocol) {
    const int index = protocol_combo_->findData(static_cast<int>(protocol));
    if (index >= 0) {
        protocol_combo_->setCurrentIndex(index);
    }
}

void QuickConnectDialog::setPrivateKeyPath(const QString& path) {
    key_edit_->setText(path);
}

void QuickConnectDialog::setPassword(const QString& password) {
    password_edit_->setText(password);
}

core::SecureByteArray QuickConnectDialog::password() const {
    core::SecureByteArray secret;
    const QByteArray raw = password_edit_->text().toUtf8();
    if (!raw.isEmpty()) {
        secret.assign(raw.constData(), static_cast<std::size_t>(raw.size()));
    }
    return secret;
}

void QuickConnectDialog::done(int result) {
    // The password was in the line edit as text. Qt does not clear strings for us, so the
    // best that can be done from here is to take it out of the widget as soon as the
    // dialog is done with it.
    password_edit_->clear();
    QDialog::done(result);
}

void QuickConnectDialog::accept() {
    const QString error = validationError();
    if (!error.isEmpty()) {
        error_label_->setText(error);
        error_label_->show();
        return;
    }
    error_label_->hide();
    QDialog::accept();
}

}  // namespace openscrt::ui
