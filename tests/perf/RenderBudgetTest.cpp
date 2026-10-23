// P1-A3 frame-rate budget, measured in CI.
//
// The unit tier reports pass/fail; this tier is the one whose *numbers* matter, so its output
// is echoed verbatim into the run log. The roadmap's acceptance criterion is "UI stays at or
// above 60 FPS while seq 1 1000000 is streaming", which decomposes into three things this
// file measures separately:
//
//   1. parse throughput   -- how fast the emulator turns bytes into screen state
//   2. repaint cost       -- how long painting the damaged rows takes
//   3. incremental saving -- repainting one changed row versus the whole screen
//
// A full end-to-end FPS figure needs the widget and a real display, which is recorded as the
// manual part of P1-A3. This file gives the machine-checkable half.

#include <gtest/gtest.h>

#include <QFont>
#include <QGuiApplication>
#include <QtGlobal>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "terminal/emulator/TerminalEmulator.h"
#include "terminal/renderer/DamageTracker.h"
#include "terminal/renderer/FrameCoalescer.h"
#include "ui/renderer/TerminalRenderer.h"

namespace openscrt::ui {
namespace {

const QGuiApplication& application() {
    static QGuiApplication* instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "openscrt-perf";
        static char* argv[] = {name, nullptr};
        return new QGuiApplication(argc, argv);
    }();
    return *instance;
}

class RenderBudgetTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override {
        grid_ = std::make_unique<terminal::ScreenGrid>(24, 80);
        renderer_ = std::make_unique<TerminalRenderer>(*grid_);
        renderer_->setFont(QFont(QStringLiteral("monospace")));
        renderer_->resetStats();
    }

    static double milliseconds(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    }

    std::unique_ptr<terminal::ScreenGrid> grid_;
    std::unique_ptr<TerminalRenderer> renderer_;
};

TEST_F(RenderBudgetTest, ParseThroughputUnderALongStream) {
    terminal::TerminalEmulator emulator{*grid_};

    constexpr int kLines = 20000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kLines; ++i) {
        emulator.write(std::to_string(i + 1) + "\r\n");
    }
    const double elapsed = milliseconds(start);

    std::fprintf(stderr, "[perf] parse: %d lines in %.1f ms (%.3f us/line, %.0f lines/s)\n", kLines,
                 elapsed, elapsed * 1000.0 / kLines,
                 elapsed > 0 ? kLines / (elapsed / 1000.0) : 0.0);

    // 60 FPS with a 16 ms budget leaves roughly 10 ms of the frame for parsing. At 20k lines
    // the whole stream must parse in well under that to keep the frame loop fed.
    EXPECT_LT(elapsed, 1000.0) << "parser throughput regressed";
}

TEST_F(RenderBudgetTest, RepaintOfAFullScreenStaysInsideAFrame) {
    terminal::TerminalEmulator emulator{*grid_};
    for (int row = 0; row < grid_->rows(); ++row) {
        emulator.write("\x1B[" + std::to_string(row + 1) +
                       ";1H"
                       "the quick brown fox jumps over "
                       "the lazy dog 0123456789 abcdefghij");
    }

    renderer_->resetStats();
    const auto start = std::chrono::steady_clock::now();
    const int painted = renderer_->repaintDamage();
    const double elapsed = milliseconds(start);

    std::fprintf(stderr, "[perf] repaint: %d rows in %.2f ms (%.1f FPS equivalent for one frame)\n",
                 painted, elapsed, elapsed > 0 ? 1000.0 / elapsed : 0.0);

    EXPECT_GT(painted, 0);
    EXPECT_LE(painted, grid_->rows()) << "one frame repaints at most one screen";
    EXPECT_LT(elapsed, 16.0) << "a full-screen repaint must fit inside a 60 FPS frame budget";
}

TEST_F(RenderBudgetTest, IncrementalRepaintIsMuchCheaperThanFull) {
    renderer_->resetStats();

    const auto partial_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        grid_->setCursor(3, 0);
        grid_->put(static_cast<std::uint32_t>('0' + (i % 10)));
        renderer_->repaintDamage();
    }
    const double partial = milliseconds(partial_start);

    const auto full_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        renderer_->repaintAll();
    }
    const double full = milliseconds(full_start);

    std::fprintf(stderr, "[perf] 200 repaints: partial %.2f ms vs full %.2f ms (%.1fx)\n", partial,
                 full, partial > 0 ? full / partial : 0.0);

    // The dirty-rectangle design only earns its keep if this ratio is real. It was 1.3x
    // while the renderer shaped text per cell; with tinted-atlas blits it should be far
    // higher, and a regression back to drawText would show up here first.
    EXPECT_LT(partial, full) << "dirty-row tracking is not saving any work";
    EXPECT_LT(partial * 3, full) << "incremental repaint must be at least 3x cheaper";
    EXPECT_LT(partial / 200.0, 16.0) << "an incremental repaint must fit inside one frame";
}

TEST_F(RenderBudgetTest, SustainedOutputStaysAtTheConfiguredRefreshRate) {
    terminal::TerminalEmulator emulator{*grid_};
    terminal::FrameCoalescer coalescer{60};
    renderer_->resetStats();

    constexpr int kTicks = 600;  // 600 ms of wall clock at 60 Hz
    const auto origin = std::chrono::steady_clock::time_point{};
    int frames = 0;
    for (int tick = 0; tick < kTicks; ++tick) {
        for (int i = 0; i < 100; ++i) {
            emulator.write("x");  // heavy output between every tick
            coalescer.signal();
        }
        if (coalescer.shouldRepaint(origin + std::chrono::milliseconds(tick))) {
            renderer_->repaintDamage();
            ++frames;
        }
    }

    std::fprintf(stderr, "[perf] 600 ms of saturated output produced %d frames (%.1f FPS)\n",
                 frames, frames * 1000.0 / kTicks);
    EXPECT_GE(frames, 30) << "the coalescer is starving the renderer";
    EXPECT_LE(frames, 42) << "the 60 Hz cap is not being enforced";
}

}  // namespace
}  // namespace openscrt::ui