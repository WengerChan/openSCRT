// The button bar (P2-T10).
//
// The bar is built by the user, so the parts that can be checked without a human are the
// ones that matter: what a send string expands to, that the groups survive a trip
// through the session file, and that a button sends what it says it sends.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <memory>
#include <string>
#include <vector>

#include "config/ButtonBarModel.h"
#include "config/ConfigManager.h"
#include "ui/workspace/ButtonBar.h"

namespace openscrt::ui {
namespace {

const QApplication& application() {
    static QApplication* instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "openscrt-buttonbar-tests";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return *instance;
}

std::string bytesOf(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class ButtonBarTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }

    void SetUp() override {
        bar_ = std::make_unique<ButtonBar>();
        // The pause is shortened so the test does not wait a second for it.
        bar_->setPauseDuration(1);
    }

    std::unique_ptr<ButtonBar> bar_;
};

TEST_F(ButtonBarTest, TheSecureCrtEscapesAreExpanded) {
    const auto steps = expandButtonSendString(QStringLiteral("ls -la\\r"), QString(), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "ls -la\r");
    EXPECT_EQ(steps[0].pause_ms, 0);
}

TEST_F(ButtonBarTest, ALineFeedIsItsOwnThing) {
    const auto steps = expandButtonSendString(QStringLiteral("a\\nb"), QString(), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "a\nb");
}

TEST_F(ButtonBarTest, ThePauseIsAStepOfItsOwn) {
    // A pause in the middle of a string: what came before it goes out first, and the
    // pause is attached to what comes after it.
    const auto steps = expandButtonSendString(QStringLiteral("first\\psecond"), QString(), 1000);
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "first");
    EXPECT_EQ(steps[0].pause_ms, 0);
    EXPECT_EQ(bytesOf(steps[1].bytes), "second");
    EXPECT_EQ(steps[1].pause_ms, 1000) << "a pause is the SecureCRT second";
}

TEST_F(ButtonBarTest, TheClipboardIsPasted) {
    const auto steps =
        expandButtonSendString(QStringLiteral("echo \\v\\r"), QStringLiteral("pasted"), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "echo pasted\r");
}

TEST_F(ButtonBarTest, ABackslashIsABackslash) {
    const auto steps = expandButtonSendString(QStringLiteral("a\\\\b"), QString(), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "a\\b");
}

TEST_F(ButtonBarTest, AnUnknownEscapeIsPassedOnAsItWasTyped) {
    const auto steps = expandButtonSendString(QStringLiteral("a\\qb"), QString(), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "a\\qb");
}

TEST_F(ButtonBarTest, ABackslashAtTheEndIsABackslash) {
    const auto steps = expandButtonSendString(QStringLiteral("a\\"), QString(), 1000);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(bytesOf(steps[0].bytes), "a\\");
}

TEST_F(ButtonBarTest, TheGroupsSurviveTheSessionFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = std::string(QDir(dir.path()).filePath("sessions.ini").toStdString());

    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.type = config::ButtonActionType::kSendString;
    action.label = "uptime";
    action.value = "uptime\\r";
    ASSERT_TRUE(model.addButton(group, action));

    config::ButtonAction other;
    other.type = config::ButtonActionType::kOpenSession;
    other.label = "jump";
    other.value = "jump-host";
    ASSERT_TRUE(model.addButton(group, other));

    ASSERT_TRUE(config::ConfigManager::saveButtonBar(model, path));
    const config::ButtonBarModel loaded = config::ConfigManager::loadButtonBar(path);

    ASSERT_EQ(loaded.groups.size(), 1u);
    EXPECT_EQ(loaded.groups[0].name, "Servers");
    ASSERT_EQ(loaded.groups[0].buttons.size(), 2u);
    EXPECT_EQ(loaded.groups[0].buttons[0].type, config::ButtonActionType::kSendString);
    EXPECT_EQ(loaded.groups[0].buttons[0].label, "uptime");
    EXPECT_EQ(loaded.groups[0].buttons[0].value, "uptime\\r");
    EXPECT_EQ(loaded.groups[0].buttons[1].type, config::ButtonActionType::kOpenSession);
    EXPECT_EQ(loaded.groups[0].buttons[1].value, "jump-host");
}

TEST_F(ButtonBarTest, AButtonThatWasRemovedDoesNotLingerInTheFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = std::string(QDir(dir.path()).filePath("sessions.ini").toStdString());

    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.label = "uptime";
    action.value = "uptime\\r";
    ASSERT_TRUE(model.addButton(group, action));
    ASSERT_TRUE(config::ConfigManager::saveButtonBar(model, path));

    ASSERT_TRUE(model.removeButton(group, 0));
    ASSERT_TRUE(config::ConfigManager::saveButtonBar(model, path));

    const config::ButtonBarModel loaded = config::ConfigManager::loadButtonBar(path);
    ASSERT_EQ(loaded.groups.size(), 1u);
    EXPECT_TRUE(loaded.groups[0].buttons.empty());
}

TEST_F(ButtonBarTest, AGroupBecomesATab) {
    config::ButtonBarModel model;
    model.addGroup("Servers");
    model.addGroup("Switches");
    bar_->setModel(model);

    EXPECT_EQ(bar_->groupCount(), 2);
    bar_->setCurrentGroup(1);
    EXPECT_EQ(bar_->currentGroup(), 1);
}

TEST_F(ButtonBarTest, AButtonSendsWhatItSays) {
    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.type = config::ButtonActionType::kSendString;
    action.label = "uptime";
    action.value = "uptime\\r";
    ASSERT_TRUE(model.addButton(group, action));
    bar_->setModel(model);

    std::string sent;
    bar_->setSender([&sent](const std::uint8_t* data, std::size_t size) {
        sent.append(reinterpret_cast<const char*>(data), size);
    });

    ASSERT_TRUE(bar_->trigger(0, 0));
    EXPECT_EQ(sent, "uptime\r");
    EXPECT_EQ(bar_->buttonText(0, 0), QStringLiteral("uptime"));
}

TEST_F(ButtonBarTest, AButtonWithoutACaptionShowsItsValue) {
    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.value = "uptime\\r";
    ASSERT_TRUE(model.addButton(group, action));
    bar_->setModel(model);

    EXPECT_EQ(bar_->buttonText(0, 0), QStringLiteral("uptime\\r"));
}

TEST_F(ButtonBarTest, AButtonThatOpensASessionAsksForIt) {
    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.type = config::ButtonActionType::kOpenSession;
    action.label = "jump";
    action.value = "jump-host";
    ASSERT_TRUE(model.addButton(group, action));
    bar_->setModel(model);

    QString asked;
    QObject::connect(bar_.get(), &ButtonBar::openSessionRequested,
                     [&asked](const QString& id) { asked = id; });

    ASSERT_TRUE(bar_->trigger(0, 0));
    EXPECT_EQ(asked, QStringLiteral("jump-host"));
}

TEST_F(ButtonBarTest, AButtonThatIsNotThereDoesNothing) {
    config::ButtonBarModel model;
    model.addGroup("Servers");
    bar_->setModel(model);

    bool asked = false;
    QObject::connect(bar_.get(), &ButtonBar::openSessionRequested,
                     [&asked](const QString&) { asked = true; });

    EXPECT_FALSE(bar_->trigger(0, 0)) << "the group has no buttons";
    EXPECT_FALSE(bar_->trigger(5, 0)) << "the group is not there";
    EXPECT_FALSE(asked);
}

TEST_F(ButtonBarTest, ThePauseWaitsBetweenTheSteps) {
    config::ButtonBarModel model;
    const std::size_t group = model.addGroup("Servers");
    config::ButtonAction action;
    action.value = "one\\ptwo";
    ASSERT_TRUE(model.addButton(group, action));
    bar_->setModel(model);

    std::vector<std::string> sent;
    bar_->setSender([&sent](const std::uint8_t* data, std::size_t size) {
        sent.emplace_back(reinterpret_cast<const char*>(data), size);
    });

    ASSERT_TRUE(bar_->trigger(0, 0));
    ASSERT_EQ(sent.size(), 1u) << "the second half waits for the pause";
    EXPECT_EQ(sent[0], "one");

    // The pause is a millisecond long in this test, so the event loop only has to run a
    // moment for the second half to go out.
    for (int i = 0; i < 200 && sent.size() < 2; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1], "two");
}

}  // namespace
}  // namespace openscrt::ui
