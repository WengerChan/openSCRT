// P1-A3: the widget's own frame time.
//
// The other perf test measures the renderer. This measures the widget, which is what the
// user actually sees: the renderer's damage repaint plus the blit of the backing image,
// through the real paintEvent of a real QWidget.
//
// The offscreen platform still runs the whole paint path -- the backing store, the
// rasteriser, the paint event -- so the number is the frame cost. What it does not model
// is the display server: the compositing and the panel's refresh rate are not here, and
// that part of the criterion stays a human item.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFont>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <vector>

#include "ui/workspace/TerminalCanvasWidget.h"

namespace openscrt::ui {
namespace {

const QApplication& application() {
    static QApplication* instance = [] {
        static int argc = 1;
        static char name[] = "openscrt-widget-perf";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return *instance;
}

class WidgetFrameBudgetTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override {
        widget_ = std::make_unique<TerminalCanvasWidget>();
        widget_->setFont(QFont(QStringLiteral("monospace")));
        widget_->resize(800, 480);
        widget_->show();
        application().processEvents();
    }

    void TearDown() override { widget_.reset(); }

    // Feeds a burst of output and times the frame it produces.
    //
    // A frame is two pieces: the renderer repaints the damaged rows, and the paint event
    // blits the backing image. The widget's own tick does them in that order and this
    // does the same, so the number is the frame's cost. The one difference is that the
    // widget's update() is asynchronous and this blit is not, which makes the measurement
    // slightly pessimistic rather than flattering.
    void frameWithOutput(std::string_view output) {
        widget_->feed(output);
        const auto start = std::chrono::steady_clock::now();
        // The tick's work -- repainting the damaged rows -- and then the blit it asks
        // for, which the event loop delivers.
        widget_->drawFrame();
        application().processEvents();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        frame_times_.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    }

    [[nodiscard]] double percentile(double fraction) {
        std::sort(frame_times_.begin(), frame_times_.end());
        const auto rank =
            static_cast<std::size_t>(static_cast<double>(frame_times_.size()) * fraction);
        return frame_times_[std::min(rank, frame_times_.size() - 1)];
    }

    std::unique_ptr<TerminalCanvasWidget> widget_;
    std::vector<double> frame_times_;
};

TEST_F(WidgetFrameBudgetTest, TheWidgetKeepsUpWithTheFrameBudget) {
    // A line of output per frame, which is what a shell printing a result looks like.
    for (int i = 0; i < 200; ++i) {
        frameWithOutput("a line of terminal output that has to be laid out and painted\r\n");
    }

    const double p95 = percentile(0.95);
    const double worst = frame_times_.back();
    const double mean = std::accumulate(frame_times_.begin(), frame_times_.end(), 0.0) /
                        static_cast<double>(frame_times_.size());

    std::printf("[perf] widget frame: 200 frames, mean %.2f ms, p95 %.2f ms, worst %.2f ms\n", mean,
                p95, worst);

    // The criterion: the frame time's p95 must stay inside a 60 FPS frame.
    EXPECT_LE(p95, 16.7) << "the widget misses the 60 FPS frame budget";
    EXPECT_GT(widget_->repaintedRowCount(), 0) << "the widget painted nothing at all";
}

TEST_F(WidgetFrameBudgetTest, AFullScreenOfOutputStillFitsInAFrame) {
    // The worst case for the renderer: every row changes, so the whole screen is painted.
    for (int i = 0; i < 50; ++i) {
        std::string burst;
        for (int row = 1; row <= 24; ++row) {
            burst +=
                "\x1b[" + std::to_string(row) + ";1H" + "a full row of text " + std::to_string(row);
        }
        frameWithOutput(burst);
    }

    const double p95 = percentile(0.95);
    std::printf("[perf] widget frame (full screen): p95 %.2f ms, worst %.2f ms\n", p95,
                frame_times_.back());
    EXPECT_LE(p95, 16.7) << "a full-screen repaint misses the 60 FPS frame budget";
}

}  // namespace
}  // namespace openscrt::ui
