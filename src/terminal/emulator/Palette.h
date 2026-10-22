// xterm colour table (P1-T5).
//
// Stored as packed RGBA words rather than palette indices in the Cell, because the renderer
// must not run a second lookup on the hot path. The SGR handler resolves an index to a
// colour exactly once, at the moment the attribute is applied.

#ifndef OPENCRT_TERMINAL_EMULATOR_PALETTE_H_
#define OPENCRT_TERMINAL_EMULATOR_PALETTE_H_

#include <cstdint>

#include "terminal/buffer/Cell.h"

namespace openscrt::terminal {

inline constexpr int kPaletteSize = 256;
inline constexpr int kIndexedColorCount = 256;

inline ColorRgba defaultForeground() noexcept {
    return makeRgba(0xD0, 0xD0, 0xD0);
}
inline ColorRgba defaultBackground() noexcept {
    return makeRgba(0x0D, 0x0D, 0x0D);
}

// The 16 base entries follow the usual convention: 0-7 normal, 8-15 bright.
constexpr ColorRgba kBasePalette[16] = {
    makeRgba(0x00, 0x00, 0x00),  // black
    makeRgba(0xCD, 0x31, 0x31),  // red
    makeRgba(0x0D, 0xBC, 0x79),  // green
    makeRgba(0xE5, 0xE5, 0x10),  // yellow
    makeRgba(0x24, 0x72, 0xC8),  // blue
    makeRgba(0xBC, 0x3F, 0xBC),  // magenta
    makeRgba(0x11, 0xA8, 0xCD),  // cyan
    makeRgba(0xE5, 0xE5, 0xE5),  // white
    makeRgba(0x66, 0x66, 0x66),  // bright black
    makeRgba(0xF1, 0x4C, 0x4C),  // bright red
    makeRgba(0x23, 0xD1, 0x8B),  // bright green
    makeRgba(0xF5, 0xF5, 0x43),  // bright yellow
    makeRgba(0x3B, 0x8E, 0xEA),  // bright blue
    makeRgba(0xD6, 0x70, 0xD6),  // bright magenta
    makeRgba(0x29, 0xB8, 0xDB),  // bright cyan
    makeRgba(0xFF, 0xFF, 0xFF),  // bright white
};

// Resolves an SGR colour index to RGBA. Index 16-231 is the 6x6x6 cube, 232-255 the
// 24-step grey ramp; both formulas are fixed by xterm and are what terminal output assumes.
constexpr ColorRgba paletteColor(int index) noexcept {
    if (index < 0 || index >= kPaletteSize) {
        return makeRgba(0xFF, 0x00, 0xFF);  // Loud magenta: an out-of-range index is a bug.
    }
    if (index < 16) {
        return kBasePalette[index];
    }
    if (index < 232) {
        const int offset = index - 16;
        const int r = offset / 36;
        const int g = (offset / 6) % 6;
        const int b = offset % 6;
        // Level 0 is 0; every other level steps by 40 starting at 55.
        const auto level = [](int v) -> std::uint8_t {
            return static_cast<std::uint8_t>(v == 0 ? 0 : 55 + 40 * v);
        };
        return makeRgba(level(r), level(g), level(b));
    }
    const int grey = 8 + 10 * (index - 232);
    const auto value = static_cast<std::uint8_t>(grey);
    return makeRgba(value, value, value);
}

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_EMULATOR_PALETTE_H_