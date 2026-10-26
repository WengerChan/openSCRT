// The hex monitor of a serial line (P3-T2).
//
// Two panes side by side: the bytes as hex on the left, the same bytes as characters on
// the right. The point of the second pane is that a byte that is not printable has to
// be recognisable at a glance -- a dot says "there is a byte here and it is not text",
// which a hex dump on its own does not tell you.
//
// The view keeps a rolling window of the most recent bytes. A line that is watched for
// an hour would otherwise grow without bound, and a monitor that eats the memory it is
// supposed to be watching is worse than no monitor.

#ifndef OPENCRT_UI_WORKSPACE_SERIAL_HEX_VIEW_H_
#define OPENCRT_UI_WORKSPACE_SERIAL_HEX_VIEW_H_

#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

class QPlainTextEdit;

namespace openscrt::ui {

class SerialHexView : public QWidget {
    Q_OBJECT

 public:
    // How many bytes the view keeps. The default is 4096: 256 lines of sixteen bytes.
    explicit SerialHexView(std::size_t capacity = 4096, QWidget* parent = nullptr);
    ~SerialHexView() override;

    SerialHexView(const SerialHexView&) = delete;
    SerialHexView& operator=(const SerialHexView&) = delete;

    // Adds bytes to the window. The oldest bytes fall out of it once the window is
    // full.
    void append(const std::uint8_t* data, std::size_t size);
    void append(std::string_view text);

    void clear();

    // What the two panes show. These are the text of the panes, so a test can read the
    // view without a screen.
    [[nodiscard]] std::string hexText() const;
    [[nodiscard]] std::string asciiText() const;

    [[nodiscard]] std::size_t byteCount() const noexcept { return bytes_.size(); }

 private:
    void render();

    std::size_t capacity_;
    std::deque<std::uint8_t> bytes_;

    QPlainTextEdit* hex_pane_{nullptr};
    QPlainTextEdit* ascii_pane_{nullptr};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_WORKSPACE_SERIAL_HEX_VIEW_H_
