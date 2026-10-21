// Paged ring scrollback buffer.
//
// Roadmap 3.2 calls for up to 128,000 retained lines. A flat std::deque<std::string> would
// churn the allocator on every line during heavy `tail -f`, and a flat pre-sized vector
// would reserve 128k string headers (32 B each = 4 MB) before the user has produced a
// single line of output. This uses fixed-size pages allocated on first use and recycled
// forever, so memory tracks *actual* output while the address of the oldest line stays
// O(1).

#ifndef OPENCRT_TERMINAL_BUFFER_RING_SCROLLBACK_BUFFER_H_
#define OPENCRT_TERMINAL_BUFFER_RING_SCROLLBACK_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::terminal {

class RingScrollbackBuffer {
public:
    static constexpr std::size_t kDefaultCapacityLines = 128000;
    static constexpr std::size_t kLinesPerPage = 4096;

    RingScrollbackBuffer() : RingScrollbackBuffer(kDefaultCapacityLines) {}
    explicit RingScrollbackBuffer(std::size_t capacity_lines);

    RingScrollbackBuffer(const RingScrollbackBuffer&) = delete;
    RingScrollbackBuffer& operator=(const RingScrollbackBuffer&) = delete;
    RingScrollbackBuffer(RingScrollbackBuffer&&) noexcept = default;
    RingScrollbackBuffer& operator=(RingScrollbackBuffer&&) noexcept = default;

    // Appends a line, overwriting the oldest one once capacity is reached.
    void push(std::string line);
    void push(const char* begin, const char* end);

    [[nodiscard]] std::size_t size() const noexcept { return retained_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_lines_; }
    [[nodiscard]] bool empty() const noexcept { return retained_ == 0; }

    // Total lines ever pushed, including the ones already overwritten. The difference from
    // size() is the number of lines lost, which the UI surfaces when resuming a long session.
    [[nodiscard]] std::uint64_t totalLinesWritten() const noexcept { return total_written_; }
    [[nodiscard]] std::uint64_t droppedLines() const noexcept { return total_written_ - retained_; }

    // index 0 is the oldest retained line, size() - 1 the newest.
    [[nodiscard]] std::string at(std::size_t index_from_oldest) const;
    [[nodiscard]] bool empty_at(std::size_t index_from_oldest) const noexcept {
        return index_from_oldest >= retained_;
    }

    void clear() noexcept;

private:
    // Absolute index of the line at index_from_oldest within the endless write sequence.
    [[nodiscard]] std::uint64_t absoluteIndex(std::size_t index_from_oldest) const noexcept;

    std::size_t capacity_lines_;
    std::size_t page_count_;
    std::uint64_t total_written_ = 0;
    std::size_t retained_ = 0;
    // Outer index is a fixed ring slot; each inner vector allocates its 4096 string headers
    // only when that slot receives its first line, so an idle buffer costs ~1 KB of outer
    // headers instead of 4 MB.
    std::vector<std::vector<std::string>> pages_;
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_BUFFER_RING_SCROLLBACK_BUFFER_H_