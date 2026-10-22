// Damage tracking tests (P1-T6).
//
// The performance claim behind the roadmap's dirty-rectangle renderer is that repainting
// costs scale with *what changed*, not with how many update() calls happen. These tests pin
// the coalescing behaviour that makes that true, using a deterministic grid so a regression
// shows up as a wrong band list rather than a slower CI run.

#include <gtest/gtest.h>

#include <vector>

#include "terminal/renderer/DamageTracker.h"

namespace openscrt::terminal {
namespace {

class DamageTrackerTest : public ::testing::Test {
 protected:
    ScreenGrid grid{10, 40};

    void writeRow(int row, char ch) {
        grid.setCursor(row, 0);
        for (int col = 0; col < grid.cols(); ++col) {
            grid.put(static_cast<std::uint32_t>(ch));
        }
    }
};

TEST_F(DamageTrackerTest, CleanGridProducesNoBands) {
    const auto bands = DamageTracker::collect(grid);
    EXPECT_TRUE(bands.empty());
}

TEST_F(DamageTrackerTest, OneDirtyRowIsOneBand) {
    writeRow(3, 'a');
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(bands.front(), (RowBand{3, 3}));
}

TEST_F(DamageTrackerTest, AdjacentRowsCoalesceIntoASingleBand) {
    writeRow(2, 'a');
    writeRow(3, 'b');
    writeRow(4, 'c');
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 1u) << "adjacent rows must not cost three update() calls";
    EXPECT_EQ(bands.front(), (RowBand{2, 4}));
}

TEST_F(DamageTrackerTest, SeparatedRowsProduceSeparateBands) {
    writeRow(0, 'a');
    writeRow(5, 'b');
    writeRow(9, 'c');
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 3u);
    EXPECT_EQ(bands[0], (RowBand{0, 0}));
    EXPECT_EQ(bands[1], (RowBand{5, 5}));
    EXPECT_EQ(bands[2], (RowBand{9, 9}));
}

TEST_F(DamageTrackerTest, AFullyDamagedGridIsOneFullHeightBand) {
    for (int row = 0; row < grid.rows(); ++row) {
        writeRow(row, 'x');
    }
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(bands.front(), (RowBand{0, grid.rows() - 1}));
}

TEST_F(DamageTrackerTest, CollectConsumesTheDirtyFlags) {
    writeRow(1, 'a');
    ASSERT_EQ(DamageTracker::collect(grid).size(), 1u);
    EXPECT_TRUE(DamageTracker::collect(grid).empty())
        << "a second collect must not repeat the same damage";
}

TEST_F(DamageTrackerTest, DamageAfterCollectIsReportedAgain) {
    writeRow(1, 'a');
    ASSERT_EQ(DamageTracker::collect(grid).size(), 1u);
    writeRow(7, 'b');
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(bands.front(), (RowBand{7, 7}));
}

TEST_F(DamageTrackerTest, BandsAreSortedAndDisjoint) {
    for (int row : {9, 1, 6, 2}) {
        writeRow(row, 'x');
    }
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 2u);
    EXPECT_EQ(bands[0], (RowBand{1, 2}));
    EXPECT_EQ(bands[1], (RowBand{6, 6}));
    EXPECT_EQ(bands[1].first, bands[0].last + 2) << "bands must not touch or overlap";
}

TEST_F(DamageTrackerTest, CollectAllIgnoresCurrentDamage) {
    const auto bands = DamageTracker::collectAll(grid);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(bands.front(), (RowBand{0, grid.rows() - 1}));
    EXPECT_TRUE(DamageTracker::collect(grid).empty());
}

TEST_F(DamageTrackerTest, RectCoversExactlyTheBands) {
    writeRow(2, 'a');
    writeRow(3, 'b');
    const auto bands = DamageTracker::collect(grid);
    const CellMetrics metrics{10, 20};
    EXPECT_EQ(DamageTracker::rectFor(bands, grid.cols(), metrics), (PixelRect{0, 40, 400, 40}));
}

TEST_F(DamageTrackerTest, RectSpansEveryBandItWasGiven) {
    writeRow(0, 'a');
    writeRow(9, 'b');
    const auto bands = DamageTracker::collect(grid);
    const CellMetrics metrics{8, 16};
    EXPECT_EQ(DamageTracker::rectFor(bands, grid.cols(), metrics), (PixelRect{0, 0, 320, 160}));
}

TEST_F(DamageTrackerTest, EmptyBandsProduceAnEmptyRectSoTheCallerSkipsUpdate) {
    EXPECT_TRUE(DamageTracker::rectFor({}, 40, CellMetrics{8, 16}).isEmpty());
}

TEST_F(DamageTrackerTest, FullRectMatchesTheGridGeometry) {
    EXPECT_EQ(DamageTracker::fullRect(24, 80, CellMetrics{8, 16}), (PixelRect{0, 0, 640, 384}));
}

TEST_F(DamageTrackerTest, FullScreenDirtiedByOneWriteIsCheaperThanMany) {
    // The shape of the `seq 1 1000000` case: every row dirtied, and the tracker must hand
    // back one band so the renderer issues one update() for the whole screen.
    for (int row = 0; row < grid.rows(); ++row) {
        writeRow(row, '0' + (row % 10));
    }
    const auto bands = DamageTracker::collect(grid);
    ASSERT_EQ(bands.size(), 1u);
    EXPECT_EQ(DamageTracker::rectFor(bands, grid.cols(), CellMetrics{8, 16}),
              DamageTracker::fullRect(grid.rows(), grid.cols(), CellMetrics{8, 16}));
}

}  // namespace
}  // namespace openscrt::terminal