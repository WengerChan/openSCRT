// Lock-free single-producer / single-consumer ring buffer.
//
// This is the only sanctioned path for raw transport bytes travelling from a session
// I/O worker to the parser worker (see docs/DEVELOPMENT_PLAN.md 2.4). Deliberately
// header-only and trivially-copyable-element only: the hot path must be a memcpy plus
// two relaxed/release stores, with no allocation and no syscalls.

#ifndef OPENCRT_CORE_SPSC_RING_BUFFER_H_
#define OPENCRT_CORE_SPSC_RING_BUFFER_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace openscrt::core {

// Monotonic (never-wrapping) 64-bit counters: a producer may safely run 16 EiB ahead of
// its consumer before wraparound matters, so the modulo arithmetic stays branch-light and
// the buffer size is not restricted to a power of two.
using SequenceCounter = std::uint64_t;

template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRingBuffer stores bytes; element type must be trivially copyable so the "
                  "hot path can use memcpy without constructors");

public:
    using value_type = T;

    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity),
          buffer_(new T[capacity_ == 0 ? 1 : capacity_]) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    ~SpscRingBuffer() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    [[nodiscard]] std::size_t available() const noexcept { return capacity_ - size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return available() == 0; }

    // Producer side. Copies as much as fits and returns the number of elements accepted;
    // a short write is normal back-pressure, not an error.
    std::size_t write(const T* src, std::size_t count) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        const std::size_t to_copy = std::min(count, capacity_ - static_cast<std::size_t>(tail - head));
        if (to_copy == 0) {
            return 0;
        }
        const std::size_t at_tail = static_cast<std::size_t>(tail % capacity_);
        const std::size_t first_chunk = std::min(to_copy, capacity_ - at_tail);
        std::memcpy(buffer_.get() + at_tail, src, first_chunk * sizeof(T));
        if (to_copy > first_chunk) {
            std::memcpy(buffer_.get(), src + first_chunk, (to_copy - first_chunk) * sizeof(T));
        }
        tail_.store(tail + to_copy, std::memory_order_release);
        return to_copy;
    }

    std::size_t write(T value) noexcept { return write(&value, 1); }

    // Strict producer: all-or-nothing, so the transport never splits a control sequence
    // across a wrap boundary by accident.
    [[nodiscard]] bool tryWriteAll(const T* src, std::size_t count) noexcept {
        if (count > available()) {
            return false;
        }
        return write(src, count) == count;
    }

    // Consumer side. Copies at most `max_count` elements and returns how many were read.
    std::size_t read(T* dst, std::size_t max_count) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        const std::size_t to_copy = std::min(max_count, static_cast<std::size_t>(tail - head));
        if (to_copy == 0) {
            return 0;
        }
        const std::size_t at_head = static_cast<std::size_t>(head % capacity_);
        const std::size_t first_chunk = std::min(to_copy, capacity_ - at_head);
        std::memcpy(dst, buffer_.get() + at_head, first_chunk * sizeof(T));
        if (to_copy > first_chunk) {
            std::memcpy(dst + first_chunk, buffer_.get(), (to_copy - first_chunk) * sizeof(T));
        }
        head_.store(head + to_copy, std::memory_order_release);
        return to_copy;
    }

    [[nodiscard]] bool tryRead(T& out) noexcept { return read(&out, 1) == 1; }

    // Drops every buffered element. Only legal while no producer/consumer is running.
    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    const std::size_t capacity_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<SequenceCounter> head_{0};  // consumer-owned
    alignas(64) std::atomic<SequenceCounter> tail_{0};  // producer-owned
};

// Transport byte stream is unsigned 8-bit; using uint8_t keeps memcpy arithmetic simple.
using ByteRingBuffer = SpscRingBuffer<std::uint8_t>;

}  // namespace openscrt::core

#endif  // OPENCRT_CORE_SPSC_RING_BUFFER_H_