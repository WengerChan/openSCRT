// Terminal cell layout.
//
// The roadmap fixes Cell at 16 bytes so a 128,000-line scrollback stays affordable
// (128k x 200 cols x 16 B = ~400 MB worst case, and typical 80x24 sessions are far
// smaller). Adding a field later breaks that budget, so wide-cell state lives in the
// attribute bit-field and colour is stored as a packed RGBA word.

#ifndef OPENCRT_TERMINAL_BUFFER_CELL_H_
#define OPENCRT_TERMINAL_BUFFER_CELL_H_

#include <cstdint>

namespace openscrt::terminal {

// Packed 0xRRGGBBAA. Palette lookups are resolved to RGBA at write time so the renderer
// never needs a second lookup table on the hot path.
using ColorRgba = std::uint32_t;

inline constexpr ColorRgba makeRgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                    std::uint8_t a = 0xFF) noexcept {
    return (static_cast<ColorRgba>(r) << 24) | (static_cast<ColorRgba>(g) << 16) |
           (static_cast<ColorRgba>(b) << 8) | static_cast<ColorRgba>(a);
}

namespace cellattr {
inline constexpr std::uint32_t kBold = 1u << 0;
inline constexpr std::uint32_t kItalic = 1u << 1;
inline constexpr std::uint32_t kUnderline = 1u << 2;
inline constexpr std::uint32_t kBlink = 1u << 3;
inline constexpr std::uint32_t kInverse = 1u << 4;
inline constexpr std::uint32_t kInvisible = 1u << 5;
inline constexpr std::uint32_t kStrikeThrough = 1u << 6;
// Wide (CJK) glyph occupies two columns: the lead cell carries the codepoint and the
// trailing cell is a blank marked kWideTrail. Keeps the grid 2-D addressable with no
// per-row remapping, which is what cursor addressing (CUP) requires.
inline constexpr std::uint32_t kWideLead = 1u << 7;
inline constexpr std::uint32_t kWideTrail = 1u << 8;
inline constexpr std::uint32_t kDefaultFg = 1u << 9;
inline constexpr std::uint32_t kDefaultBg = 1u << 10;
inline constexpr std::uint32_t kDirty = 1u << 20;
}  // namespace cellattr

struct alignas(16) Cell {
    std::uint32_t codepoint{0};  // Unicode scalar value; 0 means "blank"
    ColorRgba fg{0};
    ColorRgba bg{0};
    std::uint32_t attrs{cellattr::kDefaultFg | cellattr::kDefaultBg};

    [[nodiscard]] bool isBlank() const noexcept { return codepoint == 0; }
    [[nodiscard]] bool isWideLead() const noexcept { return (attrs & cellattr::kWideLead) != 0; }
    [[nodiscard]] bool isWideTrail() const noexcept { return (attrs & cellattr::kWideTrail) != 0; }
    [[nodiscard]] bool isDirty() const noexcept { return (attrs & cellattr::kDirty) != 0; }
    void markDirty() noexcept { attrs |= cellattr::kDirty; }
    void clearDirty() noexcept { attrs &= ~cellattr::kDirty; }

    // True when the cell draws nothing, which lets the renderer skip both glyph lookup and
    // background fill.
    [[nodiscard]] bool isInvisible() const noexcept {
        return isBlank() || (attrs & cellattr::kInvisible) != 0;
    }

    // Attributes that carry over when a cell is erased; colour and glyph do not.
    void setBlank() noexcept {
        codepoint = 0;
        attrs = cellattr::kDefaultFg | cellattr::kDefaultBg;
    }

    friend bool operator==(const Cell& a, const Cell& b) noexcept {
        return a.codepoint == b.codepoint && a.fg == b.fg && a.bg == b.bg &&
               (a.attrs & ~cellattr::kDirty) == (b.attrs & ~cellattr::kDirty);
    }
};

static_assert(sizeof(Cell) == 16,
              "Cell must stay 16 bytes; widening it breaks the scrollback budget");
static_assert(alignof(Cell) == 16, "Cell alignment keeps row indexing a shift");

// Style applied by the parser before writing a glyph. Kept separate from Cell so SGR state
// changes do not require touching the grid.
struct CellStyle {
    ColorRgba fg{0};
    ColorRgba bg{0};
    std::uint32_t attrs{cellattr::kDefaultFg | cellattr::kDefaultBg};

    void reset() noexcept {
        fg = 0;
        bg = 0;
        attrs = cellattr::kDefaultFg | cellattr::kDefaultBg;
    }

    void setAttribute(std::uint32_t bit, bool on) noexcept {
        if (on) {
            attrs |= bit;
        } else {
            attrs &= ~bit;
        }
    }

    [[nodiscard]] bool hasAttribute(std::uint32_t bit) const noexcept { return (attrs & bit) != 0; }

    friend bool operator==(const CellStyle& a, const CellStyle& b) noexcept {
        return a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs;
    }
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_BUFFER_CELL_H_