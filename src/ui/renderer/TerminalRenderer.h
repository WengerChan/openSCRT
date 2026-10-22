// Terminal renderer (P1-T6/P1-T7, the drawing half).
//
// Paints the screen grid into a backing image using the atlas and the damage bands from
// openscrt_terminal. Keeping this object free of widgets is what lets the P1-A3 frame-rate
// budget be measured headlessly in CI: the same code the widget blits from runs in a test.

#ifndef OPENCRT_UI_RENDERER_TERMINAL_RENDERER_H_
#define OPENCRT_UI_RENDERER_TERMINAL_RENDERER_H_

#include <QColor>
#include <QImage>
#include <chrono>
#include <memory>

#include "terminal/buffer/RingScrollbackBuffer.h"
#include "terminal/buffer/ScreenGrid.h"
#include "terminal/renderer/DamageTracker.h"
#include "ui/renderer/GlyphAtlas.h"

namespace openscrt::ui {

struct Theme {
    QColor background;
    QColor foreground;
    QColor cursor;
    QColor selection;
};

class TerminalRenderer {
 public:
    TerminalRenderer(ScreenGrid& grid, RingScrollbackBuffer* scrollback = nullptr);
    ~TerminalRenderer();

    TerminalRenderer(const TerminalRenderer&) = delete;
    TerminalRenderer& operator=(const TerminalRenderer&) = delete;

    void setFont(const QFont& font);
    void setTheme(const Theme& theme);
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

    // Matches the grid to rows x columns and resizes the backing image.
    void resize(int rows, int columns);

    [[nodiscard]] const QImage& backbuffer() const noexcept { return backbuffer_; }
    [[nodiscard]] const CellMetrics& cellMetrics() const noexcept {
        return {atlas_.metrics().cell_width, atlas_.metrics().cell_height};
    }

    // Repaints only the rows DamageTracker reports. Returns how many rows were painted.
    // Rows that are clean are not touched at all -- that is the whole performance claim.
    int repaintDamage();

    // Unconditional full repaint: expose, resize, font or theme change.
    int repaintAll();

    struct Stats {
        int frames{0};
        int rows_painted{0};
        int rows_skipped{0};
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = Stats{}; }

    [[nodiscard]] double lastPaintMilliseconds() const noexcept { return last_paint_ms_; }

 private:
    void paintBand(int first_row, int last_row);
    void paintCell(int row, int column);
    [[nodiscard]] GlyphStyle styleFor(const Cell& cell) const noexcept;
    [[nodiscard]] QColor resolve(const Cell& cell, bool foreground) const;

    ScreenGrid* grid_;
    RingScrollbackBuffer* scrollback_;
    GlyphAtlas atlas_;
    Theme theme_;
    QImage backbuffer_;
    Stats stats_;
    double last_paint_ms_{0.0};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_RENDERER_TERMINAL_RENDERER_H_