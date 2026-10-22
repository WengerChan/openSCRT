#include "ui/renderer/GlyphAtlas.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QRawFont>
#include <algorithm>

namespace openscrt::ui {
namespace {

// The atlas grows in whole rows so a glyph is never split across a reallocation. One row is
// deliberately left empty: `reserveRow` treats a full atlas as "stop rasterising", and the
// renderer then draws new glyphs directly, which is slower but never wrong.
constexpr int kPadding = 1;
constexpr int kAtlasWidth = 1024;
constexpr int kMaxRowsBeforeGivingUp = 4096;

}  // namespace

GlyphAtlas::GlyphAtlas() {
    reset(QFont());
}

GlyphAtlas::GlyphAtlas(const QFont& font) {
    reset(font);
}

GlyphAtlas::~GlyphAtlas() = default;

void GlyphAtlas::reset(const QFont& font) {
    cache_.clear();
    used_slots_ = 0;
    next_row_ = 0;
    next_column_ = 0;

    const QFontMetricsF metrics(font);
    metrics_.cell_width = static_cast<int>(std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    metrics_.cell_height = static_cast<int>(std::ceil(metrics.height()));
    metrics_.baseline = static_cast<int>(std::ceil(metrics.ascent()));
    if (metrics_.cell_width <= 0) {
        metrics_.cell_width = 8;
    }
    if (metrics_.cell_height <= 0) {
        metrics_.cell_height = 16;
    }

    columns_ = std::max(1, (kAtlasWidth - kPadding) / (metrics_.cell_width + kPadding));
    atlas_ = QImage(kAtlasWidth, metrics_.cell_height, QImage::Format_ARGB32_Premultiplied);
    atlas_.fill(Qt::transparent);
}

int GlyphAtlas::reserveRow() {
    if (next_row_ >= kMaxRowsBeforeGivingUp) {
        return -1;
    }
    const int row = next_row_++;
    used_slots_ += columns_;
    if (atlas_.height() < metrics_.cell_height * (next_row_ + 1)) {
        // Keep one spare row so there is always somewhere to put the next glyph.
        atlas_ = atlas_.copy(0, 0, kAtlasWidth, metrics_.cell_height * (next_row_ + 1));
        atlas_.fill(next_row_ * metrics_.cell_height, 0, kAtlasWidth, metrics_.cell_height,
                    Qt::transparent);
    }
    return row;
}

QRect GlyphAtlas::glyphRect(const GlyphKey& key) const {
    const auto found = cache_.find(key);
    return found == cache_.end() ? QRect() : found->second;
}

QRect GlyphAtlas::ensureGlyph(std::uint32_t codepoint, GlyphStyle style) {
    const GlyphKey key{codepoint, style};
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second;
    }
    if (codepoint == 0 || codepoint > 0x10FFFF) {
        return QRect();
    }

    if (next_column_ >= columns_) {
        next_column_ = 0;
        if (reserveRow() < 0) {
            return QRect();  // Atlas full: caller falls back to direct drawing.
        }
    }

    const int column = next_column_++;
    const int x = column * (metrics_.cell_width + kPadding);
    const int y = next_row_ * metrics_.cell_height;
    const QRect target(x, y, metrics_.cell_width, metrics_.cell_height);

    // Rasterise into a scratch image first so a failure cannot leave a half-drawn cell in the
    // atlas that later blits would treat as valid.
    QImage glyph(metrics_.cell_width, metrics_.cell_height, QImage::Format_ARGB32_Premultiplied);
    glyph.fill(Qt::transparent);
    {
        QPainter painter(&glyph);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::white);
        const QString text = QString::fromUcs4(&codepoint, 1);
        painter.drawText(QPointF(0, metrics_.baseline), text);
    }
    QPainter painter(&atlas_);
    painter.drawImage(target.topLeft(), glyph);

    cache_.emplace(key, target);
    return target;
}

}  // namespace openscrt::ui