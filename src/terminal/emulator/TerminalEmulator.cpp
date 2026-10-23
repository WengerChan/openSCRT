#include "terminal/emulator/TerminalEmulator.h"

#include <algorithm>
#include <string>
#include <utility>

#include "terminal/emulator/CharWidth.h"
#include "terminal/emulator/Palette.h"

namespace openscrt::terminal {
namespace {

std::uint8_t clampToByte(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

}  // namespace

TerminalEmulator::TerminalEmulator(ScreenGrid& grid, RingScrollbackBuffer* scrollback)
    : grid_(&grid), scrollback_(scrollback) {
    grid_->setScrolledOffHandler([this](std::string_view line) { scrollOffLine(line); });
}

void TerminalEmulator::scrollOffLine(std::string_view line) {
    // vim, tmux and htop all run on the alternate screen; their output must not pollute the
    // session history.
    if (scrollback_ != nullptr && !grid_->isAlternateScreen()) {
        scrollback_->push(std::string(line));
    }
}

void TerminalEmulator::write(const std::uint8_t* data, std::size_t size) {
    parser_.feed(data, size);
}

void TerminalEmulator::write(std::string_view text) {
    parser_.feed(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void TerminalEmulator::reset() {
    parser_.reset();
    style_.reset();
    grid_->reset();
    saved_row_ = 0;
    saved_column_ = 0;
    origin_mode_ = false;
    auto_wrap_ = true;
    insert_mode_ = false;
    cursor_visible_ = true;
    device_response_.clear();
}

std::string TerminalEmulator::takeDeviceResponse() {
    return std::exchange(device_response_, {});
}

// ------------------------------------------------------------------------------- events

void TerminalEmulator::print(std::uint32_t codepoint) {
    CellStyle style = style_;
    style.setAttribute(cellattr::kWideLead, isWideCodepoint(codepoint));
    if (insert_mode_) {
        // IRM: existing cells shift right instead of being overwritten.
        grid_->put(' ', CellStyle{});
        grid_->moveCursor(0, -1);
        grid_->eraseCharacters(1);
        grid_->moveCursor(0, 1);
    }
    grid_->put(codepoint, style);
}

void TerminalEmulator::execute(std::uint8_t control) {
    switch (control) {
        case 0x07:  // BEL
        case 0x00:  // NUL: explicitly ignored, per xterm
            return;
        case 0x08:  // BS
            grid_->backspace();
            return;
        case 0x09:  // HT
            grid_->tabForward(1);
            return;
        case 0x0A:  // LF
        case 0x0B:  // VT
        case 0x0C:  // FF
            grid_->lineFeed();
            return;
        case 0x0D:  // CR
            grid_->carriageReturn();
            return;
        default:
            return;
    }
}

void TerminalEmulator::escapeDispatch(std::span<const std::uint8_t> intermediates, char final) {
    if (final == '7') {  // DECSC
        saved_row_ = grid_->cursorRow();
        saved_column_ = grid_->cursorColumn();
        return;
    }
    if (final == '8') {  // DECRC
        grid_->setCursor(saved_row_, saved_column_);
        return;
    }
    if (final == 'M') {  // RI, reverse index
        grid_->reverseIndex();
        return;
    }
    if (final == 'D') {  // IND
        grid_->lineFeed();
        return;
    }
    if (final == 'E') {  // NEL
        grid_->carriageReturn();
        grid_->lineFeed();
        return;
    }
    if (final == 'c') {  // RIS
        reset();
        return;
    }
    // Character-set designators such as ESC ( 0 select G0. UTF-8 is assumed throughout, so
    // they are accepted and ignored rather than half-implemented.
    (void)intermediates;
}

void TerminalEmulator::deviceResponse(std::string_view response) {
    device_response_.append(response);
}

// ---------------------------------------------------------------------------------- CSI

int TerminalEmulator::paramOr(const CsiSequence& sequence, std::size_t index, int fallback) const {
    if (index >= sequence.params.size()) {
        return fallback;
    }
    const int value = sequence.params[index];
    return value == kDefaultParam ? fallback : value;
}

bool TerminalEmulator::isSubParam(const CsiSequence& sequence, std::size_t index) const {
    return index < sequence.sub_params.size() && sequence.sub_params[index] != 0;
}

void TerminalEmulator::csiDispatch(const CsiSequence& sequence) {
    const char final = sequence.final;

    if (final == 'h' || final == 'l') {
        if (sequence.private_marker == '?') {
            applyModeChange(sequence, final == 'h');
        }
        return;
    }
    switch (final) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case '`':
        case 'd':
        case 'f':
            applyCursorMovement(sequence, final);
            return;
        case 'J':
            applyEraseDisplay(sequence);
            return;
        case 'K':
            applyEraseLine(sequence);
            return;
        case 'L': {  // IL
            const int count = std::max(1, paramOr(sequence, 0, 1));
            for (int i = 0; i < count; ++i) {
                grid_->scrollDown(1);
            }
            return;
        }
        case 'M': {  // DL
            const int count = std::max(1, paramOr(sequence, 0, 1));
            for (int i = 0; i < count; ++i) {
                grid_->scrollUp(1);
            }
            return;
        }
        case 'S':
        case 'T':
            applyScroll(sequence, final);
            return;
        case 'X':  // ECH: blank n cells from the cursor, without moving it.
            grid_->setEraseStyle(style_);
            grid_->eraseCharacters(std::max(1, paramOr(sequence, 0, 1)));
            return;
        case 'P':  // DCH: delete n cells, pulling the rest of the row left.
            grid_->deleteCharacters(std::max(1, paramOr(sequence, 0, 1)));
            return;
        case 'm':
            applyGraphicRendition(sequence);
            return;
        case 'r':
            // DECSTBM is 1-based and inclusive, and the region must be non-empty.
            if (!sequence.params.empty()) {
                const int top = paramOr(sequence, 0, 1) - 1;
                const int bottom = paramOr(sequence, 1, grid_->rows()) - 1;
                if (top >= 0 && bottom < grid_->rows() && top < bottom) {
                    grid_->setScrollRegion(top, bottom);
                }
            }
            return;
        case 'c':  // DA: report as a VT102 with ANSI colour.
            device_response_ += "\x1B[?6c";
            return;
        case 'n': {  // DSR
            const int request = paramOr(sequence, 0, 0);
            if (request == 5) {
                device_response_ += "\x1B[0n";
            } else if (request == 6) {
                device_response_ += "\x1B[" + std::to_string(grid_->cursorRow() + 1) + ";" +
                                    std::to_string(grid_->cursorColumn() + 1) + "R";
            }
            return;
        }
        default:
            return;
    }
}

void TerminalEmulator::applyModeChange(const CsiSequence& sequence, bool enable) {
    for (std::size_t i = 0; i < sequence.params.size(); ++i) {
        switch (paramOr(sequence, i, -1)) {
            case 5:  // DECSCNM: reverse video for the whole screen.
                style_.setAttribute(cellattr::kInverse, enable);
                break;
            case 6:  // DECOM
                origin_mode_ = enable;
                grid_->setCursor(0, 0);
                break;
            case 7:  // DECAWM
                auto_wrap_ = enable;
                break;
            case 25:  // DECTCEM
                cursor_visible_ = enable;
                break;
            case 47:
            case 1047:
            case 1049:
                // 1049 also saves/restores the cursor, matching DECSC/DECRC.
                if (enable && !grid_->isAlternateScreen()) {
                    saved_row_ = grid_->cursorRow();
                    saved_column_ = grid_->cursorColumn();
                    grid_->enterAlternateScreen();
                } else if (!enable && grid_->isAlternateScreen()) {
                    grid_->leaveAlternateScreen();
                    grid_->setCursor(saved_row_, saved_column_);
                }
                break;
            default:
                break;
        }
    }
}

void TerminalEmulator::applyCursorMovement(const CsiSequence& sequence, char final) {
    const int count = std::max(1, paramOr(sequence, 0, 1));
    const int rows = grid_->rows();
    const int cols = grid_->cols();

    switch (final) {
        case 'A':
            grid_->moveCursor(-count, 0);
            return;
        case 'B':
            grid_->moveCursor(count, 0);
            return;
        case 'C':
            grid_->moveCursor(0, count);
            return;
        case 'D':
            grid_->moveCursor(0, -count);
            return;
        case 'E':  // CNL
            grid_->setCursor(grid_->cursorRow() + count, 0);
            return;
        case 'F':  // CPL
            grid_->setCursor(grid_->cursorRow() - count, 0);
            return;
        case 'G':  // CHA
            grid_->setCursor(grid_->cursorRow(), count - 1);
            return;
        case 'd':  // VPA
            grid_->setCursor(count - 1, grid_->cursorColumn());
            return;
        case '`':  // HPA
            grid_->setCursor(grid_->cursorRow(), count - 1);
            return;
        case 'H':
        case 'f': {  // CUP, 1-based; the row defaults to 1 and the column defaults to 1.
            int row = paramOr(sequence, 0, 1) - 1;
            const int col = paramOr(sequence, 1, 1) - 1;
            if (origin_mode_) {
                row += grid_->scrollTop();
            }
            grid_->setCursor(std::clamp(row, 0, rows - 1), std::clamp(col, 0, cols - 1));
            return;
        }
        default:
            return;
    }
}

void TerminalEmulator::applyEraseDisplay(const CsiSequence& sequence) {
    // An erase paints with the background that is in force, so the grid has to know it.
    grid_->setEraseStyle(style_);
    switch (paramOr(sequence, 0, 0)) {
        case 0:
            grid_->eraseInDisplay(EraseInDisplay::kCursorToEnd);
            return;
        case 1:
            grid_->eraseInDisplay(EraseInDisplay::kCursorToStart);
            return;
        default:
            grid_->eraseInDisplay(EraseInDisplay::kAll);
            return;
    }
}

void TerminalEmulator::applyEraseLine(const CsiSequence& sequence) {
    grid_->setEraseStyle(style_);
    switch (paramOr(sequence, 0, 0)) {
        case 0:
            grid_->eraseInLine(EraseInLine::kCursorToEnd);
            return;
        case 1:
            grid_->eraseInLine(EraseInLine::kCursorToStart);
            return;
        default:
            grid_->eraseInLine(EraseInLine::kAll);
            return;
    }
}

void TerminalEmulator::applyScroll(const CsiSequence& sequence, char final) {
    const int count = std::max(1, paramOr(sequence, 0, 1));
    if (final == 'S') {
        for (int i = 0; i < count; ++i) {
            grid_->scrollUp(1);
        }
    } else {
        for (int i = 0; i < count; ++i) {
            grid_->scrollDown(1);
        }
    }
}

// ------------------------------------------------------------------------------- SGR

void TerminalEmulator::applyGraphicRendition(const CsiSequence& sequence) {
    const auto params = sequence.params;

    // An empty CSI m resets, same as an explicit 0.
    if (params.empty()) {
        style_.reset();
        return;
    }

    for (std::size_t i = 0; i < params.size(); ++i) {
        const int code = params[i] == kDefaultParam ? 0 : params[i];

        switch (code) {
            case 0:
                style_.reset();
                continue;
            case 1:
                style_.setAttribute(cellattr::kBold, true);
                continue;
            case 2:
                style_.setAttribute(cellattr::kBold, true);  // Faint: reuse the bold slot.
                continue;
            case 3:
                style_.setAttribute(cellattr::kItalic, true);
                continue;
            case 4:
                style_.setAttribute(cellattr::kUnderline, true);
                continue;
            case 5:
            case 6:
                style_.setAttribute(cellattr::kBlink, true);
                continue;
            case 7:
                style_.setAttribute(cellattr::kInverse, true);
                continue;
            case 8:
                style_.setAttribute(cellattr::kInvisible, true);
                continue;
            case 9:
                style_.setAttribute(cellattr::kStrikeThrough, true);
                continue;
            case 21:
            case 22:
                style_.setAttribute(cellattr::kBold, false);
                continue;
            case 23:
                style_.setAttribute(cellattr::kItalic, false);
                continue;
            case 24:
                style_.setAttribute(cellattr::kUnderline, false);
                continue;
            case 25:
                style_.setAttribute(cellattr::kBlink, false);
                continue;
            case 27:
                style_.setAttribute(cellattr::kInverse, false);
                continue;
            case 28:
                style_.setAttribute(cellattr::kInvisible, false);
                continue;
            case 29:
                style_.setAttribute(cellattr::kStrikeThrough, false);
                continue;
            case 39:
                style_.fg = 0;
                style_.setAttribute(cellattr::kDefaultFg, true);
                continue;
            case 49:
                style_.bg = 0;
                style_.setAttribute(cellattr::kDefaultBg, true);
                continue;
            default:
                break;
        }

        if (code >= 30 && code <= 37) {
            style_.fg = paletteColor(code - 30);
            style_.setAttribute(cellattr::kDefaultFg, false);
            continue;
        }
        if (code >= 90 && code <= 97) {
            style_.fg = paletteColor(code - 90 + 8);
            style_.setAttribute(cellattr::kDefaultFg, false);
            continue;
        }
        if (code >= 40 && code <= 47) {
            style_.bg = paletteColor(code - 40);
            style_.setAttribute(cellattr::kDefaultBg, false);
            continue;
        }
        if (code >= 100 && code <= 107) {
            style_.bg = paletteColor(code - 100 + 8);
            style_.setAttribute(cellattr::kDefaultBg, false);
            continue;
        }

        // 38/48 select an extended colour, and both spellings exist in the wild:
        //     semicolon: ESC [ 38 ; 2 ; r ; g ; b m      and     ESC [ 38 ; 5 ; n m
        //     colon:    ESC [ 38 : 2 : r : g ; b m      and     ESC [ 38 : 5 : n m
        // The colon form inserts an omitted sub-parameter between the selector and the
        // components (38:2::r:g:b), so the components do not start at the same offset in
        // both spellings. Both are read from the same flat array by locating the selector
        // first, then skipping any omitted sub-parameter that follows it.
        if (code == 38 || code == 48) {
            const bool foreground = code == 38;
            const std::size_t selector_index = i + 1;
            if (selector_index >= params.size()) {
                continue;
            }
            const int selector = params[selector_index];

            std::size_t components = selector_index + 1;
            if (isSubParam(sequence, selector_index)) {
                // The colon spelling of a direct colour is normally written
                // 38:2:<colour-space>:r:g:b, with the colour-space slot present and
                // possibly empty. Terminals in the wild also emit the short form
                // 38:2:r:g:b, and tmux accepts it, so the colour-space slot is consumed
                // only when it is actually there.
                while (components < params.size() && params[components] == kDefaultParam) {
                    ++components;
                }
            }

            ColorRgba resolved = 0;
            std::size_t consumed = 0;
            bool valid = false;
            if (selector == 5 && components < params.size()) {
                resolved = paletteColor(params[components]);
                consumed = components - i;
                valid = true;
            } else if (selector == 2 && components + 2 < params.size()) {
                resolved =
                    makeRgba(clampToByte(params[components]), clampToByte(params[components + 1]),
                             clampToByte(params[components + 2]));
                consumed = components + 2 - i;
                valid = true;
            } else {
                // Malformed. The whole colour group is consumed and ignored, the way xterm
                // ignores a sequence whose parameters do not add up. Letting the leftovers
                // fall through to the SGR loop would turn the "0" of 38:2:255:0:0 into a
                // reset, which is a different bug wearing the same costume.
                consumed = params.size() - i;
            }

            if (valid) {
                if (foreground) {
                    style_.fg = resolved;
                    style_.setAttribute(cellattr::kDefaultFg, false);
                } else {
                    style_.bg = resolved;
                    style_.setAttribute(cellattr::kDefaultBg, false);
                }
            }
            i += consumed;
        }
    }
}

// ------------------------------------------------------------------------------- OSC

void TerminalEmulator::oscDispatch(const OscSequence& sequence) {
    // Phase 1 consumes OSC 0/1/2 (window title); the rest are ignored rather than surfaced.
    // Silently dropping OSC 4 (palette redefinition) or OSC 52 (clipboard) would be visible
    // to the user, so they are noted here as known gaps rather than pretended support.
    switch (sequence.identifier) {
        case 0:
        case 1:
        case 2:
            return;
        default:
            return;
    }
}

}  // namespace openscrt::terminal