// P1-T2 / P1-A2 / P1-A6: the screen grid is the contract every escape sequence depends on, so
// these tests assert observable grid behaviour rather than parser internals.

#include <gtest/gtest.h>

#include <string>

#include "terminal/buffer/ScreenGrid.h"

namespace openscrt::terminal {
namespace {

constexpr std::uint32_t kCharA = U'A';
constexpr std::uint32_t kCharB = U'B';

void writeText(ScreenGrid& grid, const std::string& text) {
    for (const char ch : text) {
        grid.put(static_cast<std::uint32_t>(static_cast<unsigned char>(ch)));
    }
}

TEST(ScreenGridTest, FreshGridIsBlank) {
    ScreenGrid grid(3, 4);
    EXPECT_EQ(grid.rows(), 3);
    EXPECT_EQ(grid.cols(), 4);
    EXPECT_EQ(grid.cursorRow(), 0);
    EXPECT_EQ(grid.cursorColumn(), 0);
    for (int row = 0; row < grid.rows(); ++row) {
        for (int col = 0; col < grid.cols(); ++col) {
            EXPECT_TRUE(grid.at(row, col).isBlank());
        }
    }
}

TEST(ScreenGridTest, PutAdvancesCursor) {
    ScreenGrid grid(2, 8);
    writeText(grid, "hi");
    EXPECT_EQ(grid.rowText(0), "hi");
    EXPECT_EQ(grid.cursorColumn(), 2);
}

TEST(ScreenGridTest, CarriageReturnAndLineFeed) {
    ScreenGrid grid(3, 8);
    writeText(grid, "abc");
    grid.carriageReturn();
    EXPECT_EQ(grid.cursorColumn(), 0);
    grid.lineFeed();
    EXPECT_EQ(grid.cursorRow(), 1);
    writeText(grid, "xyz");
    EXPECT_EQ(grid.rowText(0), "abc");
    EXPECT_EQ(grid.rowText(1), "xyz");
}

TEST(ScreenGridTest, AutoWrapAfterLastColumn) {
    ScreenGrid grid(3, 4);
    writeText(grid, "abcd");
    // Cursor parks on the right margin; the next glyph wraps to the next row.
    EXPECT_EQ(grid.cursorColumn(), 3);
    grid.put(kCharA);
    EXPECT_EQ(grid.rowText(0), "abcd");
    EXPECT_EQ(grid.rowText(1), "a");
}

TEST(ScreenGridTest, LineFeedAtBottomScrolls) {
    ScreenGrid grid(2, 4);
    writeText(grid, "row0");
    grid.carriageReturn();
    grid.lineFeed();
    writeText(grid, "row1");
    grid.carriageReturn();
    grid.lineFeed();  // at scroll bottom -> scroll up
    writeText(grid, "row2");

    EXPECT_EQ(grid.rowText(0), "row1");
    EXPECT_EQ(grid.rowText(1), "row2");
}

TEST(ScreenGridTest, ScrollRegionConstrainsScrolling) {
    ScreenGrid grid(5, 4);
    grid.setScrollRegion(1, 3);
    EXPECT_EQ(grid.scrollTop(), 1);
    EXPECT_EQ(grid.scrollBottom(), 3);
    EXPECT_EQ(grid.cursorRow(), 1) << "DECSTBM homes the cursor";

    grid.setCursor(3, 0);
    writeText(grid, "bot");
    grid.carriageReturn();
    grid.lineFeed();  // scrolls inside the region only
    writeText(grid, "new");

    EXPECT_EQ(grid.rowText(0), "") << "row above the region must not move";
    EXPECT_EQ(grid.rowText(1), "");
    EXPECT_EQ(grid.rowText(2), "bot");
    EXPECT_EQ(grid.rowText(3), "new");
}

TEST(ScreenGridTest, ReverseIndexScrollsDownAtRegionTop) {
    ScreenGrid grid(4, 4);
    writeText(grid, "a");
    grid.carriageReturn();
    grid.lineFeed();
    writeText(grid, "b");
    grid.carriageReturn();
    grid.lineFeed();
    grid.reverseIndex();  // back to row 1

    grid.setCursor(1, 0);
    grid.put(kCharA);
    EXPECT_EQ(grid.rowText(1), "A");
    EXPECT_EQ(grid.rowText(2), "");
}

TEST(ScreenGridTest, EraseInLineScopes) {
    ScreenGrid grid(1, 10);
    writeText(grid, "0123456789");
    grid.carriageReturn();
    grid.setCursor(0, 4);

    grid.eraseInLine(EraseInLine::kCursorToEnd);
    EXPECT_EQ(grid.rowText(0), "0123");

    ScreenGrid grid2(1, 10);
    writeText(grid2, "0123456789");
    grid2.setCursor(0, 4);
    grid2.eraseInLine(EraseInLine::kCursorToStart);
    EXPECT_EQ(grid2.rowText(0), "56789");

    ScreenGrid grid3(1, 10);
    writeText(grid3, "0123456789");
    grid3.setCursor(0, 4);
    grid3.eraseInLine(EraseInLine::kAll);
    EXPECT_EQ(grid3.rowText(0), "");
}

TEST(ScreenGridTest, EraseInDisplayScopes) {
    ScreenGrid grid(3, 4);
    for (int row = 0; row < 3; ++row) {
        grid.setCursor(row, 0);
        writeText(grid, "abcd");
    }
    grid.setCursor(0, 2);
    grid.eraseInDisplay(EraseInDisplay::kCursorToEnd);
    EXPECT_EQ(grid.rowText(0), "ab");
    EXPECT_EQ(grid.rowText(1), "");
    EXPECT_EQ(grid.rowText(2), "");

    grid.setCursor(0, 0);
    grid.eraseInDisplay(EraseInDisplay::kAll);
    EXPECT_EQ(grid.rowText(0), "");
}

TEST(ScreenGridTest, EraseCharactersDoesNotMoveCursor) {
    ScreenGrid grid(1, 8);
    writeText(grid, "abcdefgh");
    grid.setCursor(0, 2);
    grid.eraseCharacters(3);
    EXPECT_EQ(grid.rowText(0), "ab   fgh");
    EXPECT_EQ(grid.cursorColumn(), 2);
}

TEST(ScreenGridTest, TabForwardStopsAtTabColumns) {
    ScreenGrid grid(1, 32);
    grid.setCursor(0, 0);
    grid.tabForward();
    EXPECT_EQ(grid.cursorColumn(), 8);
    grid.tabForward();
    EXPECT_EQ(grid.cursorColumn(), 16);

    grid.setTabStop(20);
    grid.setCursor(0, 0);
    for (int i = 0; i < 3; ++i) {
        grid.tabForward();
    }
    EXPECT_EQ(grid.cursorColumn(), 20);
}

TEST(ScreenGridTest, AlternateScreenIsIsolatedAndRestoresPrimary) {
    // P1-A6: leaving the alternate buffer must bring back exactly what was on screen before.
    ScreenGrid grid(3, 6);
    grid.setCursor(0, 0);
    writeText(grid, "shell");
    grid.setCursor(2, 0);
    writeText(grid, "prompt");

    grid.enterAlternateScreen();
    EXPECT_TRUE(grid.isAlternateScreen());
    for (int row = 0; row < grid.rows(); ++row) {
        for (int col = 0; col < grid.cols(); ++col) {
            EXPECT_TRUE(grid.at(row, col).isBlank());
        }
    }
    grid.put(kCharB);
    EXPECT_EQ(grid.rowText(0), "B");

    grid.leaveAlternateScreen();
    EXPECT_FALSE(grid.isAlternateScreen());
    EXPECT_EQ(grid.rowText(0), "shell");
    EXPECT_EQ(grid.rowText(2), "prompt");
}

TEST(ScreenGridTest, WideGlyphOccupiesTwoCells) {
    ScreenGrid grid(1, 6);
    CellStyle style;
    style.setAttribute(cellattr::kWideLead, true);
    grid.put(0x4E2D, style);  // CJK '中'

    EXPECT_TRUE(grid.at(0, 0).isWideLead());
    EXPECT_EQ(grid.at(0, 0).codepoint, 0x4E2D);
    EXPECT_TRUE(grid.at(0, 1).isWideTrail());
    EXPECT_EQ(grid.at(0, 1).codepoint, 0u);
    EXPECT_EQ(grid.cursorColumn(), 2);
    EXPECT_EQ(grid.rowText(0), "\xE4\xB8\xAD") << "trailing blank half must not emit a byte";
}

TEST(ScreenGridTest, WideGlyphWrapsRatherThanSplittingAtMargin) {
    ScreenGrid grid(2, 3);
    grid.put(kCharA);  // cursor -> 1
    CellStyle style;
    style.setAttribute(cellattr::kWideLead, true);
    grid.put(0x4E2D, style);  // only 2 columns used, one left over

    EXPECT_EQ(grid.rowText(0), "A");
    EXPECT_EQ(grid.rowText(1), "\xE4\xB8\xAD");
}

TEST(ScreenGridTest, ResizePreservesOriginAlignedContent) {
    ScreenGrid grid(3, 8);
    grid.setCursor(1, 0);
    writeText(grid, "keep");
    grid.resize(5, 12);
    EXPECT_EQ(grid.rows(), 5);
    EXPECT_EQ(grid.cols(), 12);
    EXPECT_EQ(grid.rowText(1), "keep");

    grid.resize(2, 8);
    EXPECT_EQ(grid.rowText(1), "keep");
}

TEST(ScreenGridTest, DirtyFlagsTrackWritesAndReset) {
    ScreenGrid grid(3, 4);
    EXPECT_EQ(grid.dirtyRowCount(), 0);
    grid.put(kCharA);
    EXPECT_EQ(grid.dirtyRowCount(), 1);
    EXPECT_TRUE(grid.rowIsDirty(0));
    EXPECT_FALSE(grid.rowIsDirty(1));

    grid.clearDirtyFlags();
    EXPECT_EQ(grid.dirtyRowCount(), 0);
}

TEST(ScreenGridTest, RowTextTrimsTrailingBlanksAndEncodesUtf8) {
    ScreenGrid grid(1, 8);
    grid.put(0x00E9);  // e-acute, two UTF-8 bytes
    grid.put(0x4E2D);
    EXPECT_EQ(grid.rowText(0), "\xC3\xA9\xE4\xB8\xAD");
}

TEST(ScreenGridTest, ResetClearsActiveBuffer) {
    ScreenGrid grid(2, 4);
    writeText(grid, "data");
    grid.reset();
    EXPECT_EQ(grid.rowText(0), "");
    EXPECT_EQ(grid.cursorRow(), 0);
    EXPECT_EQ(grid.cursorColumn(), 0);
}

TEST(ScreenGridTest, CursorCannotLeaveGrid) {
    ScreenGrid grid(3, 4);
    grid.setCursor(99, 99);
    EXPECT_EQ(grid.cursorRow(), 2);
    EXPECT_EQ(grid.cursorColumn(), 3);
    grid.moveCursor(-100, -100);
    EXPECT_EQ(grid.cursorRow(), 0);
    EXPECT_EQ(grid.cursorColumn(), 0);
}

TEST(ScreenGridTest, InvalidScrollRegionIsIgnored) {
    ScreenGrid grid(5, 4);
    grid.setScrollRegion(3, 1);
    EXPECT_EQ(grid.scrollTop(), 0);
    EXPECT_EQ(grid.scrollBottom(), 4);
}

}  // namespace
}  // namespace openscrt::terminal