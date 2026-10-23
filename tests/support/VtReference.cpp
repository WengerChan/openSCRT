#include "support/VtReference.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "terminal/buffer/Cell.h"
#include "terminal/buffer/ScreenGrid.h"
#include "terminal/emulator/CharWidth.h"
#include "terminal/emulator/Palette.h"
#include "terminal/emulator/ScreenSnapshot.h"

namespace openscrt::test {
namespace {

using openscrt::terminal::Cell;
using openscrt::terminal::ColorRgba;
using openscrt::terminal::makeRgba;
using openscrt::terminal::paletteColor;
using openscrt::terminal::ScreenGrid;

// A namespace alias, not a using-declaration: C++ does not allow a using-declaration to
// name a namespace.
namespace cellattr = openscrt::terminal::cellattr;

// popen is spelled _popen on MSVC, and the undecorated name is deprecated there: with /WX
// it would be a compile error, not a warning.
FILE* openPipe(const std::string& command) {
#ifdef _WIN32
    return ::_popen(command.c_str(), "r");
#else
    return ::popen(command.c_str(), "r");
#endif
}

int closePipe(FILE* pipe) {
#ifdef _WIN32
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

// Runs a shell command and returns its standard output. The success flag is reported
// through the optional pointer; pclose's raw status is not portable, so only "zero or not"
// is used.
std::string runCommand(const std::string& command, bool* ok) {
    std::string output;
    FILE* pipe = openPipe(command);
    if (pipe == nullptr) {
        if (ok != nullptr) {
            *ok = false;
        }
        return output;
    }
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = closePipe(pipe);
    if (ok != nullptr) {
        *ok = (status == 0);
    }
    return output;
}

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (const std::uint8_t byte : bytes) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::vector<std::string> splitOn(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t position = text.find(separator, start);
        if (position == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, position - start));
        start = position + 1;
    }
}

// The state tmux reports through SGR sequences, mapped onto the attributes our cells carry.
struct SgrState {
    ColorRgba fg{0};
    ColorRgba bg{0};
    std::uint32_t attrs{cellattr::kDefaultFg | cellattr::kDefaultBg};

    void reset() {
        fg = 0;
        bg = 0;
        attrs = cellattr::kDefaultFg | cellattr::kDefaultBg;
    }

    void setForeground(ColorRgba color) {
        fg = color;
        attrs &= ~cellattr::kDefaultFg;
    }

    void clearForeground() {
        fg = 0;
        attrs |= cellattr::kDefaultFg;
    }

    void setBackground(ColorRgba color) {
        bg = color;
        attrs &= ~cellattr::kDefaultBg;
    }

    void clearBackground() {
        bg = 0;
        attrs |= cellattr::kDefaultBg;
    }

    void setFlag(std::uint32_t bit, bool on) {
        if (on) {
            attrs |= bit;
        } else {
            attrs &= ~bit;
        }
    }
};

std::uint8_t clampToByte(int value) {
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// tmux reports the basic and bright colours as SGR 30-37 / 90-97 (and the background
// equivalents). Both sides resolve the code through the project's own palette table, so
// the comparison is about the sequence, not about two different colour books.
void applyBasicCode(SgrState& state, int code) {
    switch (code) {
        case 0:
            state.reset();
            return;
        case 1:
        case 2:
            // Our emulator maps "faint" onto the bold slot, so the reference does too.
            state.setFlag(cellattr::kBold, true);
            return;
        case 3:
            state.setFlag(cellattr::kItalic, true);
            return;
        case 4:
            state.setFlag(cellattr::kUnderline, true);
            return;
        case 5:
        case 6:
            state.setFlag(cellattr::kBlink, true);
            return;
        case 9:
            state.setFlag(cellattr::kStrikeThrough, true);
            return;
        case 21:
        case 22:
            state.setFlag(cellattr::kBold, false);
            return;
        case 23:
            state.setFlag(cellattr::kItalic, false);
            return;
        case 24:
            state.setFlag(cellattr::kUnderline, false);
            return;
        case 25:
            state.setFlag(cellattr::kBlink, false);
            return;
        case 29:
            state.setFlag(cellattr::kStrikeThrough, false);
            return;
        case 39:
            state.clearForeground();
            return;
        case 49:
            state.clearBackground();
            return;
        default:
            break;
    }

    if (code >= 30 && code <= 37) {
        state.setForeground(paletteColor(code - 30));
        return;
    }
    if (code >= 90 && code <= 97) {
        state.setForeground(paletteColor(code - 90 + 8));
        return;
    }
    if (code >= 40 && code <= 47) {
        state.setBackground(paletteColor(code - 40));
        return;
    }
    if (code >= 100 && code <= 107) {
        state.setBackground(paletteColor(code - 100 + 8));
        return;
    }
}

// The colon spelling: "38:5:n", "38:2:r:g:b", and the "38:2::r:g:b" variant where an
// omitted sub-parameter sits between the selector and the components.
void applyColonGroup(SgrState& state, const std::string& group) {
    const std::vector<std::string> parts = splitOn(group, ':');
    std::vector<int> values;
    values.reserve(parts.size());
    for (const std::string& part : parts) {
        values.push_back(part.empty() ? -1 : std::atoi(part.c_str()));
    }
    if (values.empty()) {
        return;
    }

    if (values[0] == 38 || values[0] == 48) {
        if (values.size() < 2) {
            return;
        }
        const bool foreground = (values[0] == 38);
        const int selector = values[1];
        std::size_t component = 2;
        if (selector == 2) {
            // The direct-colour colon form always carries a colour-space slot, empty or
            // not. Consuming it keeps the reference in step with the emulator.
            ++component;
        } else {
            while (component < values.size() && values[component] < 0) {
                ++component;
            }
        }
        if (selector == 5 && component < values.size()) {
            const ColorRgba color = paletteColor(values[component]);
            if (foreground) {
                state.setForeground(color);
            } else {
                state.setBackground(color);
            }
        } else if (selector == 2 && component + 2 < values.size()) {
            const ColorRgba color =
                makeRgba(clampToByte(values[component]), clampToByte(values[component + 1]),
                         clampToByte(values[component + 2]));
            if (foreground) {
                state.setForeground(color);
            } else {
                state.setBackground(color);
            }
        }
        return;
    }

    for (const int value : values) {
        if (value >= 0) {
            applyBasicCode(state, value);
        }
    }
}

void applySgr(SgrState& state, const std::string& parameters) {
    const std::vector<std::string> groups = splitOn(parameters, ';');
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::string& group = groups[i];
        if (group.find(':') != std::string::npos) {
            applyColonGroup(state, group);
            continue;
        }

        const int code = group.empty() ? 0 : std::atoi(group.c_str());
        if (code != 38 && code != 48) {
            applyBasicCode(state, code);
            continue;
        }

        // The semicolon spelling continues in the following groups: "38;5;n" and
        // "38;2;r;g;b".
        if (i + 1 >= groups.size()) {
            continue;
        }
        const bool foreground = (code == 38);
        const int selector = std::atoi(groups[i + 1].c_str());
        if (selector == 5 && i + 2 < groups.size()) {
            const ColorRgba color = paletteColor(std::atoi(groups[i + 2].c_str()));
            if (foreground) {
                state.setForeground(color);
            } else {
                state.setBackground(color);
            }
            i += 2;
        } else if (selector == 2 && i + 4 < groups.size()) {
            const ColorRgba color = makeRgba(clampToByte(std::atoi(groups[i + 2].c_str())),
                                             clampToByte(std::atoi(groups[i + 3].c_str())),
                                             clampToByte(std::atoi(groups[i + 4].c_str())));
            if (foreground) {
                state.setForeground(color);
            } else {
                state.setBackground(color);
            }
            i += 4;
        }
    }
}

// Decodes one UTF-8 sequence starting at `index`. Returns the codepoint and advances the
// index past the sequence. Invalid bytes decode as U+FFFD and consume one byte.
std::uint32_t decodeUtf8(const std::string& text, std::size_t& index) {
    const auto byteAt = [&](std::size_t offset) {
        return static_cast<unsigned char>(text[index + offset]);
    };
    const unsigned char lead = byteAt(0);
    if (lead < 0x80) {
        ++index;
        return lead;
    }

    std::size_t extra = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        codepoint = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        codepoint = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        codepoint = lead & 0x07u;
    } else {
        ++index;
        return 0xFFFD;
    }
    if (index + extra >= text.size()) {
        ++index;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
        const unsigned char continuation = byteAt(i);
        if ((continuation & 0xC0) != 0x80) {
            ++index;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3Fu);
    }
    index += extra + 1;
    return codepoint;
}

// Rebuilds a grid out of tmux's captured pane. The captured text carries SGR sequences
// inline; each line is parsed independently because tmux restarts the attribute state at
// the beginning of a line.
ScreenGrid buildGrid(const std::string& captured, int rows, int cols) {
    ScreenGrid grid(rows, cols);
    const std::vector<std::string> lines = splitOn(captured, '\n');

    for (int row = 0; row < rows && row < static_cast<int>(lines.size()); ++row) {
        const std::string& line = lines[static_cast<std::size_t>(row)];
        SgrState state;
        int column = 0;
        std::size_t index = 0;

        while (index < line.size() && column < cols) {
            const unsigned char lead = static_cast<unsigned char>(line[index]);
            if (lead == 0x1B) {
                // Skip the escape sequence. Only SGR changes what the captured text says
                // about the cells; the rest (cursor movement and friends) is not part of
                // the rendered result.
                std::size_t cursor = index + 1;
                if (cursor < line.size() && line[cursor] == '[') {
                    ++cursor;
                    const std::size_t start = cursor;
                    while (cursor < line.size()) {
                        const unsigned char byte = static_cast<unsigned char>(line[cursor]);
                        if (byte >= 0x40 && byte <= 0x7E) {
                            break;
                        }
                        ++cursor;
                    }
                    if (cursor < line.size()) {
                        if (line[cursor] == 'm') {
                            applySgr(state, line.substr(start, cursor - start));
                        }
                        index = cursor + 1;
                        continue;
                    }
                }
                // A sequence that never terminates: nothing more to read.
                index = line.size();
                continue;
            }

            const std::uint32_t codepoint = decodeUtf8(line, index);
            Cell& cell = grid.at(row, column);
            cell.codepoint = codepoint;
            cell.fg = state.fg;
            cell.bg = state.bg;
            cell.attrs = state.attrs;
            ++column;

            if (openscrt::terminal::isWideCodepoint(codepoint) && column < cols) {
                Cell& trail = grid.at(row, column);
                trail.codepoint = 0;
                trail.fg = state.fg;
                trail.bg = state.bg;
                trail.attrs = (state.attrs & ~cellattr::kWideLead) | cellattr::kWideTrail;
                ++column;
            }
        }
    }
    return grid;
}

}  // namespace

bool tmuxAvailable() {
    bool ok = false;
    const std::string version = runCommand("tmux -V", &ok);
    return ok && !version.empty();
}

std::string renderWithTmux(const std::vector<std::uint8_t>& bytes, int rows, int cols) {
    if (!tmuxAvailable()) {
        return {};
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "openscrt-ref-" + std::to_string(stamp);

    // The pane runs `cat` on a raw tty: the bytes are echoed to the terminal and the
    // terminal interprets them. A raw tty matters because the line discipline would
    // otherwise translate newlines into carriage-return/newline pairs on the way out.
    bool ok = false;
    const std::string create = "tmux -f /dev/null new-session -d -x " + std::to_string(cols) +
                               " -y " + std::to_string(rows) + " -s " + name +
                               " \"sh -c 'stty raw -echo 2>/dev/null; exec cat'\"";
    runCommand(create, &ok);
    if (!ok) {
        return {};
    }

    const std::string hex = hexBytes(bytes);
    if (!hex.empty()) {
        const std::string send =
            "printf '%s' '" + hex + "' | tr ' ' '\\n' | xargs -n 256 tmux send-keys -H -t " + name;
        runCommand(send, &ok);
    }

    // tmux processes the input asynchronously, so the capture has to wait for the pane to
    // settle. Three identical captures in a row means it stopped changing. The first wait
    // is generous on purpose: without it, two empty captures in a row would look like a
    // pane that had settled before tmux had processed anything.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::string previous;
    int stable = 0;
    for (int attempt = 0; attempt < 60 && stable < 3; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string current = runCommand("tmux capture-pane -p -t " + name, &ok);
        if (ok && current == previous) {
            ++stable;
        } else {
            stable = 0;
        }
        previous = current;
    }

    const std::string captured = runCommand("tmux capture-pane -p -e -t " + name, &ok);
    const std::string cursor =
        runCommand("tmux display-message -p -t " + name + " '#{cursor_x},#{cursor_y}'", &ok);
    runCommand("tmux kill-session -t " + name, &ok);

    if (!ok || captured.empty()) {
        return {};
    }

    ScreenGrid grid = buildGrid(captured, rows, cols);

    // The cursor is reported by tmux separately; the snapshot carries it in its header, so
    // it has to land in the grid before the snapshot is taken.
    const std::size_t comma = cursor.find(',');
    if (comma != std::string::npos) {
        const int x = std::atoi(cursor.substr(0, comma).c_str());
        const int y = std::atoi(cursor.substr(comma + 1).c_str());
        if (x >= 0 && y >= 0 && x < cols && y < rows) {
            grid.setCursor(y, x);
        }
    }

    return openscrt::terminal::snapshotScreen(grid);
}

}  // namespace openscrt::test
