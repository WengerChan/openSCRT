// Dirty-rectangle bookkeeping (P1-T6, the logic half).
//
// The roadmap's rendering budget rests on repainting only what changed. That decision is pure
// arithmetic over the grid's dirty bits, so it lives here in openscrt_terminal where it can be
// tested without a window server. Glyph rasterisation and the actual QPainter calls stay in
// openscrt_ui; splitting them is what keeps this testable.
//
// The coarse first pass is per-row. Coalescing runs of adjacent rows into bands is the whole
// point: `seq 1 1000000` dirties every row, and one full-screen band costs the same as one
// band would, whereas 24 separate update() calls would not.

#ifndef OPENCRT_TERMINAL_RENDERER_DAMAGE_TRACKER_H_
#define OPENCRT_TERMINAL_RENDERER_DAMAGE_TRACKER_H_

#include <cstddef>
#include <vector>

#include "terminal/buffer/ScreenGrid.h"

namespace openscrt::terminal {

// A run of consecutive rows that all need repainting, inclusive.
struct RowBand {
    int first{0};
    int last{0};

    [[nodiscard]] int rowCount() const noexcept { return last - first + 1; }
    friend bool operator==(const RowBand& a, const RowBand& b) noexcept {
        return a.first == b.first && a.last == b.last;
    }
};

struct PixelRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    friend bool operator==(const PixelRect& a, const PixelRect& b) noexcept {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

struct CellMetrics {
    int width{8};
    int height{16};
};

class DamageTracker {
 public:
    // Reads the grid's dirty flags, coalesces adjacent rows, and clears the flags so the
    // next call reports only newly damaged content. Clearing here rather than in the renderer
    // keeps "what still needs painting" a single, unambiguous piece of state.
    [[nodiscard]] static std::vector<RowBand> collect(ScreenGrid& grid);

    // Repaints everything. Used for expose, resize, font change and theme change, none of
    // which can be expressed as row damage.
    [[nodiscard]] static std::vector<RowBand> collectAll(ScreenGrid& grid);

    // Pixel rectangle covering `bands`, clipped to the grid. Returns width/height 0 when there
    // is nothing to paint, so callers can skip the update() call outright.
    [[nodiscard]] static PixelRect rectFor(const std::vector<RowBand>& bands, int columns,
                                           const CellMetrics& metrics);

    // Pixel rectangle for the whole grid.
    [[nodiscard]] static PixelRect fullRect(int rows, int columns, const CellMetrics& metrics);
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_RENDERER_DAMAGE_TRACKER_H_