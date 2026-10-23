#include "ui/renderer/GlyphAtlas.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QRawFont>
#include <algorithm>
#include <utility>

namespace openscrt::ui {
namespace {

// The atlas grows in whole rows so a glyph is never split across a reallocation. One row is
// deliberately left empty: `reserveRow` treats a full atlas as "stop rasterising", and the
// renderer then draws new glyphs directly, which is slower but never wrong.
constexpr int kPadding = 1;
// Beyond this many distinct (glyph, colour) pairs the cache stops earning its memory and the
// renderer falls back to direct drawing. A screen realistically uses a few dozen colours.
constexpr std::size_t kMaxTintedGlyphs = 8192;
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
    tinted_.clear();
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
    const int wanted_height = metrics_.cell_height * (next_row_ + 1);
    if (atlas_.height() < wanted_height) {
        // Keep one spare row so there is always somewhere to put the next glyph. QImage has
        // no region fill, so grow by allocating a fresh transparent image and blitting the
        // old contents onto it.
        QImage grown(kAtlasWidth, wanted_height, QImage::Format_ARGB32_Premultiplied);
        grown.fill(Qt::transparent);
        {
            QPainter painter(&grown);
            painter.drawImage(0, 0, atlas_);
        }
        atlas_ = std::move(grown);
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
    const QImage glyph = rasteriseGlyph(codepoint, style);
    if (glyph.isNull()) {
        return QRect();
    }
    const QRect target = nextFreeSlot();
    if (target.isNull()) {
        return QRect();  // Atlas full: caller falls back to direct drawing.
    }
    {
        QPainter painter(&atlas_);
        painter.drawImage(target.topLeft(), glyph);
    }
    cache_.emplace(key, target);
    return target;
}

QRect GlyphAtlas::ensureTintedGlyph(std::uint32_t codepoint, GlyphStyle style, std::uint32_t rgba) {
    const TintedGlyphKey key{codepoint, style, rgba};
    const auto found = tinted_.find(key);
    if (found != tinted_.end()) {
        return found->second;
    }
    if (tinted_.size() >= kMaxTintedGlyphs) {
        return QRect();
    }

    // Render the outline, then recolour it by copying alpha into every colour channel. Doing
    // this once per (glyph, colour) is what turns per-frame text shaping into a blit.
    const QImage outline(rasteriseGlyph(codepoint, style));
    if (outline.isNull()) {
        return QRect();
    }
    const int red = static_cast<int>((rgba >> 24) & 0xFF);
    const int green = static_cast<int>((rgba >> 16) & 0xFF);
    const int blue = static_cast<int>((rgba >> 8) & 0xFF);
    const int alpha = static_cast<int>(rgba & 0xFF);

    for (int y = 0; y < outline.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(outline.scanLine(y));
        for (int x = 0; x < outline.width(); ++x) {
            const int coverage = qAlpha(line[x]);
            line[x] = qRgba(red, green, blue, coverage * alpha / 255);
        }
    }

    const QRect target = nextFreeSlot();
    if (target.isNull()) {
        return QRect();
    }
    {
        QPainter painter(&atlas_);
        painter.drawImage(target.topLeft(), outline);
    }
    tinted_.emplace(key, target);
    return target;
}

QRect GlyphAtlas::nextFreeSlot() {
    if (next_column_ >= columns_) {
        next_column_ = 0;
        if (reserveRow() < 0) {
            return QRect();
        }
    }
    const int column = next_column_++;
    return QRect(column * (metrics_.cell_width + kPadding), next_row_ * metrics_.cell_height,
                 metrics_.cell_width, metrics_.cell_height);
}

QImage GlyphAtlas::rasteriseGlyph(std::uint32_t codepoint, GlyphStyle style) {
    if (codepoint == 0 || codepoint > 0x10FFFF) {
        return QImage();
    }
    QFont font;
    font.setFixedPitch(true);
    font.setBold(style == GlyphStyle::kBold || style == GlyphStyle::kBoldItalic);
    font.setItalic(style == GlyphStyle::kItalic || style == GlyphStyle::kBoldItalic);

    QImage glyph(metrics_.cell_width, metrics_.cell_height, QImage::Format_ARGB32_Premultiplied);
    glyph.fill(Qt::transparent);
    QPainter painter(&glyph);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::white);
    const char32_t scalar = static_cast<char32_t>(codepoint);
    painter.drawText(QPointF(0, metrics_.baseline), QString::fromUcs4(&scalar, 1));
    return glyph;
}

}  // namespace openscrt::ui