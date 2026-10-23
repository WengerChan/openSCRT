// Terminal canvas widget tests (P1-T7).
//
// Headless: QT_QPA_PLATFORM=offscreen gives a real QWidget without a display server, so the
// frame loop, the resize path and the key encoding are all exercised in CI.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QtGlobal>
#include <chrono>
#include <string>
#include <vector>

#include "ui/workspace/TerminalCanvasWidget.h"

namespace openscrt::ui {
namespace {

const QGuiApplication& application() {
    static QGuiApplication* instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "openscrt-widget-tests";
        static char* argv[] = {name, nullptr};
        return new QGuiApplication(argc, argv);
    }();
    return *instance;
}

class CanvasWidgetTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override {
        widget_ = std::make_unique<TerminalCanvasWidget>();
        widget_->resize(800, 480);
    }

    void TearDown() override { widget_.reset(); }

    // Runs the frame loop for a while so the coalescer's timing rules apply.
    void pump(int milliseconds) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::unique_ptr<TerminalCanvasWidget> widget_;
};

TEST_F(CanvasWidgetTest, StartsWithADefaultGrid) {
    EXPECT_EQ(widget_->grid().rows(), 24);
    EXPECT_EQ(widget_->grid().cols(), 80);
    EXPECT_EQ(widget_->frameCount(), 0);
}

TEST_F(CanvasWidgetTest, FeedingBytesUpdatesTheGrid) {
    widget_->feed("hello");
    EXPECT_EQ(widget_->grid().rowText(0), "hello");
}

TEST_F(CanvasWidgetTest, FeedsAreCoalescedIntoOneFrame) {
    for (int i = 0; i < 1000; ++i) {
        widget_->feed("x");
    }
    pump(120);
    // The burst must not produce 1000 frames.
    EXPECT_LE(widget_->frameCount(), 20);
    EXPECT_GT(widget_->coalescedEventCount(), 0);
}

TEST_F(CanvasWidgetTest, NoFramesWithoutOutput) {
    pump(120);
    EXPECT_EQ(widget_->frameCount(), 0) << "an idle terminal must not repaint";
}

TEST_F(CanvasWidgetTest, ResizeAdaptsTheGridToTheWidget) {
    const auto metrics = widget_->renderer().cellMetrics();
    ASSERT_GT(metrics.width, 0);
    ASSERT_GT(metrics.height, 0);
    widget_->resize(metrics.width * 40, metrics.height * 10);

    const auto size = widget_->resizeToWidget();
    EXPECT_EQ(size.rows, 10);
    EXPECT_EQ(size.columns, 40);
    EXPECT_EQ(widget_->grid().rows(), 10);
    EXPECT_EQ(widget_->grid().cols(), 40);
}

// ------------------------------------------------------------------------- key encoding

TEST(CanvasKeyEncodingTest, PrintableTextIsSentAsIs) {
    const auto bytes =
        TerminalCanvasWidget::encodeKey(Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 'a');
}

TEST(CanvasKeyEncodingTest, EnterIsCarriageReturn) {
    const auto bytes =
        TerminalCanvasWidget::encodeKey(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], '\r');
}

TEST(CanvasKeyEncodingTest, BackspaceAndTabAndEscape) {
    EXPECT_EQ(TerminalCanvasWidget::encodeKey(Qt::Key_Backspace, Qt::NoModifier, QString())[0],
              0x7F);
    EXPECT_EQ(TerminalCanvasWidget::encodeKey(Qt::Key_Tab, Qt::NoModifier, QString())[0], '\t');
    EXPECT_EQ(TerminalCanvasWidget::encodeKey(Qt::Key_Escape, Qt::NoModifier, QString())[0], 0x1B);
}

TEST(CanvasKeyEncodingTest, ArrowKeysUseCsiSequences) {
    const auto up = TerminalCanvasWidget::encodeKey(Qt::Key_Up, Qt::NoModifier, QString());
    ASSERT_EQ(up.size(), 3u);
    EXPECT_EQ(up[0], 0x1B);
    EXPECT_EQ(up[1], '[');
    EXPECT_EQ(up[2], 'A');

    const auto left = TerminalCanvasWidget::encodeKey(Qt::Key_Left, Qt::NoModifier, QString());
    ASSERT_EQ(left.size(), 3u);
    EXPECT_EQ(left[2], 'D');
}

TEST(CanvasKeyEncodingTest, FunctionAndNavigationKeysUseTildeSequences) {
    const auto del = TerminalCanvasWidget::encodeKey(Qt::Key_Delete, Qt::NoModifier, QString());
    ASSERT_EQ(del.size(), 4u);
    EXPECT_EQ(del[1], '[');
    EXPECT_EQ(del[2], '3');
    EXPECT_EQ(del[3], '~');
}

TEST(CanvasKeyEncodingTest, ControlLettersBecomeControlCodes) {
    const auto ctrl_c =
        TerminalCanvasWidget::encodeKey(Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    EXPECT_TRUE(ctrl_c.empty()) << "Ctrl+C is a session action, not bytes to send";

    const auto ctrl_a =
        TerminalCanvasWidget::encodeKey(Qt::Key_A, Qt::ControlModifier, QStringLiteral("a"));
    ASSERT_EQ(ctrl_a.size(), 1u);
    EXPECT_EQ(ctrl_a[0], 1u) << "Ctrl+A is 0x01";
}

TEST(CanvasKeyEncodingTest, UnmappedKeysProduceNothing) {
    EXPECT_TRUE(TerminalCanvasWidget::encodeKey(Qt::Key_F5, Qt::NoModifier, QString()).empty());
}

TEST_F(CanvasWidgetTest, KeyPressesReachTheInputSink) {
    std::vector<std::uint8_t> received;
    widget_->setInputSink([&received](const std::vector<std::uint8_t>& bytes) {
        received.insert(received.end(), bytes.begin(), bytes.end());
    });

    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    widget_->keyPressEvent(&press);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], 'a');
}

TEST_F(CanvasWidgetTest, CtrlCIsReportedAsAnInterrupt) {
    bool interrupted = false;
    QObject::connect(widget_.get(), &TerminalCanvasWidget::interruptRequested,
                     [&interrupted] { interrupted = true; });

    QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    widget_->keyPressEvent(&press);
    EXPECT_TRUE(interrupted);
}

}  // namespace
}  // namespace openscrt::ui