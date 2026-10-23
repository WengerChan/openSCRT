// Primary / alternate screen grid.
//
// Roadmap 3.2 requires full VT100/VT220/xterm fidelity, so the grid owns cursor state,
// scroll region, alternate buffer and dirty tracking. It deliberately knows nothing about
// escape-sequence parsing (that is VtermStateMachine's job) or drawing (that is the
// renderer's), which keeps the two hardest-to-test parts separable.

#ifndef OPENCRT_TERMINAL_BUFFER_SCREEN_GRID_H_
#define OPENCRT_TERMINAL_BUFFER_SCREEN_GRID_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "terminal/buffer/Cell.h"
#include "terminal/buffer/RingScrollbackBuffer.h"

namespace openscrt::terminal {

enum class EraseInLine : int {
    kCursorToEnd = 0,
    kCursorToStart = 1,
    kAll = 2,
};

enum class EraseInDisplay : int {
    kCursorToEnd = 0,
    kCursorToStart = 1,
    kAll = 2,
};

class ScreenGrid {
 public:
    ScreenGrid() = default;
    ScreenGrid(int rows, int cols);

    ScreenGrid(const ScreenGrid&) = delete;
    ScreenGrid& operator=(const ScreenGrid&) = delete;
    ScreenGrid(ScreenGrid&&) noexcept = default;
    ScreenGrid& operator=(ScreenGrid&&) noexcept = default;

    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] bool empty() const noexcept { return rows_ <= 0 || cols_ <= 0; }

    // Resizes while preserving content by origin (top-left), which matches how xterm
    // reflows for the common grow/shrink case; origin mode is not yet reflow-aware.
    void resize(int rows, int cols);

    [[nodiscard]] Cell& at(int row, int col);
    [[nodiscard]] const Cell& at(int row, int col) const;
    [[nodiscard]] Cell* rowData(int row) noexcept;
    [[nodiscard]] const Cell* rowData(int row) const noexcept;

    [[nodiscard]] int cursorRow() const noexcept { return cursor_row_; }
    [[nodiscard]] int cursorColumn() const noexcept { return cursor_column_; }
    void setCursor(int row, int col) noexcept;
    // Relative motion clamps to the grid instead of wrapping; IND/NEL semantics.
    void moveCursor(int delta_row, int delta_column) noexcept;

    // Writes one scalar value at the cursor and advances. Handles auto-wrap, wide glyphs and
    // scroll-at-bottom-of-screen.
    void put(std::uint32_t codepoint, const CellStyle& style);
    void put(std::uint32_t codepoint) { put(codepoint, CellStyle{}); }

    void lineFeed();
    void carriageReturn();
    void backspace();
    void reverseIndex();
    void tabForward(int count = 1);
    void setTabStop(int column);
    void clearAllTabStops();

    void eraseInLine(EraseInLine scope);
    void eraseInDisplay(EraseInDisplay scope);
    void eraseCharacters(int count);

    // The attributes an erase paints with.
    //
    // xterm paints erased cells with the *current* background colour, not with the default
    // one. A program that sets a background and then erases a line expects the line to
    // keep that background, and dropping it is visible: htop's alternating row
    // backgrounds disappear if the erase falls back to the default colour.
    void setEraseStyle(const CellStyle& style) noexcept { erase_style_ = style; }

    // Blanks `count` cells from the cursor and pulls the remainder of the row left, leaving
    // blanks at the right margin (CSI P / DCH).
    void deleteCharacters(int count);

    void scrollUp(int lines);
    void scrollDown(int lines);

    void setScrollRegion(int top, int bottom);  // 0-based, inclusive
    void resetScrollRegion() noexcept;
    [[nodiscard]] int scrollTop() const noexcept { return scroll_top_; }
    [[nodiscard]] int scrollBottom() const noexcept { return scroll_bottom_; }

    void enterAlternateScreen();
    void leaveAlternateScreen();
    [[nodiscard]] bool isAlternateScreen() const noexcept { return active_ == &alternate_; }

    // Full RIS-style reset of the active buffer.
    void reset();

    void markRowDirty(int row) noexcept;
    void clearDirtyFlags() noexcept;
    [[nodiscard]] int dirtyRowCount() const noexcept;
    [[nodiscard]] bool rowIsDirty(int row) const noexcept;

    // Row-granular dirty flags.
    //
    // Scanning every cell to find damage costs O(rows * cols) on every frame, which is the
    // same work whether one row changed or all of them did -- and that fixed cost is what
    // capped the measured incremental-versus-full repaint ratio at 1.3x. The bitmap makes
    // the scan O(rows) and clearing proportional to the damage instead of the screen.
    void setRowDirty(int row) noexcept;
    [[nodiscard]] bool rowDirtyFlag(int row) const noexcept;
    void clearDirtyRowFlags() noexcept;

    // UTF-8 rendering of a row with trailing blanks trimmed; used by search, copy, logging
    // and the unit tests.
    [[nodiscard]] std::string rowText(int row) const;
    [[nodiscard]] static std::string toUtf8(const Cell* cells, int count);

    // Moves lines that scrolled off the top into scrollback. Returns how many were moved.
    std::size_t flushScrolledOffTo(RingScrollbackBuffer& scrollback);

    // Invoked with the text of each row that leaves the top of the screen, before it is
    // overwritten. Only fires when the scroll region starts at row 0, which is the only case
    // where the line genuinely left the session's visible history.
    using ScrolledOffHandler = std::function<void(std::string_view)>;
    void setScrolledOffHandler(ScrolledOffHandler handler) { scrolled_off_ = std::move(handler); }

 private:
    void clampCursor() noexcept;

    int rows_ = 0;
    int cols_ = 0;
    std::vector<Cell> primary_;
    std::vector<Cell> alternate_;
    std::vector<Cell>* active_ = nullptr;
    std::vector<int> tab_stops_;

    int cursor_row_ = 0;
    int cursor_column_ = 0;
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;
    // Set when the cursor sat on the last column after a put; the next put wraps first.
    // This is what makes xterm-compatible right-margin behaviour work.
    bool pending_wrap_ = false;
    CellStyle erase_style_{};
    ScrolledOffHandler scrolled_off_;
    std::vector<std::uint32_t> dirty_rows_;  // bit per row
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_BUFFER_SCREEN_GRID_H_