// Frame pacing tests (P1-T8).
//
// These assert the roadmap's §6.1 rule directly: a burst of output must collapse into a
// single repaint. A regression here is invisible in a screenshot -- the terminal would still
// look right, just burn a core -- so it has to be pinned by test.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "terminal/renderer/FrameCoalescer.h"

namespace openscrt::terminal {
namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// Deterministic clock ticks. time_point is nanosecond-based, so it has to be built from a
// duration rather than from a millisecond count directly.
Clock::time_point at(int elapsed_ms) {
    return Clock::time_point(Ms(elapsed_ms));
}

TEST(FrameCoalescerTest, StartsIdleButStillDrawsTheFirstFrame) {
    FrameCoalescer coalescer{60};
    EXPECT_FALSE(coalescer.framePending());
    // No output has arrived, but a freshly shown canvas still has to paint itself once.
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    EXPECT_FALSE(coalescer.shouldRepaint(at(4)));
}

TEST(FrameCoalescerTest, SignalCausesExactlyOneFrame) {
    FrameCoalescer coalescer{60};
    coalescer.signal();
    EXPECT_TRUE(coalescer.framePending());
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    EXPECT_FALSE(coalescer.framePending());
    EXPECT_FALSE(coalescer.shouldRepaint(at(0)));
}

TEST(FrameCoalescerTest, ABurstCollapsesIntoASingleFrame) {
    // This is the rule: `seq 1 1000000` arrives as tens of thousands of notifications and
    // must cost one repaint, not one per notification.
    FrameCoalescer coalescer{60};
    for (int i = 0; i < 10000; ++i) {
        coalescer.signal();
    }
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    const auto stats = coalescer.stats();
    EXPECT_EQ(stats.output_events, 10000u);
    EXPECT_EQ(stats.repaint_frames, 1u);
    EXPECT_EQ(stats.coalesced_events, 9999u);
}

TEST(FrameCoalescerTest, ContinuousOutputIsCappedAtTheRefreshRate) {
    FrameCoalescer coalescer{60};  // 16 ms
    EXPECT_EQ(coalescer.interval(), Ms(16));

    // Signal continuously and tick the timer every millisecond for 200 ms.
    int frames = 0;
    for (int ms = 0; ms < 200; ++ms) {
        coalescer.signal();
        if (coalescer.shouldRepaint(at(ms))) {
            ++frames;
        }
    }
    // 200 ms at 60 Hz is ~12 frames; allow slack for integer interval rounding.
    EXPECT_GE(frames, 10);
    EXPECT_LE(frames, 14);
}

TEST(FrameCoalescerTest, OneHTwentyRefreshDoublesTheRate) {
    FrameCoalescer coalescer{kHighRefreshHz};
    EXPECT_EQ(coalescer.interval(), Ms(8));
}

TEST(FrameCoalescerTest, IdleTerminalDoesNotRepaint) {
    FrameCoalescer coalescer{60};
    // One priming repaint puts the coalescer on its clock; after that it must stay idle.
    ASSERT_TRUE(coalescer.shouldRepaint(at(0)));
    // Nothing happening: no work for every 100 ms.
    for (int ms = 100; ms <= 1000; ms += 100) {
        EXPECT_FALSE(coalescer.shouldRepaint(at(ms))) << "at " << ms << " ms";
    }
    EXPECT_EQ(coalescer.stats().repaint_frames, 1u);
}

TEST(FrameCoalescerTest, WorkArrivingMidIntervalWaitsForTheBoundary) {
    FrameCoalescer coalescer{60};
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    coalescer.signal();
    EXPECT_TRUE(coalescer.framePending());
    // Too soon: keep the flag set rather than dropping the work.
    EXPECT_FALSE(coalescer.shouldRepaint(at(8)));
    EXPECT_TRUE(coalescer.framePending());
    EXPECT_TRUE(coalescer.shouldRepaint(at(16)));
}

TEST(FrameCoalescerTest, InvalidateForcesAnImmediateRepaint) {
    FrameCoalescer coalescer{60};
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    coalescer.invalidate();
    EXPECT_TRUE(coalescer.shouldRepaint(at(1))) << "resize/expose must not wait a frame";
}

TEST(FrameCoalescerTest, ZeroRefreshRateFallsBackToASafeInterval) {
    FrameCoalescer coalescer{0};
    EXPECT_EQ(coalescer.interval(), Ms(16)) << "division by zero must not reach the caller";
}

TEST(FrameCoalescerTest, SignallingFromManyThreadsLosesNoWork) {
    FrameCoalescer coalescer{60};
    constexpr int kThreads = 4;
    constexpr int kSignalsPerThread = 25000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&coalescer] {
            for (int i = 0; i < kSignalsPerThread; ++i) {
                coalescer.signal();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(coalescer.stats().output_events,
              static_cast<std::uint64_t>(kThreads * kSignalsPerThread));
    EXPECT_TRUE(coalescer.framePending()) << "signals from any thread must leave work pending";
    EXPECT_TRUE(coalescer.shouldRepaint(at(0)));
    EXPECT_EQ(coalescer.stats().repaint_frames, 1u);
}

}  // namespace
}  // namespace openscrt::terminal