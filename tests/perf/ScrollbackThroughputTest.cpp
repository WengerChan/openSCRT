// P1-A5: scrollback write throughput and O(1) lookup under a full 128,000-line working set.
//
// Budgets are deliberately generous. A shared CI runner is not a performance benchmark and a
// tight wall-clock assertion there only produces flaky red builds; the point of this gate is
// to catch algorithmic regressions (an accidental O(n) lookup or a per-line allocation in the
// hot path), not to certify a number. Scale with OPENCRT_PERF_ITERS on a dedicated runner.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "terminal/buffer/RingScrollbackBuffer.h"

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

    const double per_line_us =
        iterations > 0 ? static_cast<double>(micros) / static_cast<double>(iterations) : 0.0;
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

TEST(PerfScrollbackTest, LookupLatencyP99StaysUnderOneMillisecond) {
    // P1-A5 asks for the p99 of a single retrieval, not the average of a batch: an
    // occasional slow lookup is what a user feels as a stutter when scrolling back.
    RingScrollbackBuffer buffer;
    const std::size_t iterations = perfIterations();
    std::string payload(80, 'x');

    for (std::size_t i = 0; i < iterations; ++i) {
        buffer.push(payload);
    }
    ASSERT_GT(buffer.size(), 0u);

    const std::size_t sample_count = 2000;
    std::vector<std::chrono::nanoseconds> samples;
    samples.reserve(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const std::size_t index = (i * 2654435761u) % buffer.size();
        const auto start = std::chrono::steady_clock::now();
        const std::string line = buffer.at(index);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        samples.push_back(elapsed);
        ASSERT_EQ(line.size(), 80u);
    }

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double fraction) {
        const std::size_t rank =
            static_cast<std::size_t>(static_cast<double>(samples.size()) * fraction);
        return samples[std::min(rank, samples.size() - 1)];
    };
    const auto p99 = percentile(0.99);
    const auto worst = samples.back();
    const double p99_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(p99).count() / 1000.0;
    const double worst_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(worst).count() / 1000.0;

    std::printf("[perf] scrollback lookup p99: %.3f us (worst %.3f us, %zu samples)\n", p99_us,
                worst_us, sample_count);
    // The budget is 1 ms, four orders of magnitude above the real cost. The point is to
    // catch an accidental O(n) scan or a copy of the whole window, not to certify a number.
    EXPECT_LT(p99_us, 1000.0) << "the p99 retrieval latency blew the 1 ms budget";
}

}  // namespace
}  // namespace openscrt::terminal