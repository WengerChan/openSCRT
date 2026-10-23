#include "terminal/emulator/ScreenSnapshot.h"

#include <cstdio>

namespace openscrt::terminal {
namespace {

// The bits that reach the dump. The wide-glyph bits are deliberately excluded: they say
// how a glyph is laid out, not how it looks, and the text column already carries that.
constexpr std::uint32_t kReportedFlags =
    cellattr::kBold | cellattr::kItalic | cellattr::kUnderline | cellattr::kBlink |
    cellattr::kInverse | cellattr::kInvisible | cellattr::kStrikeThrough;

struct CellSignature {
    ColorRgba fg{0};
    ColorRgba bg{0};
    std::uint32_t flags{0};
    bool default_fg{true};
    bool default_bg{true};

    [[nodiscard]] bool operator==(const CellSignature& other) const noexcept {
        return fg == other.fg && bg == other.bg && flags == other.flags &&
               default_fg == other.default_fg && default_bg == other.default_bg;
    }
};

// A cell that carries no glyph and the default background cannot show a foreground
// colour: there is nothing to draw it with. Terminals differ on whether such a cell
// remembers the colour that was in force when it was written -- tmux does not keep it --
// so the comparison normalises it away on both sides. Without that, vim's status line
// would differ from tmux's over a foreground colour that is not on the screen.
bool foregroundCounts(const Cell& cell) {
    if (cell.isWideTrail()) {
        return true;  // The trailing half of a wide glyph belongs to its lead cell.
    }
    if (cell.codepoint == 0 || cell.codepoint == ' ') {
        return !cell.hasAttribute(cellattr::kDefaultBg);
    }
    return true;
}

// Whether a cell contributes anything to the picture: a glyph, or a background that is
// not the default one. A row ends after its last such cell, which is where tmux's capture
// ends it too.
bool cellIsVisible(const Cell& cell) {
    if (cell.codepoint != 0 && cell.codepoint != ' ') {
        return true;
    }
    return !cell.hasAttribute(cellattr::kDefaultBg);
}

CellSignature signatureOf(const Cell& cell) {
    CellSignature signature;
    signature.fg = cell.fg;
    signature.bg = cell.bg;
    signature.flags = cell.attrs & kReportedFlags;
    signature.default_fg = cell.hasAttribute(cellattr::kDefaultFg);
    signature.default_bg = cell.hasAttribute(cellattr::kDefaultBg);
    if (!foregroundCounts(cell)) {
        signature.fg = 0;
        signature.default_fg = true;
    }
    return signature;
}

bool isDefault(const CellSignature& signature) {
    return signature.default_fg && signature.default_bg && signature.flags == 0;
}

std::string hexColor(ColorRgba rgba) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02x%02x%02x",
                  static_cast<unsigned int>((rgba >> 24) & 0xFFu),
                  static_cast<unsigned int>((rgba >> 16) & 0xFFu),
                  static_cast<unsigned int>((rgba >> 8) & 0xFFu));
    return std::string(buffer);
}

std::string colorToken(const CellSignature& signature, bool foreground) {
    if (foreground) {
        return signature.default_fg ? std::string("def") : hexColor(signature.fg);
    }
    return signature.default_bg ? std::string("def") : hexColor(signature.bg);
}

std::string flagToken(const CellSignature& signature) {
    std::string flags;
    if ((signature.flags & cellattr::kBold) != 0) {
        flags.push_back('b');
    }
    if ((signature.flags & cellattr::kUnderline) != 0) {
        flags.push_back('u');
    }
    if ((signature.flags & cellattr::kItalic) != 0) {
        flags.push_back('i');
    }
    if ((signature.flags & cellattr::kBlink) != 0) {
        flags.push_back('k');
    }
    if ((signature.flags & cellattr::kInverse) != 0) {
        flags.push_back('r');
    }
    if ((signature.flags & cellattr::kInvisible) != 0) {
        flags.push_back('n');
    }
    if ((signature.flags & cellattr::kStrikeThrough) != 0) {
        flags.push_back('s');
    }
    return flags.empty() ? std::string("-") : flags;
}

// The last column of the row that still contributes something, or -1 for an empty row.
int lastVisibleColumn(const ScreenGrid& grid, int row) {
    const Cell* cells = grid.rowData(row);
    if (cells == nullptr) {
        return -1;
    }
    int last = -1;
    for (int column = 0; column < grid.cols(); ++column) {
        if (cellIsVisible(cells[column])) {
            last = column;
        }
    }
    return last;
}

// Writes the attribute runs of one row: only the runs that differ from the default, each
// as <column>:<length>:<fg>:<bg>:<flags>.
void appendRuns(std::string& out, const ScreenGrid& grid, int row, int last) {
    const Cell* cells = grid.rowData(row);
    if (cells == nullptr || last < 0) {
        return;
    }

    int column = 0;
    bool first = true;
    while (column <= last) {
        const CellSignature signature = signatureOf(cells[column]);
        int length = 1;
        while (column + length <= last && signatureOf(cells[column + length]) == signature) {
            ++length;
        }

        if (!isDefault(signature)) {
            if (!first) {
                out.push_back(' ');
            }
            first = false;
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%d:%d:%s:%s:%s", column, length,
                          colorToken(signature, true).c_str(), colorToken(signature, false).c_str(),
                          flagToken(signature).c_str());
            out += buffer;
        }
        column += length;
    }
}

}  // namespace

std::string snapshotScreen(const ScreenGrid& grid) {
    std::string out;
    out.reserve(static_cast<std::size_t>(grid.rows()) * 96);

    char header[64];
    std::snprintf(header, sizeof(header), "grid %dx%d cursor=%d,%d\n", grid.rows(), grid.cols(),
                  grid.cursorRow(), grid.cursorColumn());
    out += header;

    for (int row = 0; row < grid.rows(); ++row) {
        const int last = lastVisibleColumn(grid, row);
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "r%02d text=", row);
        out += prefix;
        if (last >= 0) {
            const Cell* cells = grid.rowData(row);
            out += ScreenGrid::toUtf8(cells, last + 1);
        }
        out += " attrs=";
        appendRuns(out, grid, row, last);
        out.push_back('\n');
    }
    return out;
}

}  // namespace openscrt::terminal
