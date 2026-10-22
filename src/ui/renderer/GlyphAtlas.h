// Glyph atlas (P1-T6).
//
// Rasterises glyphs once into a texture atlas and hands the renderer a sub-rectangle, so the
// per-frame cost is a blit instead of a shaped-text draw. The roadmap calls for ASCII plus CJK
// pre-rendered; anything not yet in the atlas falls back to direct drawing, so a session that
// suddenly switches language degrades in framerate rather than breaking.

#ifndef OPENCRT_UI_RENDERER_GLYPH_ATLAS_H_
#define OPENCRT_UI_RENDERER_GLYPH_ATLAS_H_

#include <QFont>
#include <QImage>
#include <QRect>
#include <cstdint>
#include <memory>

#include "terminal/buffer/Cell.h"

namespace openscrt::ui {

// Style variants get separate slots: bold and italic need different outlines, and reusing a
// regular glyph would render bold text as regular.
enum class GlyphStyle : std::uint8_t {
    kRegular = 0,
    kBold = 1,
    kItalic = 2,
    kBoldItalic = 3,
};

struct GlyphKey {
    std::uint32_t codepoint{0};
    GlyphStyle style{GlyphStyle::kRegular};

    friend bool operator==(const GlyphKey& a, const GlyphKey& b) noexcept {
        return a.codepoint == b.codepoint && a.style == b.style;
    }
};

struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& key) const noexcept {
        return (static_cast<std::size_t>(key.codepoint) * 131u) ^
               (static_cast<std::size_t>(key.style) * 2654435761u);
    }
};

class GlyphAtlas {
 public:
    struct Metrics {
        int cell_width{0};
        int cell_height{0};
        int baseline{0};  // Distance from the top of the cell to the glyph baseline.
    };

    GlyphAtlas();
    explicit GlyphAtlas(const QFont& font);
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // Rebuilds for a new font or DPI. Everything already rasterised is discarded, because
    // pixel metrics change with both.
    void reset(const QFont& font);

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

    // Rectangle of the glyph inside atlasImage(), or a null rect when the glyph is absent and
    // the caller should draw it directly.
    [[nodiscard]] QRect glyphRect(const GlyphKey& key) const;

    // Ensures the glyph is rasterised, then returns its rectangle.
    [[nodiscard]] QRect ensureGlyph(std::uint32_t codepoint, GlyphStyle style);

    [[nodiscard]] const QImage& atlasImage() const noexcept { return atlas_; }
    [[nodiscard]] int glyphCount() const noexcept { return static_cast<int>(cache_.size()); }
    // Atlas slots consumed, including the free rows kept back for glyphs that arrive later.
    [[nodiscard]] int usedSlots() const noexcept { return used_slots_; }

 private:
    int reserveRow();

    QImage atlas_;
    Metrics metrics_{};
    std::unordered_map<GlyphKey, QRect, GlyphKeyHash> cache_;
    int columns_{0};
    int used_slots_{0};
    int next_row_{0};
    int next_column_{0};
};

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_RENDERER_GLYPH_ATLAS_H_