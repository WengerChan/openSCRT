// Terminal canvas widget (P1-T7).
//
// The widget is the seam between the emulator core and the user: it owns the frame loop,
// paints the renderer's backing image, and turns key events into terminal input bytes.
//
// The frame loop is the only place update() is called, and it is called from the coalescer's
// decision rather than from the transport. That is the structural expression of the roadmap's
// "never call update() per packet" rule: there is no path from incoming bytes to a repaint
// that bypasses the 16 ms coalescer.

#ifndef OPENCRT_UI_WORKSPACE_TERMINAL_CANVAS_WIDGET_H_
#define OPENCRT_UI_WORKSPACE_TERMINAL_CANVAS_WIDGET_H_

#include <QKeyEvent>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/buffer/RingScrollbackBuffer.h"
#include "terminal/buffer/ScreenGrid.h"
#include "terminal/emulator/TerminalEmulator.h"
#include "terminal/renderer/FrameCoalescer.h"
#include "ui/renderer/TerminalRenderer.h"

namespace openscrt::ui {

class TerminalCanvasWidget : public QWidget {
    Q_OBJECT

 public:
    explicit TerminalCanvasWidget(QWidget* parent = nullptr);
    ~TerminalCanvasWidget() override;

    TerminalCanvasWidget(const TerminalCanvasWidget&) = delete;
    TerminalCanvasWidget& operator=(const TerminalCanvasWidget&) = delete;

    void setFont(const QFont& font);
    void setTheme(const Theme& theme);

    // Feeds transport bytes. Never repaints directly; it only signals the coalescer.
    void feed(const std::uint8_t* bytes, std::size_t size);
    void feed(std::string_view text);

    // Resizes the terminal to fit the widget, preserving the current column count when
    // possible. Returns the new size in cells.
    struct CellSize {
        int rows{24};
        int columns{80};
    };
    CellSize resizeToWidget();

    // Encodes a key press into the bytes a terminal application sends to a shell. Returns an
    // empty vector for keys that are not terminal input (Ctrl+C handled separately, function
    // keys, and so on).
    [[nodiscard]] static std::vector<std::uint8_t> encodeKey(int key,
                                                             Qt::KeyboardModifiers modifiers,
                                                             const QString& text);

    // Convenience for the common case: the bytes a key press should produce.
    [[nodiscard]] std::vector<std::uint8_t> keySequenceFor(const QKeyEvent& event) const;

    [[nodiscard]] terminal::ScreenGrid& grid() noexcept { return *grid_; }
    [[nodiscard]] terminal::TerminalEmulator& emulator() noexcept { return *emulator_; }
    [[nodiscard]] const TerminalRenderer& renderer() const noexcept { return *renderer_; }

    // Draws one frame: repaints the damaged rows and asks for the blit.
    //
    // The frame loop calls this, and so does the frame-budget measurement, so both
    // measure the same work. Driving the widget from outside would otherwise mean
    // timing the blit alone, which is the cheap half of a frame.
    void drawFrame();

    // Frame statistics, used by the P1-A3 measurement.
    [[nodiscard]] int frameCount() const noexcept { return frames_drawn_; }
    [[nodiscard]] int repaintedRowCount() const noexcept { return repainted_rows_; }
    [[nodiscard]] int coalescedEventCount() const noexcept {
        return static_cast<int>(coalescer_.stats().coalesced_events);
    }

    // The bytes the widget would send to the transport. The widget does not own a transport;
    // it hands the encoded bytes to whoever owns the session.
    using InputSink = std::function<void(const std::vector<std::uint8_t>&)>;
    void setInputSink(InputSink sink) { input_sink_ = std::move(sink); }

 signals:
    // The terminal needs a different size on the far end (SIGWINCH / NAWS).
    void windowSizeChanged(int rows, int columns);
    // A key that the terminal should act on itself, such as Ctrl+C.
    void interruptRequested();
    // Alt+B: a break on the line, for the transports that have one (P3-T2).
    void breakRequested();

 protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

 private:
    void onFrameTick();
    [[nodiscard]] int rowsForHeight() const;
    [[nodiscard]] int columnsForWidth() const;

    // Declaration order matches the constructor's member-initialiser list: grid_ and
    // scrollback_ first because the emulator and the renderer both hold pointers to them.
    std::unique_ptr<terminal::ScreenGrid> grid_;
    std::unique_ptr<terminal::RingScrollbackBuffer> scrollback_;
    std::unique_ptr<terminal::TerminalEmulator> emulator_;
    std::unique_ptr<TerminalRenderer> renderer_;
    terminal::FrameCoalescer coalescer_{terminal::kDefaultRefreshHz};

    class QTimer* frame_timer_{nullptr};
    InputSink input_sink_;
    int frames_drawn_{0};
    int repainted_rows_{0};
    bool sized_{false};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_TERMINAL_CANVAS_WIDGET_H_