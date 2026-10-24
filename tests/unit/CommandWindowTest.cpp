// The command window tests (P2-T10).
//
// The acceptance criterion for the broadcast is the interesting part: a command that
// goes to twenty sessions has to reach all twenty, in the same order, with the same
// bytes. "The same order" is the part that is easy to lose and hard to notice -- a
// broadcast that reaches every session but in a different order is not reproducible, and
// a command that means "on every host" has to mean it in the same sequence everywhere.

#include <gtest/gtest.h>

#include <QApplication>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/workspace/CommandWindow.h"

namespace openscrt::ui {
namespace {

// QWidget needs a QApplication, not a QGuiApplication.
const QApplication& application() {
    static QApplication* instance = [] {
        static int argc = 1;
        static char program[] = "command-window-test";
        static char* argv[] = {program, nullptr};
        return new QApplication(argc, argv);
    }();
    return *instance;
}

class CommandWindowFixture : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }
};

TEST_F(CommandWindowFixture, ABroadcastReachesTwentySessionsInTheSameOrder) {
    constexpr std::size_t kSessions = 20;

    CommandWindow window;
    std::vector<std::string> order;                // the order the sessions were reached in
    std::vector<std::string> received(kSessions);  // what each session got

    std::vector<CommandWindow::Sink> sinks;
    for (std::size_t i = 0; i < kSessions; ++i) {
        sinks.push_back([&order, &received, i](const std::vector<std::uint8_t>& bytes) {
            order.push_back(std::to_string(i));
            received[i].assign(bytes.begin(), bytes.end());
        });
    }
    window.setBroadcastSinks(sinks);
    window.setBroadcast(true);
    window.setText(QStringLiteral("uptime"));

    const std::size_t recipients = window.send();
    EXPECT_EQ(recipients, kSessions);
    EXPECT_EQ(order.size(), kSessions);
    for (std::size_t i = 0; i < kSessions; ++i) {
        EXPECT_EQ(order[i], std::to_string(i)) << "the sessions were reached out of order";
        EXPECT_EQ(received[i], "uptime\n") << "session " << i << " got something else";
    }
    // The line edit is cleared, so the same command is not sent twice by accident.
    EXPECT_TRUE(window.text().isEmpty());
}

TEST_F(CommandWindowFixture, ACommandThatIsNotABroadcastGoesToTheCurrentSessionOnly) {
    CommandWindow window;
    std::string received;
    std::size_t broadcasts = 0;
    window.setBroadcastSinks({[&broadcasts](const std::vector<std::uint8_t>&) { ++broadcasts; }});
    window.setCurrentSink([&received](const std::vector<std::uint8_t>& bytes) {
        received.assign(bytes.begin(), bytes.end());
    });
    window.setBroadcast(false);
    window.setText(QStringLiteral("whoami"));

    EXPECT_EQ(window.send(), 1u);
    EXPECT_EQ(received, "whoami\n");
    EXPECT_EQ(broadcasts, 0u);
}

TEST_F(CommandWindowFixture, AnEmptyCommandIsNotSent) {
    CommandWindow window;
    std::size_t broadcasts = 0;
    window.setBroadcastSinks({[&broadcasts](const std::vector<std::uint8_t>&) { ++broadcasts; }});
    window.setBroadcast(true);
    window.setText(QString());

    EXPECT_EQ(window.send(), 0u);
    EXPECT_EQ(broadcasts, 0u);
}

TEST_F(CommandWindowFixture, TheBroadcastIsReported) {
    CommandWindow window;
    window.setBroadcastSinks({[](const std::vector<std::uint8_t>&) {}});
    window.setBroadcast(true);
    window.setText(QStringLiteral("ls"));

    QString sent;
    bool broadcast = false;
    std::size_t recipients = 0;
    QObject::connect(&window, &CommandWindow::commandSent,
                     [&sent, &broadcast, &recipients](const QString& text, bool was_broadcast,
                                                      std::size_t count) {
                         sent = text;
                         broadcast = was_broadcast;
                         recipients = count;
                     });
    window.send();
    EXPECT_EQ(sent, QStringLiteral("ls"));
    EXPECT_TRUE(broadcast);
    EXPECT_EQ(recipients, 1u);
}

}  // namespace
}  // namespace openscrt::ui
