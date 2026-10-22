// P1-A4 / P1-T1: the transport queue is the one piece of concurrent code everything else
// depends on, so its wrap-around and ordering guarantees are pinned here explicitly.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/SpscRingBuffer.h"

namespace openscrt::core {
namespace {

using Buffer = ByteRingBuffer;

TEST(SpscRingBufferTest, StartsEmpty) {
    Buffer buffer(8);
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.full());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.available(), 8u);
    EXPECT_EQ(buffer.capacity(), 8u);
}

TEST(SpscRingBufferTest, ZeroCapacityIsCorrectedToOne) {
    Buffer buffer(0);
    EXPECT_EQ(buffer.capacity(), 1u);
    EXPECT_EQ(buffer.write(nullptr, 0), 0u);
}

TEST(SpscRingBufferTest, WriteReadRoundTripPreservesOrder) {
    Buffer buffer(16);
    const std::vector<std::uint8_t> in = {1, 2, 3, 4, 5};
    EXPECT_EQ(buffer.write(in.data(), in.size()), in.size());

    std::vector<std::uint8_t> out(5, 0);
    EXPECT_EQ(buffer.read(out.data(), out.size()), 5u);
    EXPECT_EQ(in, out);
    EXPECT_TRUE(buffer.empty());
}

TEST(SpscRingBufferTest, WriteStopsAtCapacity) {
    Buffer buffer(4);
    const std::vector<std::uint8_t> in = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(buffer.write(in.data(), in.size()), 4u);
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(buffer.write(in.data(), in.size()), 0u);
    EXPECT_FALSE(buffer.tryWriteAll(in.data(), in.size()));

    std::vector<std::uint8_t> out(4, 0);
    EXPECT_EQ(buffer.read(out.data(), out.size()), 4u);
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3, 4}), out);
}

TEST(SpscRingBufferTest, TryWriteAllRejectsPartialRequests) {
    Buffer buffer(4);
    const std::vector<std::uint8_t> in = {1, 2, 3};
    EXPECT_TRUE(buffer.tryWriteAll(in.data(), in.size()));
    EXPECT_FALSE(buffer.tryWriteAll(in.data(), in.size() + 1));
}

TEST(SpscRingBufferTest, SurvivesRepeatedWrapAround) {
    Buffer buffer(5);
    std::uint8_t next_write = 0;

    // 1000 short bursts with deliberately irregular sizes so every offset inside the ring is
    // hit. Each burst writes and then drains: the buffer starts empty, so reading before
    // writing would be reading nothing.
    for (int burst = 0; burst < 1000; ++burst) {
        const std::size_t chunk = static_cast<std::size_t>(burst % 4) + 1;

        std::vector<std::uint8_t> in(chunk);
        for (std::size_t i = 0; i < chunk; ++i) {
            in[i] = static_cast<std::uint8_t>(next_write + i);
        }
        ASSERT_EQ(buffer.write(in.data(), chunk), chunk) << "burst " << burst;

        std::vector<std::uint8_t> out(chunk);
        ASSERT_EQ(buffer.read(out.data(), chunk), chunk) << "burst " << burst;
        EXPECT_EQ(out, in) << "burst " << burst;

        next_write = static_cast<std::uint8_t>(next_write + chunk);
    }
}

TEST(SpscRingBufferTest, ReadReturnsPartialWhenBufferIsShort) {
    Buffer buffer(8);
    const std::vector<std::uint8_t> in = {7, 8};
    ASSERT_EQ(buffer.write(in.data(), in.size()), 2u);

    std::vector<std::uint8_t> out(6, 0xAA);
    EXPECT_EQ(buffer.read(out.data(), out.size()), 2u);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[1], 8);
    EXPECT_EQ(out[2], 0xAA) << "must not touch memory beyond the copied count";
}

TEST(SpscRingBufferTest, SingleElementHelpers) {
    Buffer buffer(4);
    EXPECT_TRUE(buffer.tryWrite(42));
    std::uint8_t value = 0;
    EXPECT_TRUE(buffer.tryRead(value));
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(buffer.tryRead(value));
}

TEST(SpscRingBufferTest, ClearResetsAccounting) {
    Buffer buffer(4);
    ASSERT_EQ(buffer.write(nullptr, 0), 0u);
    const std::vector<std::uint8_t> in = {1, 2};
    ASSERT_EQ(buffer.write(in.data(), in.size()), 2u);
    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.available(), 4u);
}

TEST(SpscRingBufferTest, ConcurrentProducerAndConsumerLoseNoBytes) {
    // Mirrors the P1-A4 gate: run under -fsanitize=thread in CI. Each byte is tagged with the
    // producer index so a reorder or duplication is detectable, not just a count mismatch.
    constexpr std::uint64_t kTotal = 2'000'000;

    Buffer buffer(4096);
    std::vector<std::uint8_t> consumed;
    consumed.reserve(kTotal);
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::vector<std::uint8_t> chunk(1024);
        std::uint64_t written = 0;
        while (written < kTotal && !failed.load(std::memory_order_relaxed)) {
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), kTotal - written));
            for (std::size_t i = 0; i < n; ++i) {
                chunk[i] = static_cast<std::uint8_t>((written + i) & 0xFF);
            }
            const std::size_t accepted = buffer.write(chunk.data(), n);
            written += accepted;
        }
    });

    std::thread consumer([&] {
        std::vector<std::uint8_t> chunk(1024);
        while (consumed.size() < kTotal) {
            const std::size_t n = buffer.read(chunk.data(), chunk.size());
            if (n == 0) {
                if (consumed.size() + n > kTotal) {
                    failed.store(true, std::memory_order_relaxed);
                }
                continue;
            }
            const std::size_t base = consumed.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (chunk[i] != static_cast<std::uint8_t>((base + i) & 0xFF)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            consumed.insert(consumed.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load()) << "byte stream was reordered or corrupted";
    EXPECT_EQ(consumed.size(), kTotal);
}

}  // namespace
}  // namespace openscrt::core