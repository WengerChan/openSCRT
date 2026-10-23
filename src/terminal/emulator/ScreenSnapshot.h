// A canonical, diffable text rendering of a screen.
//
// P1-A2 asks for a per-case baseline comparison. A baseline *image* is not available yet --
// the glyph atlas and the dirty-rectangle renderer are still open -- but what an image
// would prove, where every glyph landed and what attributes it carries, can be written
// down as text.
//
// The format:
//
//     grid 24x80 cursor=5,12
//     r00 text=hello attrs=0:5:d,d,b
//     r01 text=world attrs=
//
// "text" is the row's glyphs with trailing blanks removed. "attrs" lists the runs whose
// colours or flags differ from the default, as <column>:<length>:<fg>:<bg>:<flags>.
// A colour is six hex digits, or "def" for the terminal default; the flags are letters
// (b bold, u underline, i italic, k blink, r reverse, n invisible, s strike-through) or
// "-" for none.
//
// Two properties make this usable as a baseline. It is deterministic: the same grid always
// produces the same bytes. And it is reviewable: a human reading a diff can see which cell
// moved and which attribute changed, which is what "a difference needs a human to confirm
// it" actually requires. A hash alone would say "different" without saying how.

#ifndef OPENCRT_TERMINAL_EMULATOR_SCREEN_SNAPSHOT_H_
#define OPENCRT_TERMINAL_EMULATOR_SCREEN_SNAPSHOT_H_

#include <string>

#include "terminal/buffer/ScreenGrid.h"

namespace openscrt::terminal {

// Renders the grid in the canonical format described above.
[[nodiscard]] std::string snapshotScreen(const ScreenGrid& grid);

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_EMULATOR_SCREEN_SNAPSHOT_H_
