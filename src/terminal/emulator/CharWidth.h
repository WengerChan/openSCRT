// Character width classification shared by the emulator and the baseline tooling.
//
// The emulator marks a cell as double-width when the glyph it just printed needs two
// columns; the baseline tooling needs the same answer to line up a reference terminal's
// columns with ours. Two copies of the table would eventually disagree, and the symptom
// would be a cursor drift that looks like an emulator bug. One definition, two callers.
//
// The table is range-based rather than a full Unicode width table. It covers the blocks
// that actually appear in the CJK output the roadmap targets. Wrong-width classification
// shows up as a one-column cursor drift, never as data loss, so the heuristic is the right
// trade for Phase 1.

#ifndef OPENCRT_TERMINAL_EMULATOR_CHAR_WIDTH_H_
#define OPENCRT_TERMINAL_EMULATOR_CHAR_WIDTH_H_

#include <cstdint>

namespace openscrt::terminal {

inline constexpr bool isWideCodepoint(std::uint32_t cp) noexcept {
    return (cp >= 0x1100 && cp <= 0x115F) ||  // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x303E) ||  // CJK radicals, Kangxi
           (cp >= 0x3041 && cp <= 0x33FF) ||  // Hiragana .. CJK Compatibility
           (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Extension A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs
           (cp >= 0xA000 && cp <= 0xA4CF) ||  // Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK Compatibility Ideographs
           (cp >= 0xFF00 && cp <= 0xFF60) ||  // Fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6);    // Fullwidth signs
}

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_EMULATOR_CHAR_WIDTH_H_
