// TerminalEmulator is the seam the roadmap's P1-A1/P1-A2 acceptance depends on: real
// session bytes in, correct screen state out. These tests drive it the way a transport
// does -- raw byte strings, split at awkward places -- rather than calling the grid directly,
// so a regression in the parser-to-grid binding cannot hide behind two independently green
// unit suites.

#include <gtest/gtest.h>

#include <string>

#include "terminal/emulator/Palette.h"
#include "terminal/emulator/TerminalEmulator.h"

namespace openscrt::terminal {
namespace {

class EmulatorTest : public ::testing::Test {
 protected:
    ScreenGrid grid{5, 10};
    TerminalEmulator emulator{grid};
};

TEST_F(EmulatorTest, PlainTextLandsOnTheGrid) {
    emulator.write("hello");
    EXPECT_EQ(grid.rowText(0), "hello");
    EXPECT_EQ(grid.cursorColumn(), 5);
}

TEST_F(EmulatorTest, CursorPositioningIsOneBased) {
    emulator.write(
        "\x1B[3;4H"
        "X");
    EXPECT_EQ(grid.rowText(2), "   X");
    EXPECT_EQ(grid.cursorRow(), 2);
    EXPECT_EQ(grid.cursorColumn(), 4);
}

TEST_F(EmulatorTest, CupDefaultsToTheTopLeft) {
    emulator.write("ab\x1B[HX");
    EXPECT_EQ(grid.rowText(0), "Xb");
}

TEST_F(EmulatorTest, CursorMovementClampsAtEdges) {
    emulator.write(
        "\x1B[5A\x1B[5D"
        "X");
    EXPECT_EQ(grid.rowText(0), "X");
}

TEST_F(EmulatorTest, EraseInLineRespectsTheThreeModes) {
    emulator.write("abcdef\x1B[1D\x1B[1;4H\x1B[0K");
    EXPECT_EQ(grid.rowText(0), "abc");

    emulator.reset();
    emulator.write("abcdef\x1B[1;4H\x1B[1K");
    EXPECT_EQ(grid.rowText(0), "    ef");

    emulator.reset();
    emulator.write("abcdef\x1B[2K");
    EXPECT_EQ(grid.rowText(0), "");
}

TEST_F(EmulatorTest, EraseInDisplayClearsBelowTheCursor) {
    emulator.write(
        "\x1B[2;1H"
        "abc"
        "\x1B[2;2H"
        "d\x1B[0J");
    // The 'd' lands on column 2 and advances the cursor to column 3, so ED(0) clears from
    // column 3 onward and both 'b' and 'c' survive.
    EXPECT_EQ(grid.rowText(1), "ad");
    for (int row = 2; row < grid.rows(); ++row) {
        EXPECT_EQ(grid.rowText(row), "") << "row " << row;
    }
}

TEST_F(EmulatorTest, LineFeedScrollsAndFeedsScrollback) {
    RingScrollbackBuffer scrollback(1024);
    TerminalEmulator with_history{grid, &scrollback};
    for (int i = 0; i < grid.rows(); ++i) {
        with_history.write("line" + std::to_string(i) + "\r\n");
    }
    EXPECT_GT(scrollback.size(), 0u);
    EXPECT_EQ(grid.rowText(0), "line1") << "line0 scrolled off into scrollback";
    EXPECT_EQ(grid.rowText(grid.rows() - 1), "") << "the final newline left a blank row";
}

TEST_F(EmulatorTest, AlternateScreenDoesNotFeedScrollback) {
    RingScrollbackBuffer scrollback(1024);
    TerminalEmulator with_history{grid, &scrollback};
    with_history.write("before\x1B[?1049h");
    const std::size_t after_switch = scrollback.size();
    for (int i = 0; i < grid.rows(); ++i) {
        with_history.write("alt\r\n");
    }
    EXPECT_EQ(scrollback.size(), after_switch) << "alternate screen output must not be recorded";
    with_history.write("\x1B[?1049l");
    EXPECT_EQ(grid.rowText(0), "before");
}

TEST_F(EmulatorTest, SaveAndRestoreCursor) {
    emulator.write(
        "\x1B[2;5H\x1B"
        "7"
        "\x1B[1;1H"
        "x\x1B"
        "8"
        "y");
    EXPECT_EQ(grid.rowText(1), "    y");
}

TEST_F(EmulatorTest, ScrollRegionLimitsLineFeeds) {
    emulator.write("\x1B[2;3r");  // region is rows 2..3, 1-based
    emulator.write(
        "\x1B[1;1H"
        "top"
        "\x1B[2;1H"
        "mid"
        "\x1B[3;1H"
        "bot");
    emulator.write(
        "\x1B[3;1H"
        "X\r\n"
        "next");
    EXPECT_EQ(grid.rowText(0), "top") << "row above the region must not scroll away";
    EXPECT_EQ(grid.rowText(1), "Xot") << "the region scrolls internally";
    EXPECT_EQ(grid.rowText(2), "next");
}

TEST_F(EmulatorTest, SgrBasicAttributes) {
    emulator.write(
        "\x1B[1mB\x1B[0m"
        "N");
    EXPECT_TRUE(grid.at(0, 0).hasAttribute(cellattr::kBold));
    EXPECT_FALSE(grid.at(0, 1).hasAttribute(cellattr::kBold));
}

TEST_F(EmulatorTest, SgrEightColourForegroundAndBackground) {
    emulator.write(
        "\x1B[31;44m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, paletteColor(1));
    EXPECT_EQ(grid.at(0, 0).bg, paletteColor(4));
    EXPECT_FALSE(grid.at(0, 0).hasAttribute(cellattr::kDefaultFg));
}

TEST_F(EmulatorTest, SgrBrightColourUsesTheUpperHalf) {
    emulator.write(
        "\x1B[91m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, paletteColor(9));
}

TEST_F(EmulatorTest, SgrIndexedColourSemicolonForm) {
    emulator.write(
        "\x1B[38;5;196m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, paletteColor(196));
}

TEST_F(EmulatorTest, SgrTrueColourSemicolonForm) {
    emulator.write(
        "\x1B[38;2;10;20;30m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, makeRgba(10, 20, 30));
}

TEST_F(EmulatorTest, SgrTrueColourColonFormSkipsTheOmittedSubParameter) {
    // 38:2::r:g:b carries an empty sub-parameter between the selector and the components.
    emulator.write(
        "\x1B[38:2::10:20:30m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, makeRgba(10, 20, 30));
}

TEST_F(EmulatorTest, SgrIndexedColourColonForm) {
    emulator.write(
        "\x1B[38:5:46m"
        "X");
    EXPECT_EQ(grid.at(0, 0).fg, paletteColor(46));
}

TEST_F(EmulatorTest, SgrBackgroundTrueColourColonForm) {
    emulator.write(
        "\x1B[48:2::1:2:3m"
        "X");
    EXPECT_EQ(grid.at(0, 0).bg, makeRgba(1, 2, 3));
}

TEST_F(EmulatorTest, SgrResetRestoresDefaults) {
    emulator.write(
        "\x1B[1;31;44m"
        "A\x1B[m"
        "B");
    EXPECT_FALSE(grid.at(0, 1).hasAttribute(cellattr::kBold));
    EXPECT_TRUE(grid.at(0, 1).hasAttribute(cellattr::kDefaultFg));
    EXPECT_TRUE(grid.at(0, 1).hasAttribute(cellattr::kDefaultBg));
}

TEST_F(EmulatorTest, PaletteCubeAndGreyRampMatchTheXtermFormulas) {
    EXPECT_EQ(paletteColor(16), makeRgba(0, 0, 0)) << "first cube entry is black";
    EXPECT_EQ(paletteColor(231), makeRgba(255, 255, 255)) << "last cube entry is white";
    EXPECT_EQ(paletteColor(232), makeRgba(8, 8, 8)) << "grey ramp starts at 8";
    EXPECT_EQ(paletteColor(255), makeRgba(238, 238, 238)) << "grey ramp steps by 10";
    EXPECT_EQ(paletteColor(196), makeRgba(255, 0, 0)) << "pure red lives at cube index 196";
}

TEST_F(EmulatorTest, WideGlyphsOccupyTwoColumns) {
    emulator.write("\xE4\xB8\xAD");
    EXPECT_TRUE(grid.at(0, 0).isWideLead());
    EXPECT_TRUE(grid.at(0, 1).isWideTrail());
    EXPECT_EQ(grid.cursorColumn(), 2);
}

TEST_F(EmulatorTest, DeviceAttributesReply) {
    emulator.write("\x1B[c");
    EXPECT_EQ(emulator.takeDeviceResponse(), "\x1B[?6c");
    EXPECT_TRUE(emulator.takeDeviceResponse().empty()) << "reply is taken, not copied";
}

TEST_F(EmulatorTest, CursorPositionReportIsOneBased) {
    emulator.write("\x1B[3;5H\x1B[6n");
    EXPECT_EQ(emulator.takeDeviceResponse(), "\x1B[3;5R");
}

TEST_F(EmulatorTest, DeviceStatusReply) {
    emulator.write("\x1B[5n");
    EXPECT_EQ(emulator.takeDeviceResponse(), "\x1B[0n");
}

TEST_F(EmulatorTest, BytesSplitAcrossWritesStillProduceOneScreenState) {
    const std::string stream =
        "\x1B[2;3H"
        "hello"
        "\x1B[31m"
        "!"
        " world";
    // The byte-at-a-time stream is longer than the 10-column fixture.
    ScreenGrid wide{5, 40};
    TerminalEmulator wide_emulator{wide};
    for (const char byte : stream) {
        wide_emulator.write(std::string_view(&byte, 1));
    }
    EXPECT_EQ(wide.rowText(1), "  hello! world");
    EXPECT_EQ(wide.at(1, 7).fg, paletteColor(1));
}

TEST_F(EmulatorTest, ResetReturnsToThePowerOnState) {
    emulator.write(
        "\x1B[2;3H"
        "text\x1B[31m");
    emulator.reset();
    EXPECT_EQ(grid.rowText(0), "");
    EXPECT_EQ(grid.cursorRow(), 0);
    EXPECT_EQ(grid.cursorColumn(), 0);
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kDefaultFg));
}

TEST_F(EmulatorTest, TabsAdvanceToEveryEight) {
    // The fixture grid is only 10 columns wide, so it has a single tab stop.
    ScreenGrid wide{5, 40};
    TerminalEmulator wide_emulator{wide};
    wide_emulator.write(
        "a"
        "\t"
        "b"
        "\t"
        "c");
    EXPECT_EQ(wide.cursorColumn(), 17);
    EXPECT_EQ(wide.rowText(0), "a       b       c");
}

TEST_F(EmulatorTest, DeleteCharactersPullsTheRowLeft) {
    emulator.write(
        "abcdef"
        "\x1B[1;3H"
        "\x1B[2P");
    EXPECT_EQ(grid.rowText(0), "abef");
    EXPECT_EQ(grid.cursorColumn(), 2) << "DCH must not move the cursor";
}

TEST_F(EmulatorTest, EraseCharactersLeavesTheCursorPut) {
    emulator.write("abcdef\x1B[1;3H\x1B[2X");
    EXPECT_EQ(grid.rowText(0), "ab  ef");
    EXPECT_EQ(grid.cursorColumn(), 2);
}

}  // namespace
}  // namespace openscrt::terminal