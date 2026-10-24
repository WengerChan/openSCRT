// The split workspace (P2-T10).
//
// The panes are the part of the workspace that is easy to get wrong: a session belongs to
// the pane it was opened in, the sessions of all panes keep one order, and a tile moves
// the panes without moving the sessions out of them.
//
// The order is the part that matters for the broadcast: twenty machines have to get the
// same command in the same order, and the panes must not shuffle it.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <memory>
#include <string>

#include "config/SessionProfile.h"
#include "ui/workspace/TerminalSession.h"
#include "ui/workspace/WorkspacePanes.h"
#include "ui/workspace/WorkspaceTabs.h"

namespace openscrt::ui {
namespace {

// QWidget requires QApplication, not QGuiApplication: creating a widget under a plain
// QGuiApplication aborts the process.
const QApplication& application() {
    static QApplication* instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "openscrt-pane-tests";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return *instance;
}

// A local shell is the one protocol that needs no server. The panes do not care what is
// in a pane, only which pane it is in.
config::SessionProfile localProfile(std::string id) {
    config::SessionProfile profile;
    profile.id = std::move(id);
    profile.name = profile.id;
    profile.protocol = config::Protocol::kLocalShell;
    return profile;
}

class WorkspacePanesTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override { panes_ = std::make_unique<WorkspacePanes>(); }

    void openLocal(const std::string& id) {
        std::string error;
        const int index = panes_->openSession(localProfile(id), error);
        EXPECT_GE(index, 0) << error;
    }

    // The pane a session sits in, by looking through every pane.
    int paneOf(const QString& id) const {
        for (int index = 0; index < panes_->paneCount(); ++index) {
            WorkspaceTabs* pane = panes_->paneAt(index);
            if (pane != nullptr && pane->sessionById(id) != nullptr) {
                return index;
            }
        }
        return -1;
    }

    std::unique_ptr<WorkspacePanes> panes_;
};

TEST_F(WorkspacePanesTest, ThereIsOnePaneToStartWith) {
    EXPECT_EQ(panes_->paneCount(), 1);
    EXPECT_NE(panes_->paneAt(0), nullptr);
    EXPECT_EQ(panes_->paneAt(7), nullptr) << "a pane that is not there is not a pane";
}

TEST_F(WorkspacePanesTest, ASplitToTheRightMakesASecondPane) {
    panes_->split(Qt::Horizontal);
    EXPECT_EQ(panes_->paneCount(), 2);

    // The two panes sit next to each other in one horizontal splitter.
    auto* row = qobject_cast<QSplitter*>(panes_->paneAt(0)->parentWidget());
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->orientation(), Qt::Horizontal);
    EXPECT_EQ(row->indexOf(panes_->paneAt(0)), 0);
    EXPECT_EQ(row->indexOf(panes_->paneAt(1)), 1);
}

TEST_F(WorkspacePanesTest, SplittingDownUnderAPaneOfARowNestsAColumn) {
    panes_->split(Qt::Horizontal);
    panes_->split(Qt::Vertical);
    ASSERT_EQ(panes_->paneCount(), 3);

    // The second and the third pane share a column, and the column sits in the row.
    auto* column = qobject_cast<QSplitter*>(panes_->paneAt(1)->parentWidget());
    ASSERT_NE(column, nullptr);
    EXPECT_EQ(column->orientation(), Qt::Vertical);
    EXPECT_EQ(column->indexOf(panes_->paneAt(1)), 0);
    EXPECT_EQ(column->indexOf(panes_->paneAt(2)), 1);

    auto* row = qobject_cast<QSplitter*>(column->parentWidget());
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->orientation(), Qt::Horizontal);
    EXPECT_EQ(row->count(), 2) << "the row holds the first pane and the column";
}

TEST_F(WorkspacePanesTest, TileArrangesFourPanesInTwoColumnsOfTwo) {
    panes_->tile();
    EXPECT_EQ(panes_->paneCount(), 4);

    auto* left = qobject_cast<QSplitter*>(panes_->paneAt(0)->parentWidget());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->orientation(), Qt::Vertical);
    EXPECT_EQ(left->count(), 2);
    EXPECT_EQ(left->indexOf(panes_->paneAt(0)), 0);
    EXPECT_EQ(left->indexOf(panes_->paneAt(1)), 1);

    auto* right = qobject_cast<QSplitter*>(panes_->paneAt(2)->parentWidget());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->orientation(), Qt::Vertical);
    EXPECT_EQ(right->count(), 2);

    auto* root = qobject_cast<QSplitter*>(left->parentWidget());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->orientation(), Qt::Horizontal);
    EXPECT_EQ(root->count(), 2) << "two columns";
}

TEST_F(WorkspacePanesTest, ASessionStaysInThePaneItWasOpenedIn) {
    openLocal("one");
    panes_->split(Qt::Horizontal);
    openLocal("two");

    const int pane_of_one = paneOf(QStringLiteral("one"));
    const int pane_of_two = paneOf(QStringLiteral("two"));
    EXPECT_GE(pane_of_one, 0);
    EXPECT_GE(pane_of_two, 0);
    EXPECT_NE(pane_of_one, pane_of_two) << "the second session went into another pane";

    // A tile moves the panes, not the sessions.
    panes_->tile();
    EXPECT_EQ(paneOf(QStringLiteral("one")), pane_of_one);
    EXPECT_EQ(paneOf(QStringLiteral("two")), pane_of_two);
}

TEST_F(WorkspacePanesTest, TheSessionsKeepTheOrderTheyWereOpenedInAcrossPanes) {
    openLocal("one");
    panes_->split(Qt::Horizontal);
    openLocal("two");
    panes_->split(Qt::Vertical);
    openLocal("three");

    ASSERT_EQ(panes_->sessionCount(), 3);
    const auto& sessions = panes_->sessions();
    ASSERT_EQ(sessions.size(), 3u);
    EXPECT_EQ(sessions[0].id, "one");
    EXPECT_EQ(sessions[1].id, "two");
    EXPECT_EQ(sessions[2].id, "three")
        << "the order the sessions were opened in is the order a broadcast uses";
}

TEST_F(WorkspacePanesTest, ClosingAPaneClosesTheSessionsInIt) {
    openLocal("one");
    panes_->split(Qt::Horizontal);
    openLocal("two");
    ASSERT_EQ(panes_->sessionCount(), 2);

    // The new pane is the active one, so it is the one that goes.
    panes_->closeActivePane();
    EXPECT_EQ(panes_->paneCount(), 1);
    EXPECT_EQ(panes_->sessionCount(), 1);
    EXPECT_EQ(panes_->sessions().front().id, "one");
}

TEST_F(WorkspacePanesTest, ThereIsAlwaysAPaneToWorkIn) {
    panes_->split(Qt::Horizontal);
    panes_->closeActivePane();
    panes_->closeActivePane();
    EXPECT_GE(panes_->paneCount(), 1) << "the workspace never ends up without a pane";
}

TEST_F(WorkspacePanesTest, ClosingASessionLeavesThePaneItWasIn) {
    openLocal("one");
    panes_->split(Qt::Horizontal);
    openLocal("two");

    EXPECT_TRUE(panes_->closeSession(QStringLiteral("two")));
    EXPECT_EQ(panes_->sessionCount(), 1);
    EXPECT_EQ(panes_->paneCount(), 2) << "the pane is still there, it is just empty";
}

TEST_F(WorkspacePanesTest, TheWorkspaceReportsTheTitleOfEverySession) {
    openLocal("one");
    panes_->split(Qt::Horizontal);
    openLocal("two");

    const QStringList titles = panes_->sessionTitles();
    ASSERT_EQ(titles.size(), 2);
    EXPECT_EQ(titles.at(0), QStringLiteral("one"));
    EXPECT_EQ(titles.at(1), QStringLiteral("two"));
}

}  // namespace
}  // namespace openscrt::ui
