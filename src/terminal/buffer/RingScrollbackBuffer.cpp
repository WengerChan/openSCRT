#include "terminal/buffer/RingScrollbackBuffer.h"

#include <algorithm>
#include <stdexcept>

namespace openscrt::terminal {

RingScrollbackBuffer::RingScrollbackBuffer(std::size_t capacity_lines)
    : capacity_lines_(std::max<std::size_t>(kLinesPerPage, capacity_lines)) {
    // Round up to a whole number of pages: the ring must wrap on a page boundary, otherwise
    // a line written at the tail of page N and the next line at the head of the recycled
    // page N would need different slot arithmetic.
    page_count_ = (capacity_lines_ + kLinesPerPage - 1) / kLinesPerPage;
    capacity_lines_ = page_count_ * kLinesPerPage;
    pages_.resize(page_count_);
}

void RingScrollbackBuffer::push(std::string line) {
    const std::uint64_t absolute = total_written_;
    const std::size_t page_slot = static_cast<std::size_t>((absolute / kLinesPerPage) %
                                                           static_cast<std::uint64_t>(page_count_));
    const std::size_t line_slot = static_cast<std::size_t>(absolute % kLinesPerPage);

    auto& page = pages_[page_slot];
    if (page.empty()) {
        page.resize(kLinesPerPage);
    }
    page[line_slot] = std::move(line);

    ++total_written_;
    retained_ = std::min(retained_ + 1, capacity_lines_);
}

void RingScrollbackBuffer::push(const char* begin, const char* end) {
    if (begin == nullptr || end == nullptr || end <= begin) {
        return;
    }
    push(std::string(begin, end));
}

std::uint64_t RingScrollbackBuffer::absoluteIndex(std::size_t index_from_oldest) const noexcept {
    return total_written_ - static_cast<std::uint64_t>(retained_) +
           static_cast<std::uint64_t>(index_from_oldest);
}

std::string RingScrollbackBuffer::at(std::size_t index_from_oldest) const {
    if (index_from_oldest >= retained_) {
        throw std::out_of_range("RingScrollbackBuffer::at: index past retained lines");
    }
    const std::uint64_t absolute = absoluteIndex(index_from_oldest);
    const std::size_t page_slot = static_cast<std::size_t>((absolute / kLinesPerPage) %
                                                           static_cast<std::uint64_t>(page_count_));
    const std::size_t line_slot = static_cast<std::size_t>(absolute % kLinesPerPage);

    const auto& page = pages_[page_slot];
    if (page.empty() || line_slot >= page.size()) {
        return {};
    }
    return page[line_slot];
}

void RingScrollbackBuffer::clear() noexcept {
    for (auto& page : pages_) {
        page.clear();
    }
    total_written_ = 0;
    retained_ = 0;
}

}  // namespace openscrt::terminal