#include "terminal/emulator/ControlSequenceParser.h"

#include <algorithm>
#include <cstring>

namespace openscrt::terminal {
namespace {

constexpr std::size_t kMaxOscPayload = 8192;
constexpr std::size_t kMaxIntermediates = 4;
// xterm clamps a CSI parameter at 65535; larger values are malformed.
constexpr int kMaxParamValue = 65535;

bool isDigit(std::uint8_t byte) noexcept {
    return byte >= '0' && byte <= '9';
}
bool isIntermediate(std::uint8_t byte) noexcept {
    return byte >= 0x20 && byte <= 0x2F;
}
bool isCsiFinal(std::uint8_t byte) noexcept {
    return byte >= 0x40 && byte <= 0x7E;
}
// CAN / SUB abort a sequence in progress.
bool isAbort(std::uint8_t byte) noexcept {
    return byte == 0x18 || byte == 0x1A;
}

}  // namespace

void ControlSequenceParser::feed(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < size; ++i) {
        step(data[i]);
    }
}

void ControlSequenceParser::feed(std::string_view text) {
    feed(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void ControlSequenceParser::reset() noexcept {
    state_ = State::kGround;
    param_count_ = 0;
    current_param_ = 0;
    param_digits_seen_ = false;
    current_is_sub_param_ = false;
    private_marker_ = 0;
    intermediate_count_ = 0;
    osc_buffer_.clear();
    osc_identifier_ = 0;
    osc_has_identifier_ = false;
    osc_overflow_ = false;
    utf8_codepoint_ = 0;
    utf8_min_codepoint_ = 0;
    utf8_remaining_ = 0;
}

void ControlSequenceParser::step(std::uint8_t byte) noexcept {
    switch (state_) {
        case State::kGround:
            stepGround(byte);
            break;
        case State::kEscape:
            stepEscape(byte);
            break;
        case State::kCsiParam:
            stepCsiParam(byte);
            break;
        case State::kCsiIntermediate:
            stepCsiIntermediate(byte);
            break;
        case State::kCsiIgnore:
            // Malformed parameter byte: swallow everything up to the final.
            if (isAbort(byte)) {
                state_ = State::kGround;
                param_count_ = 0;
            } else if (byte == 0x1B) {
                startControlSequence(State::kEscape);
            } else if (isCsiFinal(byte)) {
                state_ = State::kGround;
                param_count_ = 0;
            }
            break;
        case State::kOscString:
            stepOsc(byte);
            break;
        case State::kOscEscape:
            if (byte == '\\') {
                finishOsc();
            } else if (byte != 0x1B) {
                state_ = State::kOscString;
                stepOsc(0x1B);
                stepOsc(byte);
            }
            break;
        case State::kStringIgnore:
            if (isAbort(byte)) {
                state_ = State::kGround;
            } else if (byte == 0x1B) {
                state_ = State::kStringEscape;
            }
            break;
        case State::kStringEscape:
            if (byte == '\\') {
                finishIgnoredString();
            } else if (byte != 0x1B) {
                state_ = State::kStringIgnore;
            }
            break;
        case State::kUtf8Continuation:
            // UTF-8 progress is tracked by utf8_remaining_, not by the state machine; kept so
            // the enum stays exhaustive if that is ever refactored.
            stepGround(byte);
            break;
    }
}

void ControlSequenceParser::stepGround(std::uint8_t byte) noexcept {
    // A partially consumed UTF-8 scalar keeps consuming even when the byte itself is not a
    // valid continuation, which is what makes the replacement-char path work.
    if (utf8_remaining_ > 0) {
        stepUtf8(byte);
        return;
    }

    if (byte == 0x1B) {
        startControlSequence(State::kEscape);
        return;
    }
    if (byte < 0x20) {
        sink_->execute(byte);
        return;
    }
    if (byte == 0x7F) {
        return;  // DEL is always discarded.
    }
    if (byte >= 0x80 && byte < 0xA0) {
        // C1 controls are not recognised in UTF-8 mode; they would otherwise be mis-decoded
        // as multi-byte lead bytes and print mojibake.
        return;
    }
    if (byte < 0x80) {
        sink_->print(byte);
        return;
    }

    // UTF-8 lead byte. The 0xC0/0xC1 range is excluded because those forms can only ever
    // encode an overlong ASCII byte.
    if (byte >= 0xC2 && byte <= 0xDF) {
        utf8_codepoint_ = static_cast<std::uint32_t>(byte & 0x1F);
        utf8_remaining_ = 1;
        utf8_min_codepoint_ = 0x80;
    } else if (byte >= 0xE0 && byte <= 0xEF) {
        utf8_codepoint_ = static_cast<std::uint32_t>(byte & 0x0F);
        utf8_remaining_ = 2;
        utf8_min_codepoint_ = 0x800;
    } else if (byte >= 0xF0 && byte <= 0xF4) {
        utf8_codepoint_ = static_cast<std::uint32_t>(byte & 0x07);
        utf8_remaining_ = 3;
        utf8_min_codepoint_ = 0x10000;
    } else {
        emitUtf8Codepoint(0xFFFD);
    }
}

void ControlSequenceParser::stepUtf8(std::uint8_t byte) noexcept {
    if (byte < 0x80 || byte > 0xBF) {
        // Truncated sequence: flush what we had, then re-handle this byte from the ground
        // state so no input is lost or reordered.
        emitUtf8Codepoint(0xFFFD);
        utf8_remaining_ = 0;
        stepGround(byte);
        return;
    }
    utf8_codepoint_ = (utf8_codepoint_ << 6) | static_cast<std::uint32_t>(byte & 0x3F);
    if (--utf8_remaining_ == 0) {
        const bool overlong = utf8_codepoint_ < utf8_min_codepoint_;
        const bool surrogate = utf8_codepoint_ >= 0xD800 && utf8_codepoint_ <= 0xDFFF;
        const bool out_of_range = utf8_codepoint_ > 0x10FFFF;
        emitUtf8Codepoint(overlong || surrogate || out_of_range ? 0xFFFD : utf8_codepoint_);
    }
}

void ControlSequenceParser::emitUtf8Codepoint(std::uint32_t codepoint) noexcept {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        codepoint = 0xFFFD;
    }
    sink_->print(codepoint);
}

void ControlSequenceParser::stepEscape(std::uint8_t byte) noexcept {
    if (isAbort(byte)) {
        state_ = State::kGround;
        return;
    }
    switch (byte) {
        case '[':
            startControlSequence(State::kCsiParam);
            return;
        case ']':
            startControlSequence(State::kOscString);
            return;
        case 'P':  // DCS
        case '^':  // PM
        case '_':  // APC
            startControlSequence(State::kStringIgnore);
            return;
        case 0x1B:
            return;  // ESC ESC: stay, the newer one wins.
        default:
            break;
    }
    if (isIntermediate(byte)) {
        if (static_cast<std::size_t>(intermediate_count_) < kMaxIntermediates) {
            intermediates_[intermediate_count_++] = byte;
        }
        return;
    }
    if (byte >= 0x30 && byte <= 0x7E) {
        finishEscape(static_cast<char>(byte));
        return;
    }
    // Anything else aborts and is re-examined from the ground state.
    state_ = State::kGround;
    intermediate_count_ = 0;
    stepGround(byte);
}

void ControlSequenceParser::stepCsiParam(std::uint8_t byte) noexcept {
    if (isAbort(byte)) {
        state_ = State::kGround;
        param_count_ = 0;
        return;
    }
    if (byte == 0x1B) {
        startControlSequence(State::kEscape);
        return;
    }
    if (isDigit(byte)) {
        param_digits_seen_ = true;
        current_param_ = std::min(current_param_ * 10 + (byte - '0'), kMaxParamValue);
        return;
    }
    if (byte == ';') {
        pushParam();
        return;
    }
    if (byte == ':') {
        pushParam();
        current_is_sub_param_ = true;
        return;
    }
    if (byte >= '<' && byte <= '?') {
        private_marker_ = static_cast<char>(byte);
        return;
    }
    if (isIntermediate(byte)) {
        if (static_cast<std::size_t>(intermediate_count_) < kMaxIntermediates) {
            intermediates_[intermediate_count_++] = byte;
        }
        state_ = State::kCsiIntermediate;
        return;
    }
    if (isCsiFinal(byte)) {
        finishCsi(static_cast<char>(byte));
        return;
    }
    // Out-of-range parameter byte (e.g. '@' before a final, or a stray control): swallow the
    // rest of the sequence rather than emitting it as text.
    state_ = State::kCsiIgnore;
}

void ControlSequenceParser::stepCsiIntermediate(std::uint8_t byte) noexcept {
    if (isAbort(byte)) {
        state_ = State::kGround;
        param_count_ = 0;
        return;
    }
    if (byte == 0x1B) {
        startControlSequence(State::kEscape);
        return;
    }
    if (isIntermediate(byte)) {
        if (static_cast<std::size_t>(intermediate_count_) < kMaxIntermediates) {
            intermediates_[intermediate_count_++] = byte;
        }
        return;
    }
    if (isCsiFinal(byte)) {
        finishCsi(static_cast<char>(byte));
        return;
    }
    state_ = State::kCsiIgnore;
}

void ControlSequenceParser::stepOsc(std::uint8_t byte) noexcept {
    if (osc_overflow_) {
        return;  // Keep discarding until the terminator arrives.
    }
    switch (byte) {
        case 0x07:  // BEL terminator
            finishOsc();
            return;
        case 0x1B:
            state_ = State::kOscEscape;
            return;
        default:
            break;
    }
    if (isAbort(byte)) {
        state_ = State::kGround;
        osc_buffer_.clear();
        return;
    }
    if (osc_buffer_.size() >= kMaxOscPayload) {
        osc_overflow_ = true;
        return;
    }
    osc_buffer_.push_back(static_cast<char>(byte));
}

void ControlSequenceParser::startControlSequence(State state) noexcept {
    state_ = state;
    param_count_ = 0;
    current_param_ = 0;
    param_digits_seen_ = false;
    current_is_sub_param_ = false;
    private_marker_ = 0;
    intermediate_count_ = 0;
    if (state == State::kOscString) {
        osc_buffer_.clear();
        osc_identifier_ = 0;
        osc_has_identifier_ = false;
        osc_overflow_ = false;
    }
}

void ControlSequenceParser::pushParam() noexcept {
    if (param_count_ < kMaxControlParams) {
        params_[param_count_] = param_digits_seen_ ? current_param_ : kDefaultParam;
        sub_params_[param_count_] = current_is_sub_param_ ? 1 : 0;
        ++param_count_;
    }
    // Past kMaxControlParams we keep parsing (so the final byte is still found) but drop the
    // surplus, which is what xterm does.
    current_param_ = 0;
    param_digits_seen_ = false;
    current_is_sub_param_ = false;
}

void ControlSequenceParser::finishCsi(char final) noexcept {
    // A sequence with no separator still carries one (implicit) parameter: `CSI H` is CUP with
    // both columns defaulted.
    if (param_count_ == 0 || param_digits_seen_ || current_is_sub_param_) {
        pushParam();
    }
    CsiSequence sequence{
        std::span<const int>(params_, static_cast<std::size_t>(param_count_)),
        std::span<const std::uint8_t>(sub_params_, static_cast<std::size_t>(param_count_)),
        std::span<const std::uint8_t>(intermediates_,
                                      static_cast<std::size_t>(intermediate_count_)),
        final, private_marker_};
    sink_->csiDispatch(sequence);
    state_ = State::kGround;
}

void ControlSequenceParser::finishEscape(char final) noexcept {
    sink_->escapeDispatch(std::span<const std::uint8_t>(
                              intermediates_, static_cast<std::size_t>(intermediate_count_)),
                          final);
    state_ = State::kGround;
    intermediate_count_ = 0;
}

void ControlSequenceParser::finishIgnoredString() noexcept {
    // DCS / APC / PM payloads are consumed but not yet interpreted (Phase 1 has no use for
    // them). The important behaviour is that their bytes never reach the screen as text.
    state_ = State::kGround;
    param_count_ = 0;
    intermediate_count_ = 0;
}

void ControlSequenceParser::finishOsc() noexcept {
    const std::string_view buffer(osc_buffer_);
    std::size_t offset = 0;
    std::uint32_t identifier = 0;
    bool has_identifier = false;

    if (!buffer.empty() && isDigit(static_cast<std::uint8_t>(buffer.front()))) {
        has_identifier = true;
        while (offset < buffer.size() && isDigit(static_cast<std::uint8_t>(buffer[offset]))) {
            identifier = identifier * 10 + static_cast<std::uint32_t>(buffer[offset] - '0');
            ++offset;
        }
        if (offset < buffer.size() && buffer[offset] == ';') {
            ++offset;
        }
    } else if (!buffer.empty() && buffer.front() == ';') {
        // `OSC ; text` is the degenerate form: identifier defaults to 0.
        offset = 1;
    }

    osc_identifier_ = identifier;
    osc_has_identifier_ = has_identifier;
    const OscSequence sequence{identifier, buffer.substr(offset)};
    sink_->oscDispatch(sequence);
    state_ = State::kGround;
    osc_buffer_.clear();
}

}  // namespace openscrt::terminal