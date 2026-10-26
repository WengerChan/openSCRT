#include "transfer/modem/ZmodemSniffer.h"

#include <algorithm>

namespace openscrt::transfer::modem {
namespace {

// The six bytes that start a hex frame: the padding, the padding again, the escape,
// the marker of a hex frame, and the type of the frame as two hex digits.
constexpr std::uint8_t kMagic[kFrameMagicLength] = {kZpad, kZpad, kZdle, kZhex, 0, 0};

bool isHexDigit(std::uint8_t byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
           (byte >= 'A' && byte <= 'F');
}

int hexValue(std::uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

// How many bytes at the end of the buffer could still grow into the magic. Only
// those are held back: a buffer that always keeps three bytes would delay the last
// three characters of every chunk, and a prompt would appear late.
std::size_t heldBackLength(const std::vector<std::uint8_t>& buffer) {
    const std::size_t limit = std::min<std::size_t>(buffer.size(), kFrameMagicLength - 1);
    for (std::size_t length = limit; length > 0; --length) {
        bool matches = true;
        for (std::size_t index = 0; index < length; ++index) {
            const std::uint8_t expected = kMagic[index];
            // The last two bytes of the magic are the type, and they are digits: a
            // tail that is not a hex digit cannot become one.
            if (index >= 4 && !isHexDigit(buffer[buffer.size() - length + index])) {
                matches = false;
                break;
            }
            if (index < 4 && expected != buffer[buffer.size() - length + index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return length;
        }
    }
    return 0;
}

}  // namespace

std::string ZmodemSniffer::frameName(ZmodemFrame frame) {
    switch (frame) {
        case ZmodemFrame::kRequestReceiveInit:
            return "the far end wants to send a file";
        case ZmodemFrame::kReceiveInit:
            return "the far end is ready to receive";
        case ZmodemFrame::kFile:
            return "the far end is sending a file";
        case ZmodemFrame::kFinish:
            return "the transfer is over";
        default:
            return "a zmodem frame";
    }
}

void ZmodemSniffer::reset() {
    pending_.clear();
    active_ = false;
}

ZmodemSniffer::Result ZmodemSniffer::feed(const std::uint8_t* data, std::size_t size) {
    Result result;
    if (active_ || data == nullptr || size == 0) {
        // Once a frame was found the whole stream belongs to the engine.
        return result;
    }

    pending_.insert(pending_.end(), data, data + size);

    // Look for the magic.
    for (std::size_t start = 0; start + kFrameMagicLength <= pending_.size(); ++start) {
        if (pending_[start] != kZpad || pending_[start + 1] != kZpad ||
            pending_[start + 2] != kZdle || pending_[start + 3] != kZhex) {
            continue;
        }
        if (!isHexDigit(pending_[start + 4]) || !isHexDigit(pending_[start + 5])) {
            continue;
        }
        result.text.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(start));
        result.zmodem_started = true;
        result.frame = static_cast<ZmodemFrame>((hexValue(pending_[start + 4]) << 4) |
                                                hexValue(pending_[start + 5]));
        result.consumed = start;
        active_ = true;
        pending_.clear();
        return result;
    }

    // No frame yet. The text is everything but the tail that could still grow into
    // the magic.
    const std::size_t held = heldBackLength(pending_);
    const std::size_t text_size = pending_.size() - held;
    result.text.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(text_size));
    result.consumed = text_size;
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(text_size));
    return result;
}

}  // namespace openscrt::transfer::modem
