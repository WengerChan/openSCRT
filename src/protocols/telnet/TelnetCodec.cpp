#include "protocols/telnet/TelnetCodec.h"

#include <utility>

namespace openscrt::protocols::telnet {
namespace {

constexpr std::uint8_t kIac = 255;
constexpr std::uint8_t kSe = 240;
constexpr std::uint8_t kSb = 250;
constexpr std::uint8_t kWill = 251;
constexpr std::uint8_t kWont = 252;
constexpr std::uint8_t kDo = 253;
constexpr std::uint8_t kDont = 254;

// The two bytes of a window size, most significant first: RFC 1073 says the width
// comes first and both are 16 bits.
void appendWindowSize(std::vector<std::uint8_t>& out, const WindowSize& size) {
    const auto width = static_cast<std::uint16_t>(size.columns);
    const auto height = static_cast<std::uint16_t>(size.rows);
    out.push_back(static_cast<std::uint8_t>((width >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(width & 0xFF));
    out.push_back(static_cast<std::uint8_t>((height >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(height & 0xFF));
}

WindowSize windowSizeFrom(const std::uint8_t* payload) {
    const auto width = static_cast<int>((payload[0] << 8) | payload[1]);
    const auto height = static_cast<int>((payload[2] << 8) | payload[3]);
    return WindowSize{height, width};
}

}  // namespace

void TelnetCodec::setWindowSize(const WindowSize& size) {
    window_size_ = size;
}

void TelnetCodec::setTerminalType(std::string terminal_type) {
    terminal_type_ = std::move(terminal_type);
}

std::vector<std::uint8_t> TelnetCodec::keepAlive() {
    return {kIac, static_cast<std::uint8_t>(TelnetCommand::kNop)};
}

std::vector<std::uint8_t> TelnetCodec::windowSizeReport() const {
    std::vector<std::uint8_t> out;
    out.push_back(kIac);
    out.push_back(kSb);
    out.push_back(static_cast<std::uint8_t>(TelnetOption::kWindowSize));
    appendWindowSize(out, window_size_);
    out.push_back(kIac);
    out.push_back(kSe);
    return out;
}

TelnetCodec::Result TelnetCodec::feed(const std::uint8_t* data, std::size_t size) {
    Result result;
    if (data == nullptr || size == 0) {
        return result;
    }

    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t byte = data[index];

        switch (state_) {
            case State::kData:
                if (byte == kIac) {
                    state_ = State::kIac;
                } else {
                    result.text.push_back(byte);
                }
                break;

            case State::kIac:
                if (byte == kIac) {
                    // IAC IAC is one escaped 255 in the data.
                    result.text.push_back(kIac);
                    state_ = State::kData;
                } else if (byte == kSb) {
                    subnegotiation_.clear();
                    state_ = State::kSubOption;
                } else if (byte == kWill || byte == kWont || byte == kDo || byte == kDont) {
                    state_ = State::kOption;
                    subnegotiation_.clear();
                    subnegotiation_.push_back(byte);
                } else {
                    // Every other command is two bytes and carries nothing.
                    handleCommand(static_cast<TelnetCommand>(byte), 0, result);
                    state_ = State::kData;
                }
                break;

            case State::kOption:
                handleCommand(static_cast<TelnetCommand>(subnegotiation_[0]), byte, result);
                state_ = State::kData;
                break;

            case State::kSubOption:
                subnegotiation_.push_back(byte);
                state_ = State::kSubData;
                break;

            case State::kSubData:
                if (byte == kIac) {
                    state_ = State::kSubIac;
                } else {
                    subnegotiation_.push_back(byte);
                }
                break;

            case State::kSubIac:
                if (byte == kSe) {
                    handleSubnegotiation(subnegotiation_, result);
                    subnegotiation_.clear();
                    state_ = State::kData;
                } else if (byte == kIac) {
                    // An escaped 255 inside a subnegotiation: keep the escape.
                    subnegotiation_.push_back(kIac);
                    state_ = State::kSubData;
                } else {
                    // The escape was something else, so the byte belongs to the
                    // payload and the command goes on.
                    subnegotiation_.push_back(byte);
                    state_ = State::kSubData;
                }
                break;
        }
    }

    return result;
}

void TelnetCodec::answer(TelnetCommand command, TelnetOption option, bool accept, Result& result) {
    result.replies.push_back(kIac);
    result.replies.push_back(static_cast<std::uint8_t>(command));
    result.replies.push_back(static_cast<std::uint8_t>(option));
}

void TelnetCodec::handleCommand(TelnetCommand command, std::uint8_t option, Result& result) {
    switch (command) {
        case TelnetCommand::kDo:
            // The peer asks us to do something. We agree to the options that make
            // sense for a terminal and refuse the rest.
            switch (static_cast<TelnetOption>(option)) {
                case TelnetOption::kWindowSize:
                    if (!window_size_wanted_) {
                        window_size_wanted_ = true;
                        answer(TelnetCommand::kWill, TelnetOption::kWindowSize, true, result);
                        const std::vector<std::uint8_t> report = windowSizeReport();
                        result.replies.insert(result.replies.end(), report.begin(), report.end());
                    }
                    break;
                case TelnetOption::kTermType:
                    if (!terminal_type_wanted_) {
                        terminal_type_wanted_ = true;
                        answer(TelnetCommand::kWill, TelnetOption::kTermType, true, result);
                        result.replies.push_back(kIac);
                        result.replies.push_back(kSb);
                        result.replies.push_back(
                            static_cast<std::uint8_t>(TelnetOption::kTermType));
                        for (const char character : terminal_type_) {
                            result.replies.push_back(static_cast<std::uint8_t>(character));
                        }
                        result.replies.push_back(kIac);
                        result.replies.push_back(kSe);
                    }
                    break;
                case TelnetOption::kSuppressGoAhead:
                    answer(TelnetCommand::kWill, TelnetOption::kSuppressGoAhead, true, result);
                    break;
                default:
                    answer(TelnetCommand::kWont, static_cast<TelnetOption>(option), false, result);
                    break;
            }
            break;

        case TelnetCommand::kDont:
            answer(TelnetCommand::kWont, static_cast<TelnetOption>(option), false, result);
            break;

        case TelnetCommand::kWill:
            // The peer offers to do something. We want the echo to go through so the
            // user sees what they type, and nothing else.
            if (static_cast<TelnetOption>(option) == TelnetOption::kEcho) {
                if (!echo_enabled_) {
                    echo_enabled_ = true;
                    answer(TelnetCommand::kDo, TelnetOption::kEcho, true, result);
                }
            } else {
                answer(TelnetCommand::kDont, static_cast<TelnetOption>(option), false, result);
            }
            break;

        case TelnetCommand::kWont:
            answer(TelnetCommand::kDont, static_cast<TelnetOption>(option), false, result);
            break;

        default:
            // NOP, the data mark, and the rest carry no answer.
            break;
    }
}

void TelnetCodec::handleSubnegotiation(const std::vector<std::uint8_t>& payload, Result& result) {
    // The payload is the option byte followed by whatever the option carries.
    if (payload.size() < 2) {
        return;
    }
    if (static_cast<TelnetOption>(payload[0]) != TelnetOption::kWindowSize) {
        return;
    }
    if (payload.size() < 6) {
        // Four bytes of size, and the option byte in front of them.
        return;
    }
    result.window_size = windowSizeFrom(payload.data() + 1);
    result.window_size_changed = true;
}

}  // namespace openscrt::protocols::telnet
