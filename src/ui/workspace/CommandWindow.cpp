#include "ui/workspace/CommandWindow.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLineEdit>

namespace openscrt::ui {

CommandWindow::CommandWindow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("commandWindow"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("commandWindowInput"));
    input_->setPlaceholderText(tr("Command"));
    layout->addWidget(input_, 1);

    broadcast_ = new QCheckBox(tr("Broadcast"), this);
    broadcast_->setObjectName(QStringLiteral("commandWindowBroadcast"));
    layout->addWidget(broadcast_);

    connect(input_, &QLineEdit::returnPressed, this, [this]() { send(); });
}

void CommandWindow::setBroadcastSinks(std::vector<Sink> sinks) {
    broadcast_sinks_ = std::move(sinks);
}

void CommandWindow::setCurrentSink(Sink sink) {
    current_sink_ = std::move(sink);
}

bool CommandWindow::isBroadcast() const noexcept {
    return broadcast_->isChecked();
}

void CommandWindow::setBroadcast(bool on) {
    broadcast_->setChecked(on);
}

QString CommandWindow::text() const {
    return input_->text();
}

void CommandWindow::setText(const QString& text) {
    input_->setText(text);
}

std::vector<std::uint8_t> CommandWindow::payload() const {
    const QByteArray line = input_->text().toUtf8();
    std::vector<std::uint8_t> bytes(line.begin(), line.end());
    // A shell waits for the newline.
    bytes.push_back('\n');
    return bytes;
}

std::size_t CommandWindow::send() {
    const QString line = input_->text();
    if (line.isEmpty()) {
        return 0;
    }
    const std::vector<std::uint8_t> bytes = payload();

    std::size_t recipients = 0;
    if (isBroadcast()) {
        for (const Sink& sink : broadcast_sinks_) {
            if (!sink) {
                continue;
            }
            sink(bytes);
            ++recipients;
        }
    } else if (current_sink_) {
        current_sink_(bytes);
        recipients = 1;
    }

    input_->clear();
    emit commandSent(line, isBroadcast(), recipients);
    return recipients;
}

}  // namespace openscrt::ui
