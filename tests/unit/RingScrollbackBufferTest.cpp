// P1-T3 / P1-A5: the scrollback ring must keep ordering across page recycling and never grow
// past its configured line budget.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "terminal/buffer/RingScrollbackBuffer.h"

namespace openscrt::terminal {
namespace {

std::string lineNumber(std::size_t n) {
    return "line-" + std::to_string(n);
}

TEST(RingScrollbackBufferTest, StartsEmpty) {
    RingScrollbackBuffer buffer(128);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.totalLinesWritten(), 0u);
    EXPECT_EQ(buffer.droppedLines(), 0u);
}

TEST(RingScrollbackBufferTest, CapacityRoundsUpToWholePages) {
    // The ring wraps on page boundaries, so a partial-page capacity would need two different
    // slot-arithmetic paths.
    RingScrollbackBuffer buffer(10);
    EXPECT_EQ(buffer.capacity(), RingScrollbackBuffer::kLinesPerPage);
    EXPECT_EQ(buffer.capacity() % RingScrollbackBuffer::kLinesPerPage, 0u);
}

TEST(RingScrollbackBufferTest, PreservesInsertionOrder) {
    RingScrollbackBuffer buffer(RingScrollbackBuffer::kLinesPerPage);
    for (std::size_t i = 0; i < 100; ++i) {
        buffer.push(lineNumber(i));
    }
    ASSERT_EQ(buffer.size(), 100u);
    for (std::size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(buffer.at(i), lineNumber(i));
    }
}

TEST(RingScrollbackBufferTest, OverwritesOldestBeyondCapacity) {
    RingScrollbackBuffer buffer(RingScrollbackBuffer::kLinesPerPage);
    const std::size_t total = RingScrollbackBuffer::kLinesPerPage + 50;

    for (std::size_t i = 0; i < total; ++i) {
        buffer.push(lineNumber(i));
    }

    EXPECT_EQ(buffer.size(), RingScrollbackBuffer::kLinesPerPage);
    EXPECT_EQ(buffer.totalLinesWritten(), total);
    EXPECT_EQ(buffer.droppedLines(), 50u);

    // The 50 oldest lines are gone; the newest is intact.
    EXPECT_EQ(buffer.at(0), lineNumber(50));
    EXPECT_EQ(buffer.at(buffer.size() - 1), lineNumber(total - 1));
}

TEST(RingScrollbackBufferTest, HandlesManyPageRecycles) {
    // Far more lines than pages, so every page slot is reused dozens of times; catches slot
    // arithmetic that only holds for the first lap.
    RingScrollbackBuffer buffer(2 * RingScrollbackBuffer::kLinesPerPage);
    const std::size_t total = 10 * RingScrollbackBuffer::kLinesPerPage;

    for (std::size_t i = 0; i < total; ++i) {
        buffer.push(lineNumber(i));
    }

    ASSERT_EQ(buffer.size(), buffer.capacity());
    const std::size_t first_retained = total - buffer.capacity();
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        ASSERT_EQ(buffer.at(i), lineNumber(first_retained + i)) << "index " << i;
    }
}

TEST(RingScrollbackBufferTest, RejectsOutOfRangeIndex) {
    RingScrollbackBuffer buffer(RingScrollbackBuffer::kLinesPerPage);
    buffer.push("only");
    EXPECT_TRUE(buffer.empty_at(1));
    EXPECT_FALSE(buffer.empty_at(0));
    // at() is [[nodiscard]] on purpose; discard it explicitly inside the throwing macro.
    EXPECT_THROW(static_cast<void>(buffer.at(1)), std::out_of_range);
}

TEST(RingScrollbackBufferTest, PushFromRange) {
    RingScrollbackBuffer buffer(RingScrollbackBuffer::kLinesPerPage);
    const std::string text = "abcdef";
    buffer.push(text.data(), text.data() + 3);
    buffer.push(text.data(), text.data());  // empty range is a no-op
    ASSERT_EQ(buffer.size(), 1u);
    EXPECT_EQ(buffer.at(0), "abc");
}

TEST(RingScrollbackBufferTest, ClearResetsCounters) {
    RingScrollbackBuffer buffer(RingScrollbackBuffer::kLinesPerPage);
    buffer.push("x");
    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.totalLinesWritten(), 0u);
    EXPECT_EQ(buffer.droppedLines(), 0u);

    buffer.push("y");
    EXPECT_EQ(buffer.at(0), "y");
}

TEST(RingScrollbackBufferTest, DefaultCapacityCoversTheRoadmapMaximum) {
    RingScrollbackBuffer buffer;
    // The ring wraps on a page boundary, so the effective capacity is rounded up to a whole
    // number of pages: 128,000 is not a multiple of 4096, so the buffer holds 131,072 lines.
    // The contract is "at least the roadmap maximum", not "exactly".
    EXPECT_GE(buffer.capacity(), RingScrollbackBuffer::kDefaultCapacityLines);
    EXPECT_EQ(buffer.capacity() % RingScrollbackBuffer::kLinesPerPage, 0u);
}

}  // namespace
}  // namespace openscrt::terminal