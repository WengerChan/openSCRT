#include "protocols/serial/SerialSettings.h"

namespace openscrt::protocols::serial {
namespace {

// The text of a value is its name; the parsers compare against the same strings, so
// the two cannot drift apart.
bool matches(std::string_view text, std::string_view expected) {
    return text == expected;
}

}  // namespace

const char* toString(SerialParity parity) noexcept {
    switch (parity) {
        case SerialParity::kNone:
            return "none";
        case SerialParity::kEven:
            return "even";
        case SerialParity::kOdd:
            return "odd";
        case SerialParity::kMark:
            return "mark";
        case SerialParity::kSpace:
            return "space";
    }
    return "none";
}

const char* toString(SerialDataBits bits) noexcept {
    switch (bits) {
        case SerialDataBits::k5:
            return "5";
        case SerialDataBits::k6:
            return "6";
        case SerialDataBits::k7:
            return "7";
        case SerialDataBits::k8:
            return "8";
    }
    return "8";
}

const char* toString(SerialStopBits bits) noexcept {
    switch (bits) {
        case SerialStopBits::kOne:
            return "1";
        case SerialStopBits::kOneAndHalf:
            return "1.5";
        case SerialStopBits::kTwo:
            return "2";
    }
    return "1";
}

const char* toString(SerialFlowControl flow) noexcept {
    switch (flow) {
        case SerialFlowControl::kNone:
            return "none";
        case SerialFlowControl::kRtsCts:
            return "rts-cts";
        case SerialFlowControl::kXonXoff:
            return "xon-xoff";
    }
    return "none";
}

std::optional<SerialParity> serialParityFromString(std::string_view text) noexcept {
    if (matches(text, "none")) {
        return SerialParity::kNone;
    }
    if (matches(text, "even")) {
        return SerialParity::kEven;
    }
    if (matches(text, "odd")) {
        return SerialParity::kOdd;
    }
    if (matches(text, "mark")) {
        return SerialParity::kMark;
    }
    if (matches(text, "space")) {
        return SerialParity::kSpace;
    }
    return std::nullopt;
}

std::optional<SerialDataBits> serialDataBitsFromString(std::string_view text) noexcept {
    if (matches(text, "5")) {
        return SerialDataBits::k5;
    }
    if (matches(text, "6")) {
        return SerialDataBits::k6;
    }
    if (matches(text, "7")) {
        return SerialDataBits::k7;
    }
    if (matches(text, "8")) {
        return SerialDataBits::k8;
    }
    return std::nullopt;
}

std::optional<SerialStopBits> serialStopBitsFromString(std::string_view text) noexcept {
    if (matches(text, "1")) {
        return SerialStopBits::kOne;
    }
    if (matches(text, "1.5")) {
        return SerialStopBits::kOneAndHalf;
    }
    if (matches(text, "2")) {
        return SerialStopBits::kTwo;
    }
    return std::nullopt;
}

std::optional<SerialFlowControl> serialFlowControlFromString(std::string_view text) noexcept {
    if (matches(text, "none")) {
        return SerialFlowControl::kNone;
    }
    if (matches(text, "rts-cts")) {
        return SerialFlowControl::kRtsCts;
    }
    if (matches(text, "xon-xoff")) {
        return SerialFlowControl::kXonXoff;
    }
    return std::nullopt;
}

bool SerialSettings::valid() const noexcept {
    return validationError().empty();
}

std::string SerialSettings::validationError() const {
    if (port_name.empty()) {
        return "the serial port has no name";
    }
    if (baud_rate <= 0) {
        return "the baud rate must be a positive number of bits per second";
    }
    return {};
}

}  // namespace openscrt::protocols::serial
