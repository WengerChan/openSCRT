// The hex monitor of a serial line (P3-T2).
//
// The view is a widget, so it needs the offscreen platform to be tested at all. What is
// checked here is what the two panes show: the bytes as hex on one side, the same
// bytes as characters on the other.

#include <gtest/gtest.h>

#include <QApplication>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ui/workspace/SerialHexView.h"

namespace ui = openscrt::ui;

namespace {

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

void appendText(ui::SerialHexView& view, std::string_view text) {
    const std::vector<std::uint8_t> bytes = bytesOf(text);
    view.append(bytes.data(), bytes.size());
}

}  // namespace

TEST(SerialHexViewTest, TheBytesShowUpAsHexAndAsCharacters) {
    ui::SerialHexView view;
    appendText(view, "Hello");

    EXPECT_EQ(view.hexText(), "0000  48 65 6c 6c 6f\n");
    EXPECT_EQ(view.asciiText(), "Hello\n");
    EXPECT_EQ(view.byteCount(), 5u);
}

TEST(SerialHexViewTest, AByteThatIsNotTextShowsAsADotOnTheRight) {
    ui::SerialHexView view;
    const std::vector<std::uint8_t> payload = {0x41, 0x00, 0x1B, 0xFF};
    view.append(payload.data(), payload.size());

    // The hex side says what the bytes are, the character side says what they mean.
    EXPECT_EQ(view.hexText(), "0000  41 00 1b ff\n");
    EXPECT_EQ(view.asciiText(), "A...\n");
}

TEST(SerialHexViewTest, TheSecondLineStartsAtSixteen) {
    ui::SerialHexView view;
    const std::vector<std::uint8_t> payload(17, 'x');
    view.append(payload.data(), payload.size());

    const std::string hex = view.hexText();
    EXPECT_NE(hex.find("0000  "), std::string::npos);
    EXPECT_NE(hex.find("0010  "), std::string::npos);
}

TEST(SerialHexViewTest, TheWindowKeepsTheMostRecentBytes) {
    // A window of four bytes: three are appended, then two more, and the oldest falls
    // out. A monitor that grows without bound would eat the memory it is watching.
    ui::SerialHexView view(4);
    appendText(view, "abc");
    appendText(view, "de");

    EXPECT_EQ(view.byteCount(), 4u);
    EXPECT_EQ(view.asciiText(), "bcde\n");
}

TEST(SerialHexViewTest, ClearingEmptiesBothPanes) {
    ui::SerialHexView view;
    appendText(view, "Hello");
    view.clear();

    EXPECT_EQ(view.byteCount(), 0u);
    EXPECT_TRUE(view.hexText().empty());
    EXPECT_TRUE(view.asciiText().empty());
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
