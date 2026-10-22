// P1-T4: escape-sequence grammar tests.
//
// These are the contract the rest of the emulator depends on: if a sequence is dropped,
// split or double-dispatched here, the visible symptom is "vim renders wrong" and the bug is
// enormously expensive to find. The split-across-feed() cases matter most in production,
// because TCP segments almost never align with control-sequence boundaries.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "terminal/emulator/ControlSequenceParser.h"

namespace openscrt::terminal {
namespace {

struct RecordedEvent {
    enum class Kind { kPrint, kExecute, kEscape, kCsi, kOsc, kResponse };
    Kind kind{Kind::kPrint};
    std::uint32_t value{0};                   // codepoint / control byte
    std::string payload;                      // OSC payload or device response
    std::vector<int> params;                  // CSI parameters
    std::vector<std::uint8_t> sub_params;     // CSI ':' flags
    std::vector<std::uint8_t> intermediates;  // CSI / ESC intermediates
    char final{0};
    char private_marker{0};
};

class RecordingSink : public IControlSequenceSink {
 public:
    std::vector<RecordedEvent> events;

    void print(std::uint32_t codepoint) override { add(RecordedEvent::Kind::kPrint, codepoint); }

    void execute(std::uint8_t control) override { add(RecordedEvent::Kind::kExecute, control); }

    void escapeDispatch(std::span<const std::uint8_t> intermediates, char final) override {
        RecordedEvent event;
        event.kind = RecordedEvent::Kind::kEscape;
        event.intermediates.assign(intermediates.begin(), intermediates.end());
        event.final = final;
        events.push_back(std::move(event));
    }

    void csiDispatch(const CsiSequence& sequence) override {
        RecordedEvent event;
        event.kind = RecordedEvent::Kind::kCsi;
        event.params.assign(sequence.params.begin(), sequence.params.end());
        event.sub_params.assign(sequence.sub_params.begin(), sequence.sub_params.end());
        event.intermediates.assign(sequence.intermediates.begin(), sequence.intermediates.end());
        event.final = sequence.final;
        event.private_marker = sequence.private_marker;
        events.push_back(std::move(event));
    }

    void oscDispatch(const OscSequence& sequence) override {
        RecordedEvent event;
        event.kind = RecordedEvent::Kind::kOsc;
        event.value = sequence.identifier;
        event.payload = std::string(sequence.payload);
        events.push_back(std::move(event));
    }

    void deviceResponse(std::string_view response) override {
        RecordedEvent event;
        event.kind = RecordedEvent::Kind::kResponse;
        event.payload = std::string(response);
        events.push_back(std::move(event));
    }

    [[nodiscard]] std::size_t count() const { return events.size(); }

 private:
    void add(RecordedEvent::Kind kind, std::uint32_t value) {
        RecordedEvent event;
        event.kind = kind;
        event.value = value;
        events.push_back(std::move(event));
    }
};

class ParserTest : public ::testing::Test {
 protected:
    RecordingSink sink;
    ControlSequenceParser parser{sink};

    void feed(std::string_view text) { parser.feed(text); }

    [[nodiscard]] const RecordedEvent& only() const {
        EXPECT_EQ(sink.events.size(), 1u) << "expected exactly one dispatched event";
        return sink.events.front();
    }
};

// ----------------------------------------------------------------------------------- text

TEST_F(ParserTest, PlainAsciiPrintsCharacterByCharacter) {
    feed("ab");
    ASSERT_EQ(sink.count(), 2u);
    EXPECT_EQ(sink.events[0].kind, RecordedEvent::Kind::kPrint);
    EXPECT_EQ(sink.events[0].value, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(sink.events[1].value, static_cast<std::uint32_t>('b'));
}

TEST_F(ParserTest, DelIsDiscarded) {
    feed(
        "a\x7F"
        "b");
    ASSERT_EQ(sink.count(), 2u);
    EXPECT_EQ(sink.events[0].value, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(sink.events[1].value, static_cast<std::uint32_t>('b'));
}

TEST_F(ParserTest, ControlCharactersExecute) {
    feed("a\r\n\t\x07");
    ASSERT_EQ(sink.count(), 5u);
    EXPECT_EQ(sink.events[1].kind, RecordedEvent::Kind::kExecute);
    EXPECT_EQ(sink.events[1].value, '\r');
    EXPECT_EQ(sink.events[2].value, '\n');
    EXPECT_EQ(sink.events[3].value, '\t');
    EXPECT_EQ(sink.events[4].value, 0x07);
}

// ----------------------------------------------------------------------------------- utf-8

TEST_F(ParserTest, DecodesTwoThreeAndFourByteUtf8) {
    feed("\xC3\xA9");          // U+00E9
    feed("\xE4\xB8\xAD");      // U+4E2D
    feed("\xF0\x9F\x98\x80");  // U+1F600
    ASSERT_EQ(sink.count(), 3u);
    EXPECT_EQ(sink.events[0].value, 0x00E9u);
    EXPECT_EQ(sink.events[1].value, 0x4E2Du);
    EXPECT_EQ(sink.events[2].value, 0x1F600u);
}

TEST_F(ParserTest, RejectsOverlongEncoding) {
    // C0 80 is an overlong NUL; must not decode to 0.
    feed("\xC0\x80");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.events[0].value, 0xFFFDu);
}

TEST_F(ParserTest, RejectsSurrogateHalf) {
    feed("\xED\xA0\x80");  // U+D800
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.events[0].value, 0xFFFDu);
}

TEST_F(ParserTest, RejectsInvalidLeadByte) {
    feed("\xFF");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.events[0].value, 0xFFFDu);
}

TEST_F(ParserTest, TruncatedUtf8FlushesAndResumes) {
    // A 3-byte lead followed by ASCII: emit one replacement char, then print the ASCII.
    feed(
        "a\xE4\xB8"
        "b");
    ASSERT_EQ(sink.count(), 3u);
    EXPECT_EQ(sink.events[0].value, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(sink.events[1].value, 0xFFFDu);
    EXPECT_EQ(sink.events[2].value, static_cast<std::uint32_t>('b'));
}

TEST_F(ParserTest, Utf8SplitAcrossFeedsIsReassembled) {
    feed("\xE4\xB8");
    EXPECT_EQ(sink.count(), 0u);
    EXPECT_TRUE(parser.inSequence());
    feed("\xAD");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.events[0].value, 0x4E2Du);
}

// ------------------------------------------------------------------------------ sequences

TEST_F(ParserTest, EmptyCsiHasOneDefaultedParameter) {
    feed("\x1B[H");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kCsi);
    EXPECT_EQ(only().final, 'H');
    ASSERT_EQ(only().params.size(), 1u);
    EXPECT_EQ(only().params[0], kDefaultParam);
}

TEST_F(ParserTest, ParsesMultipleParameters) {
    feed("\x1B[2;31m");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().final, 'm');
    ASSERT_EQ(only().params.size(), 2u);
    EXPECT_EQ(only().params[0], 2);
    EXPECT_EQ(only().params[1], 31);
    EXPECT_EQ(only().sub_params[0], 0u);
    EXPECT_EQ(only().sub_params[1], 0u);
}

TEST_F(ParserTest, EmptyParameterBecomesDefaultNotZero) {
    // `CSI ;5H` means row=default, col=5 -- not row=0.
    feed("\x1B[;5H");
    ASSERT_EQ(sink.count(), 1u);
    ASSERT_EQ(only().params.size(), 2u);
    EXPECT_EQ(only().params[0], kDefaultParam);
    EXPECT_EQ(only().params[1], 5);
}

TEST_F(ParserTest, ParsesColonSubParametersForTrueColorSgr) {
    feed("\x1B[38:2::255:128:0m");
    ASSERT_EQ(sink.count(), 1u);
    const auto& event = only();
    EXPECT_EQ(event.final, 'm');
    ASSERT_EQ(event.params.size(), 6u);
    EXPECT_EQ(event.params[0], 38);
    EXPECT_EQ(event.params[1], 2);
    EXPECT_EQ(event.params[2], kDefaultParam) << "the doubled colon is an omitted sub-param";
    EXPECT_EQ(event.params[3], 255);
    EXPECT_EQ(event.params[4], 128);
    EXPECT_EQ(event.params[5], 0);
    EXPECT_EQ(event.sub_params[0], 0u);
    EXPECT_EQ(event.sub_params[1], 1u);
}

TEST_F(ParserTest, CapturesPrivateMarkerForDecset) {
    feed("\x1B[?25l");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().private_marker, '?');
    EXPECT_EQ(only().final, 'l');
    ASSERT_EQ(only().params.size(), 1u);
    EXPECT_EQ(only().params[0], 25);
}

TEST_F(ParserTest, CapturesIntermediateBytes) {
    feed("\x1B[4 q");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().final, 'q');
    ASSERT_EQ(only().intermediates.size(), 1u);
    EXPECT_EQ(only().intermediates[0], static_cast<std::uint8_t>(' '));
}

TEST_F(ParserTest, ClampsOversizedParameter) {
    feed("\x1B[99999m");
    ASSERT_EQ(sink.count(), 1u);
    ASSERT_EQ(only().params.size(), 1u);
    EXPECT_EQ(only().params[0], 65535);
}

TEST_F(ParserTest, DropsParametersBeyondTheLimit) {
    std::string sequence = "\x1B[";
    for (int i = 0; i < 40; ++i) {
        sequence += (i == 0) ? "1;" : "1;";
    }
    sequence += "1m";
    feed(sequence);
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().final, 'm');
    EXPECT_EQ(only().params.size(), static_cast<std::size_t>(kMaxControlParams));
}

TEST_F(ParserTest, EscapeDispatchWithCharsetIntermediate) {
    feed("\x1B(B");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kEscape);
    EXPECT_EQ(only().final, 'B');
    ASSERT_EQ(only().intermediates.size(), 1u);
    EXPECT_EQ(only().intermediates[0], static_cast<std::uint8_t>('('));
}

TEST_F(ParserTest, BareEscapeDispatch) {
    // "\x1B" "7" rather than "\x1B7": C++ consumes as many hex digits as it can, so \x1B7 is
    // the out-of-range escape 0x1B7, not ESC followed by '7'.
    feed(
        "\x1B"
        "7");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kEscape);
    EXPECT_EQ(only().final, '7');
    EXPECT_TRUE(only().intermediates.empty());
}

// ----------------------------------------------------------------------------------- osc

TEST_F(ParserTest, OscTerminatedByBel) {
    feed("\x1B]0;my title\x07");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kOsc);
    EXPECT_EQ(only().value, 0u);
    EXPECT_EQ(only().payload, "my title");
}

TEST_F(ParserTest, OscTerminatedByStringTerminator) {
    feed("\x1B]52;c;BASE64DATA\x1B\\");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kOsc);
    EXPECT_EQ(only().value, 52u);
    EXPECT_EQ(only().payload, "c;BASE64DATA");
}

TEST_F(ParserTest, OscWithMultiDigitIdentifier) {
    feed("\x1B]1337;notify\x07");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().value, 1337u);
    EXPECT_EQ(only().payload, "notify");
}

TEST_F(ParserTest, OscWithEmptyPayload) {
    feed("\x1B]0;\x07");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().payload, "");
}

TEST_F(ParserTest, OscEscEscIsNotATerminator) {
    // ESC ESC inside an OSC: the first ESC starts a possible ST, the second cancels it and
    // re-arms. Neither reaches the payload.
    feed("\x1B]0;a\x1B\x1B\\");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().payload, "a");
}

TEST_F(ParserTest, DcsPayloadNeverReachesTheScreen) {
    feed(
        "\x1B"
        "P1;2q"
        "hidden data"
        "\x1B\\"
        "visible");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kPrint);
    EXPECT_EQ(only().value, static_cast<std::uint32_t>('v'));
}

// -------------------------------------------------------------------------------- aborts

TEST_F(ParserTest, CanAbortsIncompleteSequence) {
    feed(
        "a\x1B[31\x18"
        "b");
    ASSERT_EQ(sink.count(), 2u);
    EXPECT_EQ(sink.events[0].value, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(sink.events[1].value, static_cast<std::uint32_t>('b'));
}

TEST_F(ParserTest, EscEscRestartsTheSequence) {
    feed("\x1B\x1B[1m");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kCsi);
    EXPECT_EQ(only().final, 'm');
}

TEST_F(ParserTest, ResetDropsPartialState) {
    feed("\x1B[31");
    EXPECT_TRUE(parser.inSequence());
    parser.reset();
    EXPECT_FALSE(parser.inSequence());
    feed("m");
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kPrint);
    EXPECT_EQ(only().value, static_cast<std::uint32_t>('m'));
}

// -------------------------------------------------------------------------- split feeds

TEST_F(ParserTest, CsiSplitAcrossThreeFeedsDispatchesExactlyOnce) {
    feed("\x1B");
    EXPECT_TRUE(parser.inSequence());
    feed("[");
    EXPECT_TRUE(parser.inSequence());
    feed("2;5H");
    EXPECT_FALSE(parser.inSequence());
    ASSERT_EQ(sink.count(), 1u);
    ASSERT_EQ(only().params.size(), 2u);
    EXPECT_EQ(only().params[0], 2);
    EXPECT_EQ(only().params[1], 5);
    EXPECT_EQ(only().final, 'H');
}

TEST_F(ParserTest, TextAndSequencesInterleaveAcrossFeeds) {
    // Byte-for-byte feed is the pathological case: every single byte is its own feed call.
    const std::string stream = "ls -la\r\n\x1B[?25l\x1B[0m$\xE4\xB8\xAD\x07";
    for (const char byte : stream) {
        feed(std::string_view(&byte, 1));
    }

    bool sawPrivateMarker = false;
    bool sawBel = false;
    bool sawCjk = false;
    for (const auto& event : sink.events) {
        if (event.kind == RecordedEvent::Kind::kCsi && event.private_marker == '?') {
            sawPrivateMarker = true;
        }
        if (event.kind == RecordedEvent::Kind::kExecute && event.value == 0x07) {
            sawBel = true;
        }
        if (event.kind == RecordedEvent::Kind::kPrint && event.value == 0x4E2D) {
            sawCjk = true;
        }
    }
    EXPECT_TRUE(sawPrivateMarker);
    EXPECT_TRUE(sawBel);
    EXPECT_TRUE(sawCjk);
}

TEST_F(ParserTest, NullFeedIsIgnored) {
    parser.feed(nullptr, 10);
    EXPECT_EQ(sink.count(), 0u);
}

TEST_F(ParserTest, OscOverflowIsDiscardedUntilTerminator) {
    std::string stream = "\x1B]0;";
    stream.append(20000, 'x');
    stream.push_back('\x07');
    feed(stream);
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(only().kind, RecordedEvent::Kind::kOsc);
    EXPECT_LT(only().payload.size(), 20000u) << "the buffer must stay bounded";
}

}  // namespace
}  // namespace openscrt::terminal