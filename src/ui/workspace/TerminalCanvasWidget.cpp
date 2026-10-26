#include "ui/workspace/TerminalCanvasWidget.h"

#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

namespace openscrt::ui {
namespace {

constexpr int kFrameIntervalMs = 1000 / terminal::kDefaultRefreshHz;

void appendUtf8(std::vector<std::uint8_t>& out, const QString& text) {
    const QByteArray encoded = text.toUtf8();
    out.insert(out.end(), encoded.begin(), encoded.end());
}

}  // namespace

TerminalCanvasWidget::TerminalCanvasWidget(QWidget* parent)
    : QWidget(parent),
      grid_(std::make_unique<terminal::ScreenGrid>(24, 80)),
      scrollback_(std::make_unique<terminal::RingScrollbackBuffer>()),
      emulator_(std::make_unique<terminal::TerminalEmulator>(*grid_, scrollback_.get())),
      renderer_(std::make_unique<TerminalRenderer>(*grid_, scrollback_.get())) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);

    renderer_->setFont(font());

    frame_timer_ = new QTimer(this);
    frame_timer_->setInterval(kFrameIntervalMs);
    frame_timer_->setTimerType(Qt::PreciseTimer);
    connect(frame_timer_, &QTimer::timeout, this, &TerminalCanvasWidget::onFrameTick);
    frame_timer_->start();
}

TerminalCanvasWidget::~TerminalCanvasWidget() = default;

void TerminalCanvasWidget::setFont(const QFont& font) {
    renderer_->setFont(font);
    coalescer_.invalidate();
}

void TerminalCanvasWidget::setTheme(const Theme& theme) {
    renderer_->setTheme(theme);
    coalescer_.invalidate();
}

void TerminalCanvasWidget::feed(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size == 0) {
        return;
    }
    emulator_->write(bytes, size);
    coalescer_.signal();
}

void TerminalCanvasWidget::feed(std::string_view text) {
    feed(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

TerminalCanvasWidget::CellSize TerminalCanvasWidget::resizeToWidget() {
    const auto metrics = renderer_->cellMetrics();
    if (metrics.width <= 0 || metrics.height <= 0) {
        return CellSize{grid_->rows(), grid_->cols()};
    }
    const int rows = std::max(1, rowsForHeight());
    const int columns = std::max(1, columnsForWidth());
    if (rows != grid_->rows() || columns != grid_->cols()) {
        renderer_->resize(rows, columns);
        coalescer_.invalidate();
    }
    return CellSize{rows, columns};
}

int TerminalCanvasWidget::rowsForHeight() const {
    const auto metrics = renderer_->cellMetrics();
    return metrics.height > 0 ? height() / metrics.height : grid_->rows();
}

int TerminalCanvasWidget::columnsForWidth() const {
    const auto metrics = renderer_->cellMetrics();
    return metrics.width > 0 ? width() / metrics.width : grid_->cols();
}

std::vector<std::uint8_t> TerminalCanvasWidget::encodeKey(int key, Qt::KeyboardModifiers modifiers,
                                                          const QString& text) {
    std::vector<std::uint8_t> out;

    // Ctrl+C and friends are a session-level action, not bytes to send.
    if (modifiers.testFlag(Qt::ControlModifier)) {
        const QChar letter = text.isEmpty() ? QChar() : text.at(0).toLower();
        if (letter == QLatin1Char('c')) {
            return {};
        }
        if (letter.unicode() >= 'a' && letter.unicode() <= 'z') {
            out.push_back(static_cast<std::uint8_t>(letter.unicode() - 'a' + 1));
            return out;
        }
        return {};
    }

    switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            out.push_back('\r');
            return out;
        case Qt::Key_Backspace:
            out.push_back(0x7F);
            return out;
        case Qt::Key_Tab:
            out.push_back('\t');
            return out;
        case Qt::Key_Escape:
            out.push_back(0x1B);
            return out;
        case Qt::Key_Up:
            out.insert(out.end(), {0x1B, '[', 'A'});
            return out;
        case Qt::Key_Down:
            out.insert(out.end(), {0x1B, '[', 'B'});
            return out;
        case Qt::Key_Right:
            out.insert(out.end(), {0x1B, '[', 'C'});
            return out;
        case Qt::Key_Left:
            out.insert(out.end(), {0x1B, '[', 'D'});
            return out;
        case Qt::Key_Home:
            out.insert(out.end(), {0x1B, '[', 'H'});
            return out;
        case Qt::Key_End:
            out.insert(out.end(), {0x1B, '[', 'F'});
            return out;
        case Qt::Key_Delete:
            out.insert(out.end(), {0x1B, '[', '3', '~'});
            return out;
        case Qt::Key_PageUp:
            out.insert(out.end(), {0x1B, '[', '5', '~'});
            return out;
        case Qt::Key_PageDown:
            out.insert(out.end(), {0x1B, '[', '6', '~'});
            return out;
        default:
            break;
    }

    if (!text.isEmpty()) {
        appendUtf8(out, text);
    }
    return out;
}

std::vector<std::uint8_t> TerminalCanvasWidget::keySequenceFor(const QKeyEvent& event) const {
    return encodeKey(event.key(), event.modifiers(), event.text());
}

void TerminalCanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier) &&
        event->text().toLower() == QLatin1Char('c')) {
        emit interruptRequested();
        event->accept();
        return;
    }
    // Alt+B is a break on the line. It is not a byte to send: the transport that has a
    // break carries it out itself. Control is excluded because on Windows AltGr is
    // reported as Control+Alt, and AltGr+B is a character, not a break.
    if (event->modifiers().testFlag(Qt::AltModifier) &&
        !event->modifiers().testFlag(Qt::ControlModifier) && event->key() == Qt::Key_B) {
        emit breakRequested();
        event->accept();
        return;
    }
    const std::vector<std::uint8_t> bytes = keySequenceFor(*event);
    if (!bytes.empty()) {
        if (input_sink_) {
            input_sink_(bytes);
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TerminalCanvasWidget::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    QPainter painter(this);
    painter.drawImage(0, 0, renderer_->backbuffer());
}

void TerminalCanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const CellSize size = resizeToWidget();
    if (!sized_) {
        sized_ = true;
    }
    emit windowSizeChanged(size.rows, size.columns);
}

void TerminalCanvasWidget::drawFrame() {
    repainted_rows_ += renderer_->repaintDamage();
    ++frames_drawn_;
    update();
}

void TerminalCanvasWidget::onFrameTick() {
    if (!coalescer_.shouldRepaint(std::chrono::steady_clock::now())) {
        return;
    }
    drawFrame();
}

}  // namespace openscrt::ui