#include "ui/renderer/TerminalRenderer.h"

#include <QPainter>
#include <QRectF>
#include <algorithm>

#include "terminal/emulator/Palette.h"

namespace openscrt::ui {

TerminalRenderer::TerminalRenderer(terminal::ScreenGrid& grid,
                                   terminal::RingScrollbackBuffer* scrollback)
    : grid_(&grid), scrollback_(scrollback) {
    theme_.background = QColor(terminal::defaultBackground());
    theme_.foreground = QColor(terminal::defaultForeground());
    theme_.cursor = theme_.foreground;
    theme_.selection = QColor(60, 100, 180);
    resize(grid.rows(), grid.cols());
}

TerminalRenderer::~TerminalRenderer() = default;

void TerminalRenderer::setFont(const QFont& font) {
    atlas_.reset(font);
    resize(grid_->rows(), grid_->cols());
    repaintAll();
}

void TerminalRenderer::setTheme(const Theme& theme) {
    theme_ = theme;
    repaintAll();
}

void TerminalRenderer::resize(int rows, int columns) {
    rows = std::max(1, rows);
    columns = std::max(1, columns);
    grid_->resize(rows, columns);
    backbuffer_ = QImage(columns * atlas_.metrics().cell_width, rows * atlas_.metrics().cell_height,
                         QImage::Format_ARGB32_Premultiplied);
    backbuffer_.fill(theme_.background);
    repaintAll();
}

GlyphStyle TerminalRenderer::styleFor(const terminal::Cell& cell) const noexcept {
    const bool bold = cell.hasAttribute(terminal::cellattr::kBold);
    const bool italic = cell.hasAttribute(terminal::cellattr::kItalic);
    if (bold && italic) {
        return GlyphStyle::kBoldItalic;
    }
    return bold ? GlyphStyle::kBold : (italic ? GlyphStyle::kItalic : GlyphStyle::kRegular);
}

QColor TerminalRenderer::resolve(const terminal::Cell& cell, bool foreground) const {
    const std::uint32_t packed = foreground ? cell.fg : cell.bg;
    const std::uint32_t is_default =
        foreground ? terminal::cellattr::kDefaultFg : terminal::cellattr::kDefaultBg;
    if (cell.hasAttribute(is_default)) {
        return foreground ? theme_.foreground : theme_.background;
    }
    return QColor(static_cast<int>((packed >> 24) & 0xFF), static_cast<int>((packed >> 16) & 0xFF),
                  static_cast<int>((packed >> 8) & 0xFF), static_cast<int>(packed & 0xFF));
}

void TerminalRenderer::paintCell(QPainter& painter, int row, int column) {
    const terminal::Cell* cell = grid_->rowData(row);
    if (cell == nullptr) {
        return;
    }
    const terminal::Cell& value = cell[column];

    const int width = atlas_.metrics().cell_width;
    const int height = atlas_.metrics().cell_height;
    const QRect target(column * width, row * height, width, height);

    painter.fillRect(target, resolve(value, false));

    if (value.isBlank() || value.isWideTrail()) {
        return;
    }
    if (value.hasAttribute(terminal::cellattr::kInvisible)) {
        return;
    }

    const bool inverse = value.hasAttribute(terminal::cellattr::kInverse);
    QColor foreground = inverse ? resolve(value, false) : resolve(value, true);
    if (value.hasAttribute(terminal::cellattr::kBold)) {
        foreground = foreground.lighter(130);
    }

    // Blit from the tinted atlas. Shaping the text every frame cost far more than the blit,
    // which is what capped the dirty-rectangle benefit at ~1.3x (P1-A3b). Falls back to
    // direct drawing only when the tint cache or the atlas is full.
    const std::uint32_t rgba = (static_cast<std::uint32_t>(foreground.red()) << 24) |
                               (static_cast<std::uint32_t>(foreground.green()) << 16) |
                               (static_cast<std::uint32_t>(foreground.blue()) << 8) |
                               static_cast<std::uint32_t>(foreground.alpha());
    const QRect glyph = atlas_.ensureTintedGlyph(value.codepoint, styleFor(value), rgba);
    if (!glyph.isNull()) {
        // Source+target rect overloads take QRectF, not QRect.
        painter.drawImage(QRectF(target), atlas_.atlasImage(), QRectF(glyph));
        return;
    }
    painter.setPen(foreground);
    const char32_t scalar = static_cast<char32_t>(value.codepoint);
    painter.drawText(QPointF(target.x(), target.y() + atlas_.metrics().baseline),
                     QString::fromUcs4(&scalar, 1));
}

void TerminalRenderer::paintBand(int first_row, int last_row) {
    const int width = atlas_.metrics().cell_width;
    const int height = atlas_.metrics().cell_height;
    QPainter painter(&backbuffer_);
    for (int row = first_row; row <= last_row; ++row) {
        // Background first, in one fill per row, so a blank tail costs one rectangle rather
        // than one per cell.
        painter.fillRect(0, row * height, grid_->cols() * width, height, theme_.background);
        const terminal::Cell* cells = grid_->rowData(row);
        if (cells == nullptr) {
            continue;
        }
        for (int column = 0; column < grid_->cols(); ++column) {
            const terminal::Cell& cell = cells[column];
            if (cell.isBlank() && cell.hasAttribute(terminal::cellattr::kDefaultBg)) {
                continue;  // Already painted by the row fill.
            }
            paintCell(painter, row, column);
        }
    }
}

int TerminalRenderer::repaintDamage() {
    const auto start = std::chrono::steady_clock::now();
    const auto bands = terminal::DamageTracker::collect(*grid_);
    if (bands.empty()) {
        return 0;
    }
    for (const auto& band : bands) {
        paintBand(band.first, band.last);
    }
    const int rows = grid_->rows();
    int painted = 0;
    for (const auto& band : bands) {
        painted += band.rowCount();
    }
    ++stats_.repaint_frames;
    stats_.rows_painted += painted;
    stats_.rows_skipped += rows - painted;
    last_paint_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return painted;
}

int TerminalRenderer::repaintAll() {
    const auto start = std::chrono::steady_clock::now();
    paintBand(0, grid_->rows() - 1);
    ++stats_.repaint_frames;
    stats_.rows_painted += grid_->rows();
    last_paint_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return grid_->rows();
}

}  // namespace openscrt::ui