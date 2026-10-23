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

    error_label_ = new QLabel(this);
    error_label_->setStyleSheet(QStringLiteral("color: #b00020;"));
    error_label_->setWordWrap(true);
    error_label_->hide();

    auto* form = new QFormLayout;
    form->addRow(tr("Host:"), host_edit_);
    form->addRow(tr("Port:"), port_spin_);
    form->addRow(tr("Username:"), user_edit_);
    form->addRow(tr("Protocol:"), protocol_combo_);
    form->addRow(tr("Private key:"), key_edit_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QuickConnectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QuickConnectDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(error_label_);
    layout->addWidget(buttons);

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
