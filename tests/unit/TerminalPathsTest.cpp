// The paths of the terminal that the other tests do not walk.
//
// The emulator, the grid and the parser each have a set of behaviours that only show up
// when the input is unusual: a control character nobody sends, a sequence that is
// malformed in the middle, a grid that is asked for a cell that is not there.
//
// They are worth a test each for a reason that has nothing to do with coverage: a
// terminal that mishandles a malformed sequence shows up as "vim renders wrong", and the
// bug is enormously expensive to find later.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/buffer/RingScrollbackBuffer.h"
#include "terminal/buffer/ScreenGrid.h"
#include "terminal/emulator/ControlSequenceParser.h"
#include "terminal/emulator/TerminalEmulator.h"

namespace openscrt::terminal {
namespace {

// ------------------------------------------------------------------ the grid

TEST(ScreenGridPathsTest, AnEmptyGridHasNoCells) {
    ScreenGrid grid;
    EXPECT_TRUE(grid.empty());
    // Asking an empty grid for a cell is a mistake, and it has to be reported as one
    // rather than answered with whatever happens to be in memory.
    EXPECT_THROW(static_cast<void>(grid.at(0, 0)), std::out_of_range);
    EXPECT_EQ(grid.rowData(0), nullptr);
}

TEST(ScreenGridPathsTest, TheUtf8EncoderCoversTheWholeRange) {
    // One, two, three and four bytes, plus the scalars that cannot be encoded at all.
    struct Case {
        std::uint32_t codepoint;
        std::string_view expected;
    };
    const std::vector<Case> cases = {
        {0x41, "\x41"},
        {0xE9, "\xC3\xA9"},
        {0x4E2D, "\xE4\xB8\xAD"},
        {0x1F600, "\xF0\x9F\x98\x80"},
        {0xD800, "\xEF\xBF\xBD"},    // a surrogate is not a scalar
        {0x110000, "\xEF\xBF\xBD"},  // and neither is anything above the last one
    };
    for (const Case& item : cases) {
        std::vector<Cell> cells(1);
        cells[0].codepoint = item.codepoint;
        EXPECT_EQ(ScreenGrid::toUtf8(cells.data(), 1), item.expected)
            << "codepoint " << item.codepoint;
    }
}

TEST(ScreenGridPathsTest, BackspaceAtTheStartOfALineDoesNothing) {
    ScreenGrid grid(2, 8);
    grid.setCursor(1, 0);
    grid.backspace();
    EXPECT_EQ(grid.cursorColumn(), 0) << "the cursor went left of the line";
    EXPECT_EQ(grid.cursorRow(), 1);

    grid.setCursor(1, 3);
    grid.backspace();
    EXPECT_EQ(grid.cursorColumn(), 2);
}

TEST(ScreenGridPathsTest, TheTabStopsCanBeSetAndCleared) {
    ScreenGrid grid(1, 20);
    grid.setTabStop(4);
    grid.setTabStop(9);
    grid.setCursor(0, 0);
    grid.tabForward();
    EXPECT_EQ(grid.cursorColumn(), 4);
    grid.tabForward();
    EXPECT_EQ(grid.cursorColumn(), 9);

    // Without a tab stop left, the cursor goes to the end of the line.
    grid.clearAllTabStops();
    grid.setCursor(0, 0);
    grid.tabForward();
    EXPECT_EQ(grid.cursorColumn(), 19);
}

TEST(ScreenGridPathsTest, EraseInDisplayFromTheCursorToTheStartBlanksEverythingAbove) {
    ScreenGrid grid(3, 4);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            grid.at(row, col).codepoint = 'x';
        }
    }
    grid.setCursor(1, 1);

    grid.eraseInDisplay(EraseInDisplay::kCursorToStart);

    EXPECT_EQ(grid.at(0, 0).codepoint, 0u) << "the row above the cursor is not blank";
    EXPECT_EQ(grid.at(1, 0).codepoint, 0u) << "the line up to the cursor is not blank";
    EXPECT_EQ(grid.at(1, 1).codepoint, 0u) << "the cursor's own cell is not blank";
    EXPECT_EQ(grid.at(2, 0).codepoint, static_cast<std::uint32_t>('x'))
        << "the row below the cursor was touched";
}

TEST(ScreenGridPathsTest, ScrollDownPushesTheLinesUp) {
    ScreenGrid grid(3, 2);
    for (int row = 0; row < 3; ++row) {
        grid.at(row, 0).codepoint = static_cast<std::uint32_t>('a' + row);
    }

    grid.scrollDown(1);

    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(grid.at(1, 0).codepoint, static_cast<std::uint32_t>('b'));
    // The last line is blank: the content moved up and a fresh line appeared at the end.
    EXPECT_EQ(grid.at(2, 0).codepoint, 0u);

    // Scrolling by more than the window blanks the whole thing.
    grid.scrollDown(10);
    EXPECT_EQ(grid.at(0, 0).codepoint, 0u);
}

TEST(ScreenGridPathsTest, TheScrolledOffLinesGoIntoTheScrollback) {
    RingScrollbackBuffer scrollback;
    ScreenGrid grid(2, 4);
    TerminalEmulator emulator(grid, &scrollback);
    emulator.write("first\n");
    emulator.write("second\n");

    // The lines that leave the top of the screen are the history.
    const std::size_t moved = grid.flushScrolledOffTo(scrollback);
    EXPECT_GT(moved, 0u);
    EXPECT_GT(scrollback.size(), 0u);
}

TEST(ScreenGridPathsTest, TheAlternateScreenNeverFeedsTheScrollback) {
    RingScrollbackBuffer scrollback;
    ScreenGrid grid(2, 4);
    TerminalEmulator emulator(grid, &scrollback);
    emulator.write("first\n");

    grid.enterAlternateScreen();
    EXPECT_EQ(grid.flushScrolledOffTo(scrollback), 0u)
        << "the alternate screen must not pollute the history";
}

// ------------------------------------------------------------------ the emulator

TEST(TerminalEmulatorPathsTest, TheBellDoesNothing) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);
    emulator.write("a");
    const int column = grid.cursorColumn();

    emulator.write("\x07");

    EXPECT_EQ(grid.cursorColumn(), column);
    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('a'));
}

TEST(TerminalEmulatorPathsTest, BackspaceMovesTheCursorLeft) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");

    emulator.write("\x08");

    EXPECT_EQ(grid.cursorColumn(), 1);
    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(grid.at(0, 1).codepoint, static_cast<std::uint32_t>('b'));
}

TEST(TerminalEmulatorPathsTest, CarriageReturnAndLineFeedMoveTheCursor) {
    ScreenGrid grid(3, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");

    emulator.write("\r");
    EXPECT_EQ(grid.cursorColumn(), 0) << "the carriage return did not go back to the line";

    emulator.write("\n");
    EXPECT_EQ(grid.cursorRow(), 1);
    EXPECT_EQ(grid.cursorColumn(), 0);
}

TEST(TerminalEmulatorPathsTest, AnUnknownControlCharacterIsIgnored) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");

    emulator.write("\x01");

    EXPECT_EQ(grid.cursorColumn(), 2) << "an unknown control character moved the cursor";
    EXPECT_EQ(grid.at(0, 2).codepoint, 0u) << "an unknown control character wrote something";
}

TEST(TerminalEmulatorPathsTest, ReverseIndexMovesTheCursorUp) {
    ScreenGrid grid(3, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");
    emulator.write("\n");
    ASSERT_EQ(grid.cursorRow(), 1);

    emulator.write("\x1bM");

    EXPECT_EQ(grid.cursorRow(), 0) << "the reverse index did not move the cursor up";
}

TEST(TerminalEmulatorPathsTest, TheResetSequencePutsTheScreenBack) {
    ScreenGrid grid(3, 8);
    TerminalEmulator emulator(grid);
    emulator.write("hello");
    emulator.write("\x1b[31m");  // red

    emulator.write("\x1bc");

    EXPECT_EQ(grid.at(0, 0).codepoint, 0u) << "the reset did not clear the screen";
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kInverse));
}

TEST(TerminalEmulatorPathsTest, InsertModeShiftsTheLineToTheRight) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");

    emulator.write("\x1b[4h");  // IRM
    emulator.write("c");

    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(grid.at(0, 1).codepoint, static_cast<std::uint32_t>('b'));
    EXPECT_EQ(grid.at(0, 2).codepoint, static_cast<std::uint32_t>('c'));
}

TEST(TerminalEmulatorPathsTest, InsertAndDeleteLinesMoveTheRows) {
    ScreenGrid grid(3, 4);
    TerminalEmulator emulator(grid);
    for (int row = 0; row < 3; ++row) {
        grid.at(row, 0).codepoint = static_cast<std::uint32_t>('a' + row);
    }

    emulator.write("\x1b[1L");  // IL: one blank line at the cursor
    EXPECT_EQ(grid.at(0, 0).codepoint, 0u) << "the line at the cursor is not blank";
    EXPECT_EQ(grid.at(1, 0).codepoint, static_cast<std::uint32_t>('a'))
        << "the content did not move down";

    emulator.write("\x1b[1M");  // DL: one line deleted at the cursor
    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(grid.at(1, 0).codepoint, static_cast<std::uint32_t>('b'));
}

TEST(TerminalEmulatorPathsTest, ScrollUpAndDownMoveTheContent) {
    ScreenGrid grid(3, 4);
    TerminalEmulator emulator(grid);
    for (int row = 0; row < 3; ++row) {
        grid.at(row, 0).codepoint = static_cast<std::uint32_t>('a' + row);
    }

    emulator.write("\x1b[1S");  // SU
    EXPECT_EQ(grid.at(0, 0).codepoint, static_cast<std::uint32_t>('b'));

    emulator.write("\x1b[1T");  // SD
    EXPECT_EQ(grid.at(0, 0).codepoint, 0u) << "the content did not move down";
    EXPECT_EQ(grid.at(1, 0).codepoint, static_cast<std::uint32_t>('b'));
}

TEST(TerminalEmulatorPathsTest, TheDeviceResponseIsKeptUntilItIsTaken) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);

    emulator.deviceResponse("\x1b[?1;2c");

    EXPECT_EQ(emulator.takeDeviceResponse(), "\x1b[?1;2c");
    EXPECT_TRUE(emulator.takeDeviceResponse().empty()) << "the reply was not taken";
}

TEST(TerminalEmulatorPathsTest, TheModeChangesAreApplied) {
    ScreenGrid grid(4, 8);
    TerminalEmulator emulator(grid);
    emulator.write("ab");
    ASSERT_EQ(grid.cursorColumn(), 2);

    emulator.write("\x1b[?5h");  // DECSCNM: reverse video
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kInverse));
    emulator.write("\x1b[?5l");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kInverse));

    emulator.write("\x1b[?6h");  // DECOM: the origin mode puts the cursor at the top
    EXPECT_EQ(grid.cursorRow(), 0);
    EXPECT_EQ(grid.cursorColumn(), 0);
}

TEST(TerminalEmulatorPathsTest, CursorNextLineAndPreviousLine) {
    ScreenGrid grid(5, 8);
    TerminalEmulator emulator(grid);
    emulator.write("\x1b[3;5H");
    ASSERT_EQ(grid.cursorRow(), 2);
    ASSERT_EQ(grid.cursorColumn(), 4);

    emulator.write("\x1bE");  // CNL: down one line, column zero
    EXPECT_EQ(grid.cursorRow(), 3);
    EXPECT_EQ(grid.cursorColumn(), 0);

    emulator.write("\x1bF");  // CPL: up one line, column zero
    EXPECT_EQ(grid.cursorRow(), 2);
    EXPECT_EQ(grid.cursorColumn(), 0);
}

TEST(TerminalEmulatorPathsTest, HorizontalPositionAbsolute) {
    ScreenGrid grid(4, 8);
    TerminalEmulator emulator(grid);
    emulator.write("\x1b[2;5H");

    emulator.write("\x1b[3`");  // HPA: the column, the row stays

    EXPECT_EQ(grid.cursorRow(), 1);
    EXPECT_EQ(grid.cursorColumn(), 2);
}

TEST(TerminalEmulatorPathsTest, EraseDisplayFromTheCursorToTheStart) {
    ScreenGrid grid(3, 4);
    TerminalEmulator emulator(grid);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            grid.at(row, col).codepoint = 'x';
        }
    }
    emulator.write("\x1b[2;3H");

    emulator.write("\x1b[1J");

    EXPECT_EQ(grid.at(0, 0).codepoint, 0u);
    EXPECT_EQ(grid.at(2, 0).codepoint, static_cast<std::uint32_t>('x'));
}

TEST(TerminalEmulatorPathsTest, TheGraphicRenditionAttributes) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);

    emulator.write("\x1b[5m");
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kBlink));
    emulator.write("\x1b[25m");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kBlink));

    emulator.write("\x1b[8m");
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kInvisible));
    emulator.write("\x1b[28m");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kInvisible));

    emulator.write("\x1b[9m");
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kStrikeThrough));

    emulator.write("\x1b[1m");
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kBold));
    emulator.write("\x1b[21m");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kBold));

    emulator.write("\x1b[4m");
    EXPECT_TRUE(emulator.style().hasAttribute(cellattr::kUnderline));
    emulator.write("\x1b[24m");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kUnderline));

    // A bright background is a colour of its own, and it is not the default one.
    emulator.write("\x1b[101m");
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kDefaultBg));
}

TEST(TerminalEmulatorPathsTest, SgrZeroResetsTheStyle) {
    ScreenGrid grid(2, 8);
    TerminalEmulator emulator(grid);

    emulator.write("\x1b[1;4;5m");
    ASSERT_TRUE(emulator.style().hasAttribute(cellattr::kBold));

    emulator.write("\x1b[0m");

    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kBold));
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kUnderline));
    EXPECT_FALSE(emulator.style().hasAttribute(cellattr::kBlink));
}

// ------------------------------------------------------------------ the parser

// A sink that only counts. The parser tests are about what the parser *swallows* and
// what it hands on, not about the shape of what it hands on.
class CountingSink : public IControlSequenceSink {
 public:
    int prints{0};
    int csi{0};
    int osc{0};
    int escapes{0};
    std::uint32_t last_codepoint{0};
    std::vector<char> last_intermediates;
    char last_final{0};

    void print(std::uint32_t codepoint) override {
        ++prints;
        last_codepoint = codepoint;
    }

    void execute(std::uint8_t) override {}

    void escapeDispatch(std::span<const std::uint8_t> intermediates, char final) override {
        ++escapes;
        last_intermediates.assign(intermediates.begin(), intermediates.end());
        last_final = final;
    }

    void csiDispatch(const CsiSequence&) override { ++csi; }

    void oscDispatch(const OscSequence&) override { ++osc; }

    void deviceResponse(std::string_view) override {}
};

TEST(ControlSequenceParserPathsTest, AMalformedParameterIsSwallowed) {
    // A control character in the middle of a parameter list: the sequence is not
    // dispatched, and the parser is back in the ground state afterwards.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1b[1\x01m");
    parser.feed("x");

    EXPECT_EQ(sink.csi, 0) << "a malformed sequence was dispatched";
    EXPECT_EQ(sink.prints, 1) << "the parser did not recover";
    EXPECT_EQ(sink.last_codepoint, static_cast<std::uint32_t>('x'));
}

TEST(ControlSequenceParserPathsTest, AnUnknownStringIntroducerIsSwallowed) {
    // DCS and friends: the payload is not text and must not be printed.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1bPsome payload\x1b\\");
    parser.feed("x");

    EXPECT_EQ(sink.prints, 1) << "the string was printed";
    EXPECT_EQ(sink.last_codepoint, static_cast<std::uint32_t>('x'));
}

TEST(ControlSequenceParserPathsTest, ASurrogateBecomesTheReplacementCharacter) {
    // A UTF-8 encoded surrogate is not a scalar value; it must not reach the screen as
    // one.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\xED\xA0\x80");

    EXPECT_EQ(sink.prints, 1);
    EXPECT_EQ(sink.last_codepoint, 0xFFFDu);
}

TEST(ControlSequenceParserPathsTest, AnAbortByteEndsTheSequence) {
    // CAN and SUB abort whatever is in flight.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1b[1\x18m");

    EXPECT_EQ(sink.csi, 0) << "the aborted sequence was dispatched";
    EXPECT_EQ(sink.prints, 1) << "the byte after the abort was not printed";
}

TEST(ControlSequenceParserPathsTest, TheNewerSequenceWins) {
    // An escape in the middle of a sequence starts a new one.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1b[1\x1b[2m");

    EXPECT_EQ(sink.csi, 1) << "the sequence was dispatched twice or not at all";
}

TEST(ControlSequenceParserPathsTest, AnIntermediateIsKept) {
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1b[1$q");

    EXPECT_EQ(sink.csi, 1);
    ASSERT_EQ(sink.last_intermediates.size(), 1u);
    EXPECT_EQ(sink.last_intermediates[0], '$');
    EXPECT_EQ(sink.last_final, 'q');
}

TEST(ControlSequenceParserPathsTest, AnOscEscapeReturnsToTheString) {
    // An ESC inside an OSC that is not the ST terminator goes back into the string.
    CountingSink sink;
    ControlSequenceParser parser(sink);

    parser.feed("\x1b]0;title\x1bx\x07");

    EXPECT_EQ(sink.osc, 1) << "the OSC was not dispatched";
    EXPECT_EQ(sink.prints, 0) << "the OSC payload was printed";
}

}  // namespace
}  // namespace openscrt::terminal
