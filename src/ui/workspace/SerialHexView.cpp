#include "ui/workspace/SerialHexView.h"

#include <QFont>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QString>
#include <algorithm>
#include <utility>

namespace openscrt::ui {
namespace {

// Bytes per line. Sixteen is the number every hex viewer settles on because it is
// what a terminal line can hold with the offset in front of it.
constexpr std::size_t kBytesPerLine = 16;

bool isPrintable(std::uint8_t byte) {
    return byte >= 0x20 && byte < 0x7F;
}

}  // namespace

SerialHexView::SerialHexView(std::size_t capacity, QWidget* parent)
    : QWidget(parent), capacity_(capacity == 0 ? 1 : capacity) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    hex_pane_ = new QPlainTextEdit(this);
    ascii_pane_ = new QPlainTextEdit(this);
    for (QPlainTextEdit* pane : {hex_pane_, ascii_pane_}) {
        pane->setReadOnly(true);
        pane->setLineWrapMode(QPlainTextEdit::NoWrap);
        // A monospace font is not decoration here: the two panes are meant to be read
        // against each other, so a column has to be a column.
        pane->setFont(QFont(QStringLiteral("monospace")));
    }
    layout->addWidget(hex_pane_);
    layout->addWidget(ascii_pane_);
}

SerialHexView::~SerialHexView() = default;

void SerialHexView::append(const std::uint8_t* payload, std::size_t size) {
    if (payload == nullptr || size == 0) {
        return;
    }
    for (std::size_t index = 0; index < size; ++index) {
        bytes_.push_back(payload[index]);
    }
    // The oldest bytes fall out of the window. The deque is trimmed from the front,
    // which is where it is cheap.
    while (bytes_.size() > capacity_) {
        bytes_.pop_front();
    }
    render();
}

void SerialHexView::append(std::string_view text) {
    append(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void SerialHexView::clear() {
    bytes_.clear();
    render();
}

void SerialHexView::render() {
    QString hex;
    QString ascii;
    const auto count = static_cast<std::size_t>(bytes_.size());

    for (std::size_t offset = 0; offset < count; offset += kBytesPerLine) {
        // The offset in front of every line is what makes a hex dump navigable: it is
        // the only way to say which byte a character is.
        hex += QStringLiteral("%1  ").arg(static_cast<qulonglong>(offset), 4, 16, QLatin1Char('0'));

        for (std::size_t index = 0; index < kBytesPerLine; ++index) {
            const std::size_t at = offset + index;
            if (at >= count) {
                break;
            }
            const std::uint8_t byte = bytes_[at];
            if (index > 0) {
                hex += QLatin1Char(' ');
            }
            hex += QStringLiteral("%1").arg(static_cast<qulonglong>(byte), 2, 16, QLatin1Char('0'));
            ascii += isPrintable(byte) ? QString(QChar::fromLatin1(static_cast<char>(byte)))
                                       : QStringLiteral(".");
        }
        hex += QLatin1Char('\n');
        ascii += QLatin1Char('\n');
    }

    hex_pane_->setPlainText(hex);
    ascii_pane_->setPlainText(ascii);
}

std::string SerialHexView::hexText() const {
    return hex_pane_->toPlainText().toStdString();
}

std::string SerialHexView::asciiText() const {
    return ascii_pane_->toPlainText().toStdString();
}

}  // namespace openscrt::ui
