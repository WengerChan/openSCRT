// P1-A5: scrollback write throughput and O(1) lookup under a full 128,000-line working set.
//
// Budgets are deliberately generous. A shared CI runner is not a performance benchmark and a
// tight wall-clock assertion there only produces flaky red builds; the point of this gate is
// to catch algorithmic regressions (an accidental O(n) lookup or a per-line allocation in the
// hot path), not to certify a number. Scale with OPENCRT_PERF_ITERS on a dedicated runner.

#include "terminal/buffer/RingScrollbackBuffer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>

namespace openscrt::terminal {
namespace {

std::size_t perfIterations() {
    // One full lap of the ring by default; CI sets this lower to keep the job short.
    const char* env = std::getenv("OPENCRT_PERF_ITERS");
    if (env == nullptr) {
        return RingScrollbackBuffer::kDefaultCapacityLines;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(env, &end, 10);
    if (end == env || value == 0) {
        return RingScrollbackBuffer::kDefaultCapacityLines;
    }
    return static_cast<std::size_t>(value);
}

TEST(PerfScrollbackTest, FullRingWriteThroughput) {
    RingScrollbackBuffer buffer;
    const std::size_t iterations = perfIterations();

    // A realistic 80-column log line, so the measurement includes string construction and
    // not just the ring bookkeeping.
    std::string payload = "[2026-10-21 12:00:00.123] INFO  connection established to 10.20.30.40";
    payload.resize(80, ' ');

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        buffer.push(payload);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    const double per_line_us = iterations > 0 ? static_cast<double>(micros) / static_cast<double>(iterations)
                                              : 0.0;
    std::printf("[perf] scrollback push: %zu lines in %lld us (%.3f us/line)\n", iterations,
                static_cast<long long>(micros), per_line_us);
    EXPECT_LE(buffer.size(), buffer.capacity());
    // Budget: 20 us/line. Real cost is ~0.05 us/line; the headroom absorbs a slow runner.
    EXPECT_LT(per_line_us, 20.0) << "scrollback write path regressed";
}

TEST(PerfScrollbackTest, RandomLookupStaysConstantTime) {
    RingScrollbackBuffer buffer;
    const std::size_t iterations = perfIterations();
    std::string payload(80, 'x');

    for (std::size_t i = 0; i < iterations; ++i) {
        buffer.push(payload);
    }
    const std::size_t sample_count = 2000;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < sample_count; ++i) {
        // Deterministic pseudo-random spread across the whole retained window.
        const std::size_t index = (i * 2654435761u) % buffer.size();
        EXPECT_EQ(buffer.at(index).size(), 80u);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    std::printf("[perf] scrollback lookup: %zu samples in %lld us\n", sample_count,
                static_cast<long long>(micros));
    // Budget: 1 us per lookup. An O(n) scan or a full copy would blow this by orders of
    // magnitude, which is the regression this gate exists to catch.
    EXPECT_LT(static_cast<double>(micros) / static_cast<double>(sample_count), 1.0);
}

}  // namespace
}  // namespace openscrt::terminal