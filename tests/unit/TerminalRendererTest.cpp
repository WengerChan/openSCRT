// Renderer tests (P1-T6/P1-T7) and the P1-A3 frame-rate budget.
//
// These run headlessly: Qt's offscreen platform plugin needs no display server, so the
// renderer can be measured in CI rather than only on a developer's machine.

#include <gtest/gtest.h>

#include <QFont>
#include <QGuiApplication>
#include <QtGlobal>
#include <chrono>
#include <memory>
#include <string>

#include "terminal/emulator/TerminalEmulator.h"
#include "terminal/renderer/FrameCoalescer.h"
#include "ui/renderer/GlyphAtlas.h"
#include "ui/renderer/TerminalRenderer.h"

namespace openscrt::ui {
namespace {

// One application per process; the offscreen platform keeps this valid in CI.
const QGuiApplication& application() {
    static QGuiApplication* instance = [] {
        // Must be set before QGuiApplication is constructed.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "openscrt-tests";
        static char* argv[] = {name, nullptr};
        return new QGuiApplication(argc, argv);
    }();
    return *instance;
}

class RendererTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override {
        grid_ = std::make_unique<terminal::ScreenGrid>(24, 80);
        renderer_ = std::make_unique<TerminalRenderer>(*grid_);
        renderer_->setFont(QFont(QStringLiteral("monospace")));
        renderer_->resetStats();
    }

    void writeRow(int row, const std::string& text) {
        grid_->setCursor(row, 0);
        for (const char ch : text) {
            grid_->put(static_cast<std::uint32_t>(static_cast<unsigned char>(ch)));
        }
    }

    std::unique_ptr<terminal::ScreenGrid> grid_;
    std::unique_ptr<TerminalRenderer> renderer_;
};

// ------------------------------------------------------------------------------- atlas

TEST_F(RendererTest, AtlasReportsUsableCellMetrics) {
    const auto metrics = renderer_->cellMetrics();
    EXPECT_GT(metrics.width, 0);
    EXPECT_GT(metrics.height, 0);
}

TEST_F(RendererTest, AtlasRasterisesAsciiAndCjk) {
    GlyphAtlas atlas{QFont(QStringLiteral("monospace"))};
    const QRect ascii = atlas.ensureGlyph('A', GlyphStyle::kRegular);
    const QRect cjk = atlas.ensureGlyph(0x4E2D, GlyphStyle::kRegular);
    EXPECT_FALSE(ascii.isNull());
    EXPECT_FALSE(cjk.isNull());
    EXPECT_EQ(ascii.width(), atlas.metrics().cell_width);
    EXPECT_EQ(ascii.height(), atlas.metrics().cell_height);
    EXPECT_EQ(atlas.glyphCount(), 2);
}

TEST_F(RendererTest, AtlasCachesRepeatedLookups) {
    GlyphAtlas atlas{QFont(QStringLiteral("monospace"))};
    const QRect first = atlas.ensureGlyph('x', GlyphStyle::kRegular);
    const QRect second = atlas.ensureGlyph('x', GlyphStyle::kRegular);
    EXPECT_EQ(first, second);
    EXPECT_EQ(atlas.glyphCount(), 1);
}

TEST_F(RendererTest, AtlasKeepsStylesApart) {
    GlyphAtlas atlas{QFont(QStringLiteral("monospace"))};
    const QRect regular = atlas.ensureGlyph('A', GlyphStyle::kRegular);
    const QRect bold = atlas.ensureGlyph('A', GlyphStyle::kBold);
    EXPECT_NE(regular, bold) << "bold must not reuse the regular outline";
    EXPECT_EQ(atlas.glyphCount(), 2);
}

TEST_F(RendererTest, AtlasRejectsInvalidScalars) {
    GlyphAtlas atlas{QFont(QStringLiteral("monospace"))};
    EXPECT_TRUE(atlas.ensureGlyph(0, GlyphStyle::kRegular).isNull());
    EXPECT_TRUE(atlas.ensureGlyph(0x110000, GlyphStyle::kRegular).isNull());
}

TEST_F(RendererTest, ResetDiscardsEverythingRasterised) {
    GlyphAtlas atlas{QFont(QStringLiteral("monospace"))};
    static_cast<void>(atlas.ensureGlyph('a', GlyphStyle::kRegular));
    atlas.reset(QFont(QStringLiteral("monospace"), 20));
    EXPECT_EQ(atlas.glyphCount(), 0);
    EXPECT_TRUE(atlas.glyphRect({'a', GlyphStyle::kRegular}).isNull());
}

// ---------------------------------------------------------------------------- renderer

TEST_F(RendererTest, CleanGridPaintsNothing) {
    EXPECT_EQ(renderer_->repaintDamage(), 0) << "no damage must not cost a repaint";
    EXPECT_EQ(renderer_->stats().frames, 0);
}

TEST_F(RendererTest, OneDirtyRowCostsOneRowOfPaint) {
    writeRow(5, "hello");
    EXPECT_EQ(renderer_->repaintDamage(), 1);
    EXPECT_EQ(renderer_->stats().rows_skipped, grid_->rows() - 1);
}

TEST_F(RendererTest, AdjacentRowsCoalesceIntoOnePaintPass) {
    for (int row = 4; row <= 6; ++row) {
        writeRow(row, "x");
    }
    EXPECT_EQ(renderer_->repaintDamage(), 3);
    EXPECT_EQ(renderer_->stats().frames, 1) << "three dirty rows, one repaint";
}

TEST_F(RendererTest, FullScreenDamagePaintsEveryRowOnce) {
    for (int row = 0; row < grid_->rows(); ++row) {
        writeRow(row, "y");
    }
    EXPECT_EQ(renderer_->repaintDamage(), grid_->rows());
    EXPECT_EQ(renderer_->stats().rows_skipped, 0);
}

TEST_F(RendererTest, BackbufferMatchesTheGridGeometry) {
    const auto metrics = renderer_->cellMetrics();
    EXPECT_EQ(renderer_->backbuffer().width(), grid_->cols() * metrics.width);
    EXPECT_EQ(renderer_->backbuffer().height(), grid_->rows() * metrics.height);
}

TEST_F(RendererTest, TextActuallyChangesPixels) {
    const QImage before = renderer_->backbuffer();
    writeRow(0, "MMMM");
    renderer_->repaintDamage();
    EXPECT_NE(renderer_->backbuffer(), before) << "the glyphs were never drawn";
}

TEST_F(RendererTest, ResizeKeepsGridAndBackbufferInStep) {
    renderer_->resize(40, 120);
    const auto metrics = renderer_->cellMetrics();
    EXPECT_EQ(grid_->rows(), 40);
    EXPECT_EQ(grid_->cols(), 120);
    EXPECT_EQ(renderer_->backbuffer().width(), 120 * metrics.width);
    EXPECT_EQ(renderer_->backbuffer().height(), 40 * metrics.height);
}

// ------------------------------------------------------------------------- P1-A3 gate

TEST_F(RendererTest, HighVolumeOutputStaysInsideTheFrameBudget) {
    // The P1-A3 scenario. Emulator parses the stream, the renderer repaints only what
    // changed, and the coalescer decides when a frame actually happens. Budget is
    // deliberately loose: this catches algorithmic regressions, it is not a benchmark.
    terminal::TerminalEmulator emulator{*grid_};

    constexpr int kLines = 20000;
    const auto parse_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kLines; ++i) {
        emulator.write(std::to_string(i + 1) + "\r\n");
    }
    const auto parse_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start)
            .count();

    renderer_->resetStats();
    const auto paint_start = std::chrono::steady_clock::now();
    const int painted = renderer_->repaintDamage();
    const auto paint_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - paint_start)
            .count();

    std::printf("[perf] %d lines: parse %.1f ms (%.3f us/line), paint %d rows in %.2f ms\n", kLines,
                parse_ms, parse_ms * 1000.0 / kLines, painted, paint_ms);

    EXPECT_LT(parse_ms, 2000.0) << "parser throughput regressed";
    // Only the rows that actually changed should be repainted, never the whole screen more
    // than once per frame.
    EXPECT_LE(painted, grid_->rows()) << "a single frame must not repaint more than one screen";
    EXPECT_LT(paint_ms, 250.0) << "repaint regressed";
}

TEST_F(RendererTest, RepeatedPartialRepaintsCostLessThanFullRepaints) {
    // The dirty-rectangle claim, measured: touching one line 50 times must be materially
    // cheaper than repainting the screen 50 times.
    const auto one_line_cost = [&] {
        renderer_->resetStats();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) {
            writeRow(3, std::to_string(i));
            renderer_->repaintDamage();
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    }();

    const auto full_cost = [&] {
        renderer_->resetStats();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) {
            renderer_->repaintAll();
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    }();

    std::printf("[perf] 50 partial repaints %.2f ms vs 50 full repaints %.2f ms\n", one_line_cost,
                full_cost);
    EXPECT_LT(one_line_cost, full_cost) << "dirty-row tracking is not actually saving any work";
}

TEST_F(RendererTest, CoalescerAndRendererAgreeOnFrameCount) {
    // The two halves of the pacing design must not disagree: a burst of writes produces
    // exactly one frame, and that frame repaints exactly the damaged rows.
    terminal::TerminalEmulator emulator{*grid_};
    terminal::FrameCoalescer coalescer{60};
    renderer_->resetStats();

    for (int i = 0; i < 500; ++i) {
        emulator.write("x");
        coalescer.signal();
    }

    EXPECT_TRUE(coalescer.shouldRepaint(std::chrono::steady_clock::time_point{}));
    const int painted = renderer_->repaintDamage();
    EXPECT_EQ(renderer_->stats().frames, 1);
    EXPECT_EQ(painted, 1) << "only the row that received the text";
    EXPECT_FALSE(coalescer.shouldRepaint(std::chrono::steady_clock::time_point{}))
        << "a second immediate tick must not repaint again";
}

}  // namespace
}  // namespace openscrt::ui