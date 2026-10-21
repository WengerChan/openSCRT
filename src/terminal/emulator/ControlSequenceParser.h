// Escape-sequence parser front end (P1-T4).
//
// Feeds raw transport bytes in, emits decoded events out. This is the narrow waist between
// the I/O thread's byte ring and ScreenGrid: it knows the VT grammar but nothing about
// scrolling, colour or rendering.
//
// Design constraints taken from the roadmap:
//  * No per-sequence heap allocation. Params live in a fixed 32-slot array, matching the
//    xterm/xterm-like limit, so a `seq 1 1000000` burst cannot thrash the allocator while
//    the parser thread is already the busiest thread in the process.
//  * Feeding a partial sequence and resuming is lossless: state carries across feed() calls
//    so a control sequence split across two TCP segments still dispatches once.

#ifndef OPENCRT_TERMINAL_EMULATOR_CONTROL_SEQUENCE_PARSER_H_
#define OPENCRT_TERMINAL_EMULATOR_CONTROL_SEQUENCE_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace openscrt::terminal {

// xterm caps a control sequence at 16 parameters in practice; 32 leaves headroom for the
// long SGR / DECSET forms without allocating.
inline constexpr int kMaxControlParams = 32;

// Sentinel stored for a parameter that was written but left empty, e.g. `CSI ;5H`. Parsers
// differ on whether that means 0 or 1 (CUP treats it as 1, ED as 0), so the parser records
// "unspecified" and lets the emulator apply the right default per command.
inline constexpr int kDefaultParam = -1;

// One decoded sequence, as a view over the parser's internal scratch storage. Valid only for
// the duration of the sink call that receives it.
struct CsiSequence {
    std::span<const int> params;
    // Per-parameter sub-index separator: 0 for ';' (or absent), 1 for ':'. Needed because
    // `SGR 38:2::r:g:b` (colon form, ITU-T T.416) differs from `38;2;r;g;b`.
    std::span<const std::uint8_t> sub_params;
    std::span<const std::uint8_t> intermediates;  // 0x20-0x2F
    char final{0};
    // '?', '>', '<' or '=' when the sequence carried one (DECSET/DECRST use `CSI ? 25 h`).
    char private_marker{0};
};

struct OscSequence {
    std::uint32_t identifier{0};  // e.g. 0 = title, 52 = clipboard
    std::string_view payload;     // parameter bytes after the first ';'
};

// Decoded event sink. Implemented by the emulator state machine and by tests.
class IControlSequenceSink {
 public:
    virtual ~IControlSequenceSink() = default;

    // A printable scalar value (UTF-8 already decoded) in the ground state.
    virtual void print(std::uint32_t codepoint) = 0;

    // C0/C1 control such as BEL, BS, HT, LF, CR, FF. Two-byte ESC-prefixed controls reach
    // escapeDispatch instead.
    virtual void execute(std::uint8_t control) = 0;

    // ESC <intermediates> <final>, e.g. ESC ( B (select G0 charset).
    virtual void escapeDispatch(std::span<const std::uint8_t> intermediates, char final) = 0;

    // CSI ... final, covering CUP/ED/EL/SGR/DECSET and friends.
    virtual void csiDispatch(const CsiSequence& sequence) = 0;

    // OSC <identifier> ; <payload> terminated by BEL or ST.
    virtual void oscDispatch(const OscSequence& sequence) = 0;

    // Bytes destined back to the host: DSR/DA/PRIMARY-DA replies. Emitted separately from
    // the sink so the parser never has to know how to reach the transport.
    virtual void deviceResponse(std::string_view response) = 0;
};

class ControlSequenceParser {
 public:
    explicit ControlSequenceParser(IControlSequenceSink& sink) noexcept : sink_(&sink) {}

    ControlSequenceParser(const ControlSequenceParser&) = delete;
    ControlSequenceParser& operator=(const ControlSequenceParser&) = delete;

    void feed(const std::uint8_t* data, std::size_t size);
    void feed(std::string_view text);

    // Drops partial state. Called on session reset so a truncated sequence from the previous
    // connection cannot leak into the new one.
    void reset() noexcept;

    // True when a sequence is mid-flight; used by the integration tests to confirm that a
    // sequence split across feed() boundaries dispatched exactly once. UTF-8 progress counts
    // too, because a half-decoded scalar is equally "not yet a printed glyph".
    [[nodiscard]] bool inSequence() const noexcept {
        return state_ != State::kGround || utf8_remaining_ > 0;
    }

 private:
    enum class State {
        kGround,
        kEscape,
        kCsiParam,
        kCsiIntermediate,
        kCsiIgnore,
        kOscString,
        kOscEscape,     // saw ESC inside OSC; only ESC \ (ST) is valid
        kStringIgnore,  // DCS / APC / PM payloads: consumed and discarded for now
        kStringEscape,
        kUtf8Continuation,
    };

    void step(std::uint8_t byte) noexcept;
    void stepGround(std::uint8_t byte) noexcept;
    void stepEscape(std::uint8_t byte) noexcept;
    void stepCsiParam(std::uint8_t byte) noexcept;
    void stepCsiIntermediate(std::uint8_t byte) noexcept;
    void stepOsc(std::uint8_t byte) noexcept;
    void stepUtf8(std::uint8_t byte) noexcept;

    void startControlSequence(State state) noexcept;
    void finishCsi(char final) noexcept;
    void finishEscape(char final) noexcept;
    void finishOsc() noexcept;
    void finishIgnoredString() noexcept;
    void pushParam() noexcept;
    void emitUtf8Codepoint(std::uint32_t codepoint) noexcept;

    IControlSequenceSink* sink_;
    State state_{State::kGround};

    int params_[kMaxControlParams]{};
    std::uint8_t sub_params_[kMaxControlParams]{};
    int param_count_{0};
    int current_param_{0};
    bool param_digits_seen_{false};
    bool current_is_sub_param_{false};
    char private_marker_{0};

    std::uint8_t intermediates_[4]{};
    int intermediate_count_{0};

    // OSC payload accumulation. Only grows for OSC, which is rare, so the common path stays
    // allocation-free.
    std::string osc_buffer_;
    std::uint32_t osc_identifier_{0};
    bool osc_has_identifier_{false};
    // A garbage stream can emit an unterminated OSC; cap the buffer so one bad remote cannot
    // grow parser memory without bound.
    bool osc_overflow_{false};

    // UTF-8 decoder scratch.
    std::uint32_t utf8_codepoint_{0};
    std::uint32_t utf8_min_codepoint_{0};
    int utf8_remaining_{0};
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_EMULATOR_CONTROL_SEQUENCE_PARSER_H_