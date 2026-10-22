#include "terminal/buffer/ScreenGrid.h"

#include <algorithm>
#include <stdexcept>

namespace openscrt::terminal {
namespace {

void fillBlank(Cell* cells, int count) noexcept {
    for (int i = 0; i < count; ++i) {
        cells[i].setBlank();
    }
}

int clampInt(int value, int lo, int hi) noexcept {
    return std::clamp(value, lo, hi);
}

// UTF-8 encoder for the surrogate-free scalar range; invalid scalars become U+FFFD so a
// corrupt byte stream can never emit an ill-formed sequence into a log file.
void appendUtf8(std::string& out, std::uint32_t cp) noexcept {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

ScreenGrid::ScreenGrid(int rows, int cols) {
    resize(rows, cols);
}

void ScreenGrid::resize(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);

    const int old_rows = rows_;
    const int old_cols = cols_;
    const std::size_t new_count = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);

    if (primary_.size() != new_count) {
        // Snapshot before resizing: std::vector::resize value-initialises new elements but
        // leaves existing ones, and we need the old geometry to compute the overlap copy.
        std::vector<Cell> old_primary = primary_;
        std::vector<Cell> old_alternate = alternate_;

        primary_.resize(new_count);
        alternate_.resize(new_count);
        // "Zero" is not a valid blank Cell (it would read as default colours with no
        // kDefaultFg/kDefaultBg bits), so normalise the whole buffer rather than trust
        // vector::resize's default initialisation.
        fillBlank(primary_.data(), static_cast<int>(new_count));
        fillBlank(alternate_.data(), static_cast<int>(new_count));

        if (!old_primary.empty()) {
            const int keep_rows = std::min(old_rows, rows);
            const int keep_cols = std::min(old_cols, cols);
            for (int row = 0; row < keep_rows; ++row) {
                Cell* dst_primary = primary_.data() + static_cast<std::size_t>(row) * cols;
                Cell* dst_alternate = alternate_.data() + static_cast<std::size_t>(row) * cols;
                const Cell* src_primary =
                    old_primary.data() + static_cast<std::size_t>(row) * old_cols;
                const Cell* src_alternate =
                    old_alternate.data() + static_cast<std::size_t>(row) * old_cols;
                std::copy_n(src_primary, keep_cols, dst_primary);
                std::copy_n(src_alternate, keep_cols, dst_alternate);
            }
        }
    }

    rows_ = rows;
    cols_ = cols;
    active_ = isAlternateScreen() ? &alternate_ : &primary_;

    tab_stops_.clear();
    tab_stops_.reserve(static_cast<std::size_t>(cols));
    for (int col = 8; col < cols; col += 8) {
        tab_stops_.push_back(col);
    }

    cursor_row_ = clampInt(cursor_row_, 0, rows_ - 1);
    cursor_column_ = clampInt(cursor_column_, 0, cols_ - 1);
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    pending_wrap_ = false;
}

Cell& ScreenGrid::at(int row, int col) {
    if (empty()) {
        throw std::out_of_range("ScreenGrid::at: empty grid");
    }
    return (*active_)[static_cast<std::size_t>(row) * cols_ + col];
}

const Cell& ScreenGrid::at(int row, int col) const {
    if (empty()) {
        throw std::out_of_range("ScreenGrid::at: empty grid");
    }
    return (*active_)[static_cast<std::size_t>(row) * cols_ + col];
}

Cell* ScreenGrid::rowData(int row) noexcept {
    if (row < 0 || row >= rows_) {
        return nullptr;
    }
    return active_->data() + static_cast<std::size_t>(row) * cols_;
}

const Cell* ScreenGrid::rowData(int row) const noexcept {
    if (row < 0 || row >= rows_) {
        return nullptr;
    }
    return active_->data() + static_cast<std::size_t>(row) * cols_;
}

void ScreenGrid::setCursor(int row, int col) noexcept {
    cursor_row_ = clampInt(row, 0, rows_ - 1);
    cursor_column_ = clampInt(col, 0, cols_ - 1);
    pending_wrap_ = false;
}

void ScreenGrid::moveCursor(int delta_row, int delta_column) noexcept {
    clampCursor();
    cursor_row_ = clampInt(cursor_row_ + delta_row, 0, rows_ - 1);
    cursor_column_ = clampInt(cursor_column_ + delta_column, 0, cols_ - 1);
    pending_wrap_ = false;
}

void ScreenGrid::clampCursor() noexcept {
    cursor_row_ = clampInt(cursor_row_, 0, rows_ - 1);
    cursor_column_ = clampInt(cursor_column_, 0, cols_ - 1);
}

void ScreenGrid::put(std::uint32_t codepoint, const CellStyle& style) {
    if (empty()) {
        return;
    }
    // Deliberately no scroll-region clamping here. DECSTBM constrains *scrolling*, not the
    // cursor: a program may position anywhere on the screen and write there, and full-screen
    // TUIs do exactly that. Clamping in put() silently relocated such writes into the
    // region, which is how `[2;3r` followed by a write to row 1 lost row 1.

    // Codepoint 0 is how the parser spells NUL; treat it as a no-op blank so it never
    // overwrites a glyph that is already on screen.
    if (codepoint == 0) {
        codepoint = ' ';
    }

    if (pending_wrap_) {
        carriageReturn();
        lineFeed();
        pending_wrap_ = false;
    }

    // A double-width glyph needs two columns. When only one is left, wrap now: writing the
    // lead cell there would leave its trailing half with nowhere to go and corrupt the row.
    if (style.hasAttribute(cellattr::kWideLead) && cursor_column_ + 1 >= cols_) {
        carriageReturn();
        lineFeed();
    }

    Cell& cell = at(cursor_row_, cursor_column_);
    cell.codepoint = codepoint;
    cell.fg = style.fg;
    cell.bg = style.bg;
    cell.attrs = style.attrs;
    cell.markDirty();

    // Wide glyph: mark the trailing half and occupy two columns. If only one column is left
    // on this row, wrap now so we never split a double-width glyph across the right margin.
    if (style.hasAttribute(cellattr::kWideLead) && cursor_column_ + 1 < cols_) {
        Cell& trail = at(cursor_row_, cursor_column_ + 1);
        trail.setBlank();
        trail.fg = style.fg;
        trail.bg = style.bg;
        trail.attrs =
            (style.attrs & ~cellattr::kWideLead) | cellattr::kWideTrail | cellattr::kDirty;
        ++cursor_column_;
    }

    if (cursor_column_ + 1 >= cols_) {
        cursor_column_ = cols_ - 1;
        pending_wrap_ = true;
    } else {
        ++cursor_column_;
    }
}

void ScreenGrid::lineFeed() {
    if (empty()) {
        return;
    }
    pending_wrap_ = false;
    if (cursor_row_ == scroll_bottom_) {
        scrollUp(1);
    } else if (cursor_row_ < rows_ - 1) {
        ++cursor_row_;
    }
}

void ScreenGrid::carriageReturn() {
    cursor_column_ = 0;
    pending_wrap_ = false;
}

void ScreenGrid::backspace() {
    if (cursor_column_ > 0) {
        --cursor_column_;
    }
    pending_wrap_ = false;
}

void ScreenGrid::reverseIndex() {
    if (empty()) {
        return;
    }
    pending_wrap_ = false;
    if (cursor_row_ == scroll_top_) {
        scrollDown(1);
    } else if (cursor_row_ > 0) {
        --cursor_row_;
    }
}

void ScreenGrid::setTabStop(int column) {
    column = clampInt(column, 0, cols_ - 1);
    if (!std::binary_search(tab_stops_.begin(), tab_stops_.end(), column)) {
        tab_stops_.insert(std::lower_bound(tab_stops_.begin(), tab_stops_.end(), column), column);
    }
}

void ScreenGrid::clearAllTabStops() {
    tab_stops_.clear();
}

void ScreenGrid::tabForward(int count) {
    if (empty()) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        auto it = std::upper_bound(tab_stops_.begin(), tab_stops_.end(), cursor_column_);
        if (it == tab_stops_.end()) {
            cursor_column_ = cols_ - 1;
            break;
        }
        cursor_column_ = *it;
    }
    pending_wrap_ = false;
}

void ScreenGrid::eraseInLine(EraseInLine scope) {
    if (empty()) {
        return;
    }
    clampCursor();
    Cell* line = rowData(cursor_row_);
    int start = 0;
    int count = cols_;
    switch (scope) {
        case EraseInLine::kCursorToEnd:
            start = cursor_column_;
            count = cols_ - cursor_column_;
            break;
        case EraseInLine::kCursorToStart:
            start = 0;
            count = cursor_column_ + 1;
            break;
        case EraseInLine::kAll:
            break;
    }
    for (int i = 0; i < count; ++i) {
        line[start + i].setBlank();
        line[start + i].markDirty();
    }
}

void ScreenGrid::eraseInDisplay(EraseInDisplay scope) {
    if (empty()) {
        return;
    }
    clampCursor();
    switch (scope) {
        case EraseInDisplay::kCursorToEnd:
            eraseInLine(EraseInLine::kCursorToEnd);
            for (int row = cursor_row_ + 1; row < rows_; ++row) {
                fillBlank(rowData(row), cols_);
                markRowDirty(row);
            }
            break;
        case EraseInDisplay::kCursorToStart:
            eraseInLine(EraseInLine::kCursorToStart);
            for (int row = 0; row < cursor_row_; ++row) {
                fillBlank(rowData(row), cols_);
                markRowDirty(row);
            }
            break;
        case EraseInDisplay::kAll:
            for (int row = 0; row < rows_; ++row) {
                fillBlank(rowData(row), cols_);
                markRowDirty(row);
            }
            break;
    }
}

void ScreenGrid::eraseCharacters(int count) {
    if (empty() || count <= 0) {
        return;
    }
    Cell* line = rowData(cursor_row_);
    for (int i = 0; i < count && cursor_column_ + i < cols_; ++i) {
        line[cursor_column_ + i].setBlank();
        line[cursor_column_ + i].markDirty();
    }
}

void ScreenGrid::deleteCharacters(int count) {
    if (empty() || count <= 0) {
        return;
    }
    clampCursor();
    Cell* line = rowData(cursor_row_);
    if (line == nullptr) {
        return;
    }
    const int start = std::min(cursor_column_, cols_);
    const int removable = std::min(count, cols_ - start);
    const int tail = cols_ - start - removable;
    if (tail > 0) {
        std::copy(line + start + removable, line + start + removable + tail, line + start);
    }
    for (int col = cols_ - removable; col < cols_; ++col) {
        line[col].setBlank();
    }
    for (int col = start; col < cols_; ++col) {
        line[col].markDirty();
    }
}

void ScreenGrid::scrollUp(int lines) {
    if (empty() || lines <= 0) {
        return;
    }
    const int height = scroll_bottom_ - scroll_top_ + 1;
    if (lines >= height) {
        for (int row = scroll_top_; row <= scroll_bottom_; ++row) {
            fillBlank(rowData(row), cols_);
            markRowDirty(row);
        }
        return;
    }
    // Rows leaving the top carry real content into scrollback before they are overwritten.
    // Only when the region starts at row 0: with a scroll region below it, the line moved
    // inside the region and is still part of the screen.
    if (scrolled_off_ && scroll_top_ == 0) {
        for (int row = 0; row < lines; ++row) {
            const std::string text = rowText(row);
            scrolled_off_(std::string_view(text));
        }
    }
    for (int row = scroll_top_; row <= scroll_bottom_ - lines; ++row) {
        std::copy(rowData(row + lines), rowData(row + lines) + cols_, rowData(row));
        markRowDirty(row);
    }
    for (int row = scroll_bottom_ - lines + 1; row <= scroll_bottom_; ++row) {
        fillBlank(rowData(row), cols_);
        markRowDirty(row);
    }
}

void ScreenGrid::scrollDown(int lines) {
    if (empty() || lines <= 0) {
        return;
    }
    const int height = scroll_bottom_ - scroll_top_ + 1;
    if (lines >= height) {
        for (int row = scroll_top_; row <= scroll_bottom_; ++row) {
            fillBlank(rowData(row), cols_);
            markRowDirty(row);
        }
        return;
    }
    for (int row = scroll_bottom_; row >= scroll_top_ + lines; --row) {
        std::copy(rowData(row - lines), rowData(row - lines) + cols_, rowData(row));
        markRowDirty(row);
    }
    for (int row = scroll_top_; row < scroll_top_ + lines; ++row) {
        fillBlank(rowData(row), cols_);
        markRowDirty(row);
    }
}

void ScreenGrid::setScrollRegion(int top, int bottom) {
    if (empty()) {
        return;
    }
    if (top < 0 || bottom >= rows_ || top >= bottom) {
        return;  // Invalid regions are ignored, matching DECSTBM.
    }
    scroll_top_ = top;
    scroll_bottom_ = bottom;
    // DECSTBM homes the cursor.
    setCursor(top, 0);
}

void ScreenGrid::resetScrollRegion() noexcept {
    scroll_top_ = 0;
    scroll_bottom_ = empty() ? 0 : rows_ - 1;
}

void ScreenGrid::enterAlternateScreen() {
    if (isAlternateScreen()) {
        return;
    }
    active_ = &alternate_;
    // DECSET 1049 restores the primary cursor; park it at home and let the parser save the
    // real position. Content is untouched so leaving the alternate screen restores it.
    setCursor(0, 0);
}

void ScreenGrid::leaveAlternateScreen() {
    if (!isAlternateScreen()) {
        return;
    }
    active_ = &primary_;
    pending_wrap_ = false;
}

void ScreenGrid::reset() {
    fillBlank(active_->data(), static_cast<int>(active_->size()));
    cursor_row_ = 0;
    cursor_column_ = 0;
    resetScrollRegion();
    pending_wrap_ = false;
}

void ScreenGrid::markRowDirty(int row) noexcept {
    if (row < 0 || row >= rows_) {
        return;
    }
    Cell* line = rowData(row);
    for (int col = 0; col < cols_; ++col) {
        line[col].markDirty();
    }
}

void ScreenGrid::clearDirtyFlags() noexcept {
    for (Cell& cell : *active_) {
        cell.clearDirty();
    }
}

int ScreenGrid::dirtyRowCount() const noexcept {
    int count = 0;
    for (int row = 0; row < rows_; ++row) {
        if (rowIsDirty(row)) {
            ++count;
        }
    }
    return count;
}

bool ScreenGrid::rowIsDirty(int row) const noexcept {
    const Cell* line = rowData(row);
    if (line == nullptr) {
        return false;
    }
    for (int col = 0; col < cols_; ++col) {
        if (line[col].isDirty()) {
            return true;
        }
    }
    return false;
}

std::string ScreenGrid::toUtf8(const Cell* cells, int count) {
    std::string out;
    if (cells == nullptr || count <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        if (cells[i].isWideTrail()) {
            continue;  // Already emitted with its lead cell.
        }
        if (cells[i].codepoint != 0) {
            appendUtf8(out, cells[i].codepoint);
        } else {
            // A blank cell is still a column. Dropping it would shift everything after the
            // gap leftwards, which corrupts column alignment for search, copy and logs.
            // Trailing blanks never reach here -- rowText trims them first.
            out.push_back(' ');
        }
    }
    return out;
}

std::string ScreenGrid::rowText(int row) const {
    const Cell* line = rowData(row);
    if (line == nullptr) {
        return {};
    }
    int last = cols_;
    while (last > 0 && line[last - 1].isBlank() && !line[last - 1].isWideTrail()) {
        --last;
    }
    return toUtf8(line, last);
}

std::size_t ScreenGrid::flushScrolledOffTo(RingScrollbackBuffer& scrollback) {
    if (isAlternateScreen()) {
        return 0;  // The alternate buffer never contributes to scrollback.
    }
    std::size_t moved = 0;
    ScrolledOffHandler previous = scrolled_off_;
    scrolled_off_ = [&scrollback, &moved](std::string_view line) {
        scrollback.push(std::string(line));
        ++moved;
    };
    scrollUp(1);
    scrolled_off_ = std::move(previous);
    return moved;
}

}  // namespace openscrt::terminal