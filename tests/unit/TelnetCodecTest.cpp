// The Telnet state machine (P3-T3).
//
// The codec is pure logic over bytes, so every negotiation is written down here as a
// list of bytes and checked. No server, no socket, no display.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "protocols/telnet/TelnetCodec.h"

namespace telnet = openscrt::protocols::telnet;

namespace {

constexpr std::uint8_t kIac = 255;
constexpr std::uint8_t kSe = 240;
constexpr std::uint8_t kSb = 250;
constexpr std::uint8_t kWill = 251;
constexpr std::uint8_t kWont = 252;
constexpr std::uint8_t kDo = 253;
constexpr std::uint8_t kDont = 254;
constexpr std::uint8_t kNop = 241;

constexpr std::uint8_t kEcho = 1;
constexpr std::uint8_t kSuppressGoAhead = 3;
constexpr std::uint8_t kTermType = 24;
constexpr std::uint8_t kWindowSize = 31;

telnet::TelnetCodec::Result feed(telnet::TelnetCodec& codec,
                                 const std::vector<std::uint8_t>& bytes) {
    return codec.feed(bytes.data(), bytes.size());
}

std::string textOf(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST(TelnetCodecTest, PlainTextGoesStraightThrough) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {'h', 'e', 'l', 'l', 'o'});
    EXPECT_EQ(textOf(result.text), "hello");
    EXPECT_TRUE(result.replies.empty());
}

TEST(TelnetCodecTest, AnEscapedEscapeIsOneByteOfText) {
    // IAC IAC is one 255 in the data: the escape is escaped.
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {'A', kIac, kIac, 'B'});
    ASSERT_EQ(result.text.size(), 3u);
    EXPECT_EQ(result.text[0], 'A');
    EXPECT_EQ(result.text[1], kIac);
    EXPECT_EQ(result.text[2], 'B');
    EXPECT_TRUE(result.replies.empty());
}

TEST(TelnetCodecTest, ThePeerAsksForTheWindowSizeAndWeSayYes) {
    telnet::TelnetCodec codec;
    codec.setWindowSize(openscrt::protocols::WindowSize{40, 120});
    const auto result = feed(codec, {kIac, kDo, kWindowSize});

    // IAC WILL NAWS, and then the size we have: the agreement and the size are two
    // separate things, and the size only makes sense after the agreement.
    ASSERT_GE(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kWill);
    EXPECT_EQ(result.replies[2], kWindowSize);

    std::vector<std::uint8_t> expected = {kIac, kWill, kWindowSize};
    const std::vector<std::uint8_t> report = codec.windowSizeReport();
    expected.insert(expected.end(), report.begin(), report.end());
    EXPECT_EQ(textOf(result.replies), textOf(expected));
}

TEST(TelnetCodecTest, ThePeerRefusesTheWindowSizeAndWeRefuseItBack) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kDont, kWindowSize});
    ASSERT_EQ(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kWont);
    EXPECT_EQ(result.replies[2], kWindowSize);
}

TEST(TelnetCodecTest, ThePeerWantsTheEchoAndWeLetIt) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kWill, kEcho});
    ASSERT_EQ(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kDo);
    EXPECT_EQ(result.replies[2], kEcho);
}

TEST(TelnetCodecTest, ThePeerOffersSomethingWeDoNotWant) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kWill, kWindowSize});
    ASSERT_EQ(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kDont);
    EXPECT_EQ(result.replies[2], kWindowSize);
}

TEST(TelnetCodecTest, AnOptionWeDoNotKnowIsRefused) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kDo, 99});
    ASSERT_EQ(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kWont);
    EXPECT_EQ(result.replies[2], 99);
}

TEST(TelnetCodecTest, ThePeerAsksForTheTerminalTypeAndWeNameOurselves) {
    telnet::TelnetCodec codec;
    codec.setTerminalType("xterm-256color");
    const auto result = feed(codec, {kIac, kDo, kTermType});

    ASSERT_GE(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[0], kIac);
    EXPECT_EQ(result.replies[1], kWill);
    EXPECT_EQ(result.replies[2], kTermType);
    // And then the name itself, in a subnegotiation.
    const std::string replies = textOf(result.replies);
    EXPECT_NE(replies.find("xterm-256color"), std::string::npos);
}

TEST(TelnetCodecTest, ThePeerTellsUsHowBigItsWindowIs) {
    telnet::TelnetCodec codec;
    // IAC SB NAWS <columns> <rows> IAC SE. The width comes first: that is RFC 1073.
    const auto result = feed(codec, {kIac, kSb, kWindowSize, 0x00, 0x50, 0x00, 0x28, kIac, kSe});

    EXPECT_TRUE(result.window_size_changed);
    EXPECT_EQ(result.window_size.columns, 80);
    EXPECT_EQ(result.window_size.rows, 40);
    EXPECT_TRUE(result.text.empty()) << "a negotiation is not text";
}

TEST(TelnetCodecTest, ACommandSplitOverTwoReadsIsStillOneCommand) {
    // The escape arrives first and the command a moment later: the state machine has
    // to remember where it was.
    telnet::TelnetCodec codec;
    const auto first = feed(codec, {kIac});
    EXPECT_TRUE(first.replies.empty());
    EXPECT_TRUE(first.text.empty());

    const auto second = feed(codec, {kDo, kWindowSize});
    ASSERT_GE(second.replies.size(), 3u);
    EXPECT_EQ(second.replies[0], kIac);
    EXPECT_EQ(second.replies[1], kWill);
    EXPECT_EQ(second.replies[2], kWindowSize);
}

TEST(TelnetCodecTest, AKeepAliveIsAnsweredWithNothing) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kNop});
    EXPECT_TRUE(result.text.empty());
    EXPECT_TRUE(result.replies.empty());
}

TEST(TelnetCodecTest, TheKeepAliveIsTwoBytes) {
    const std::vector<std::uint8_t> probe = telnet::TelnetCodec::keepAlive();
    ASSERT_EQ(probe.size(), 2u);
    EXPECT_EQ(probe[0], kIac);
    EXPECT_EQ(probe[1], kNop);
}

TEST(TelnetCodecTest, ASubnegotiationWeDoNotKnowIsIgnored) {
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kSb, 99, 'x', 'y', kIac, kSe});
    EXPECT_FALSE(result.window_size_changed);
    EXPECT_TRUE(result.text.empty());
    EXPECT_TRUE(result.replies.empty());
}

TEST(TelnetCodecTest, TheSuppressGoAheadOptionIsAccepted) {
    // It is the option that stops the server from waiting for the other side to say
    // "go ahead", and a terminal wants it.
    telnet::TelnetCodec codec;
    const auto result = feed(codec, {kIac, kDo, kSuppressGoAhead});
    ASSERT_EQ(result.replies.size(), 3u);
    EXPECT_EQ(result.replies[1], kWill);
    EXPECT_EQ(result.replies[2], kSuppressGoAhead);
}

TEST(TelnetCodecTest, TheCodecSaysWhenItIsInTheMiddleOfACommand) {
    telnet::TelnetCodec codec;
    EXPECT_FALSE(codec.midCommand());
    feed(codec, {kIac});
    EXPECT_TRUE(codec.midCommand());
    feed(codec, {kNop});
    EXPECT_FALSE(codec.midCommand());
}
