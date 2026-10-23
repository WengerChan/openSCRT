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

CellSignature signatureOf(const Cell& cell) {
    CellSignature signature;
    signature.fg = cell.fg;
    signature.bg = cell.bg;
    signature.flags = cell.attrs & kReportedFlags;
    signature.default_fg = cell.hasAttribute(cellattr::kDefaultFg);
    signature.default_bg = cell.hasAttribute(cellattr::kDefaultBg);
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

// Writes the attribute runs of one row: only the runs that differ from the default, each
// as <column>:<length>:<fg>:<bg>:<flags>.
void appendRuns(std::string& out, const ScreenGrid& grid, int row) {
    const Cell* cells = grid.rowData(row);
    if (cells == nullptr) {
        return;
    }

    int column = 0;
    bool first = true;
    while (column < grid.cols()) {
        const CellSignature signature = signatureOf(cells[column]);
        int length = 1;
        while (column + length < grid.cols() && signatureOf(cells[column + length]) == signature) {
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
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "r%02d text=", row);
        out += prefix;
        out += grid.rowText(row);
        out += " attrs=";
        appendRuns(out, grid, row);
        out.push_back('\n');
    }
    return out;
}

}  // namespace openscrt::terminal
