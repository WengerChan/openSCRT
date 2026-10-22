#include "terminal/renderer/DamageTracker.h"

#include <algorithm>
#include <stdexcept>

namespace openscrt::terminal {

std::vector<RowBand> DamageTracker::collect(ScreenGrid& grid) {
    std::vector<RowBand> bands;
    const int rows = grid.rows();
    int open_first = -1;

    for (int row = 0; row < rows; ++row) {
        if (grid.rowIsDirty(row)) {
            if (open_first < 0) {
                open_first = row;
            }
        } else if (open_first >= 0) {
            bands.push_back(RowBand{open_first, row - 1});
            open_first = -1;
        }
    }
    if (open_first >= 0) {
        bands.push_back(RowBand{open_first, rows - 1});
    }

    // Consume the flags. A row that is damaged again before the next paint gets re-marked by
    // whatever wrote to it, so nothing is lost by clearing now.
    grid.clearDirtyFlags();
    return bands;
}

std::vector<RowBand> DamageTracker::collectAll(ScreenGrid& grid) {
    for (int row = 0; row < grid.rows(); ++row) {
        grid.markRowDirty(row);
    }
    return collect(grid);
}

PixelRect DamageTracker::rectFor(const std::vector<RowBand>& bands, int columns,
                                 const CellMetrics& metrics) {
    if (bands.empty() || columns <= 0 || metrics.width <= 0 || metrics.height <= 0) {
        return PixelRect{0, 0, 0, 0};
    }
    int first = bands.front().first;
    int last = bands.front().last;
    for (const RowBand& band : bands) {
        first = std::min(first, band.first);
        last = std::max(last, band.last);
    }
    if (last < first) {
        return PixelRect{0, 0, 0, 0};
    }
    // Columns: assume the worst and repaint the full width. Narrowing this to a per-row
    // column range is the natural next optimisation and is the reason DamageTracker returns
    // bands rather than a single rectangle -- a caller that tracks horizontal extent can
    // build tighter rects from the same data.
    return PixelRect{0, first * metrics.height, columns * metrics.width,
                     (last - first + 1) * metrics.height};
}

PixelRect DamageTracker::fullRect(int rows, int columns, const CellMetrics& metrics) {
    if (rows <= 0 || columns <= 0 || metrics.width <= 0 || metrics.height <= 0) {
        return PixelRect{0, 0, 0, 0};
    }
    return PixelRect{0, 0, columns * metrics.width, rows * metrics.height};
}

}  // namespace openscrt::terminal