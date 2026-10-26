// The Zmodem sniffer (P3-T6).
//
// The sniffer is what keeps the binary of a transfer off the screen, so the test is
// about two things: the text that comes before a frame is still text, and the frame
// is found even when it arrives in pieces.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "transfer/modem/ZmodemSniffer.h"

namespace modem = openscrt::transfer::modem;

namespace {

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

std::string textOf(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// The six bytes that start a hex frame, with the type as two hex digits.
std::vector<std::uint8_t> frame(int type) {
    const char* digits = "0123456789abcdef";
    return {'*',
            '*',
            0x18,
            'B',
            static_cast<std::uint8_t>(digits[(type >> 4) & 0xF]),
            static_cast<std::uint8_t>(digits[type & 0xF])};
}

}  // namespace

TEST(ZmodemSnifferTest, TextGoesStraightThrough) {
    modem::ZmodemSniffer sniffer;
    const auto result = sniffer.feed(bytesOf("hello").data(), 5);
    EXPECT_EQ(textOf(result.text), "hello");
    EXPECT_FALSE(result.zmodem_started);
}

TEST(ZmodemSnifferTest, TheRequestToSendIsFound) {
    modem::ZmodemSniffer sniffer;
    const std::vector<std::uint8_t> magic = frame(0);  // ZRQINIT
    const auto result = sniffer.feed(magic.data(), magic.size());
    EXPECT_TRUE(result.zmodem_started);
    EXPECT_EQ(result.frame, modem::ZmodemFrame::kRequestReceiveInit);
    EXPECT_TRUE(sniffer.active());
}

TEST(ZmodemSnifferTest, TheAnswerIsFound) {
    modem::ZmodemSniffer sniffer;
    const std::vector<std::uint8_t> magic = frame(1);  // ZRINIT
    const auto result = sniffer.feed(magic.data(), magic.size());
    EXPECT_TRUE(result.zmodem_started);
    EXPECT_EQ(result.frame, modem::ZmodemFrame::kReceiveInit);
}

TEST(ZmodemSnifferTest, TheTextBeforeTheFrameStaysText) {
    modem::ZmodemSniffer sniffer;
    std::vector<std::uint8_t> payload = bytesOf("sz file\r\n");
    const std::vector<std::uint8_t> magic = frame(0);
    payload.insert(payload.end(), magic.begin(), magic.end());

    const auto result = sniffer.feed(payload.data(), payload.size());
    EXPECT_TRUE(result.zmodem_started);
    EXPECT_EQ(textOf(result.text), "sz file\r\n");
}

TEST(ZmodemSnifferTest, AFrameThatArrivesInPiecesIsStillFound) {
    modem::ZmodemSniffer sniffer;
    const std::vector<std::uint8_t> magic = frame(0);

    // Two bytes, then the rest. The sniffer has to remember that it is in the middle
    // of a frame.
    const auto first = sniffer.feed(magic.data(), 2);
    EXPECT_FALSE(first.zmodem_started);
    EXPECT_TRUE(first.text.empty()) << "the bytes that could be a frame are held back";

    const auto second = sniffer.feed(magic.data() + 2, magic.size() - 2);
    EXPECT_TRUE(second.zmodem_started);
    EXPECT_EQ(second.frame, modem::ZmodemFrame::kRequestReceiveInit);
}

TEST(ZmodemSnifferTest, TheTailThatCouldBeAFrameIsHeldBack) {
    modem::ZmodemSniffer sniffer;
    // The last byte could be the start of the magic, so it is not shown yet.
    const auto result = sniffer.feed(bytesOf("ab*").data(), 3);
    EXPECT_EQ(textOf(result.text), "ab");

    // And when the next bytes turn out not to be a frame, it comes out.
    const auto second = sniffer.feed(bytesOf("c").data(), 1);
    EXPECT_EQ(textOf(second.text), "*c");
    EXPECT_FALSE(second.zmodem_started);
}

TEST(ZmodemSnifferTest, AfterAFrameTheSnifferDoesNothing) {
    modem::ZmodemSniffer sniffer;
    const std::vector<std::uint8_t> magic = frame(0);
    sniffer.feed(magic.data(), magic.size());

    // The rest of the transfer is binary, and none of it is text.
    const std::vector<std::uint8_t> binary = {0x00, 0xFF, 0x18, 0x42, 0x7F};
    const auto result = sniffer.feed(binary.data(), binary.size());
    EXPECT_TRUE(result.text.empty());
    EXPECT_FALSE(result.zmodem_started) << "the transfer is already under way";
}

TEST(ZmodemSnifferTest, TheSnifferCanBePutBackToWork) {
    modem::ZmodemSniffer sniffer;
    const std::vector<std::uint8_t> magic = frame(0);
    sniffer.feed(magic.data(), magic.size());
    ASSERT_TRUE(sniffer.active());

    sniffer.reset();
    EXPECT_FALSE(sniffer.active());
    const auto result = sniffer.feed(bytesOf("after").data(), 5);
    EXPECT_EQ(textOf(result.text), "after");
}
