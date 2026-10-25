// The broadcast against real SSH servers (P2-A6).
//
// The CommandWindow test next door proves the dispatch order with twenty stand-ins. That
// is the logic. This one is the thing the acceptance criterion actually says: twenty
// concurrent SSH sessions, one command, the same bytes in the same order, and every
// remote really runs it.
//
// The stand-ins cannot show the last part: a sink that records bytes proves the command
// window handed the command out, not that twenty machines ran it. So the twenty sessions
// here are twenty SSH connections to a real server, and the proof is the answer that comes
// back over the wire.
//
// The CI Linux leg starts an sshd on a loopback port and points these variables at it.
// Every other platform has no server and says so instead of pretending to have passed.

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "protocols/ssh/Ssh2Channel.h"
#include "ui/workspace/CommandWindow.h"

namespace openscrt::ui {
namespace {

constexpr std::size_t kSessions = 20;

// The marker the shell prints is not the text that was typed: the remote PTY echoes the
// command back, so a marker that appears in the command would match the echo even if the
// command never ran. The shell gets a format string and prints the marker itself.
constexpr char kCommand[] = "printf 'openscrt-broadcast-%s\\n' ok\r";
constexpr char kMarker[] = "openscrt-broadcast-ok";

// QWidget needs a QApplication, not a QGuiApplication.
const QApplication& application() {
    static QApplication* instance = [] {
        static int argc = 1;
        static char program[] = "command-window-ssh-test";
        static char* argv[] = {program, nullptr};
        return new QApplication(argc, argv);
    }();
    return *instance;
}

class CommandWindowBroadcastSshTest : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { application(); }
};

TEST_F(CommandWindowBroadcastSshTest, ABroadcastReachesTwentyRealSshSessionsInTheSameOrder) {
    const char* port = std::getenv("OPENCRT_TEST_SSH_PORT");
    const char* user = std::getenv("OPENCRT_TEST_SSH_USER");
    const char* key = std::getenv("OPENCRT_TEST_SSH_KEY");
    if (port == nullptr || user == nullptr || key == nullptr) {
        GTEST_SKIP() << "no SSH server configured for this run";
    }

    // The sessions are opened one after the other. A server drops connections that pile up
    // before they authenticate, so the handshakes are not fired in parallel -- twenty of
    // them at once would be refused rather than slow.
    std::vector<std::unique_ptr<protocols::ssh::Ssh2Channel>> sessions;
    for (std::size_t i = 0; i < kSessions; ++i) {
        protocols::ssh::SshConnectionOptions options;
        options.host = "127.0.0.1";
        options.port = static_cast<std::uint16_t>(std::atoi(port));
        options.user = user;
        options.private_key_path = key;
        options.auth_method = protocols::ssh::SshAuthMethod::kPublicKey;
        options.timeout_seconds = 10;
        auto channel = std::make_unique<protocols::ssh::Ssh2Channel>(std::move(options));
        const protocols::TransportResult result = channel->connectAsync();
        ASSERT_TRUE(result.ok) << "session " << i << " did not connect: " << result.error;
        sessions.push_back(std::move(channel));
    }

    CommandWindow window;
    std::vector<std::size_t> order;  // the order the sessions were reached in
    std::vector<std::vector<std::uint8_t>> received(kSessions);  // what each session got
    std::vector<CommandWindow::Sink> sinks;
    for (std::size_t i = 0; i < kSessions; ++i) {
        sinks.push_back([&order, &received, &sessions, i](const std::vector<std::uint8_t>& bytes) {
            order.push_back(i);
            received[i] = bytes;
            // Into the session, not into a stand-in: these are the bytes the remote gets.
            sessions[i]->send(bytes.data(), bytes.size());
        });
    }
    window.setBroadcastSinks(sinks);
    window.setBroadcast(true);
    window.setText(QString::fromUtf8(kCommand));

    const std::size_t recipients = window.send();
    EXPECT_EQ(recipients, kSessions);
    ASSERT_EQ(order.size(), kSessions);
    for (std::size_t i = 0; i < kSessions; ++i) {
        EXPECT_EQ(order[i], i) << "the sessions were reached out of order";
        EXPECT_EQ(received[i], received[0]) << "session " << i << " got different bytes";
    }

    // And the part the stand-ins cannot show: every remote really ran the command. One
    // deadline for all twenty, so a server that never answers costs a minute and not
    // twenty.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::size_t answered = 0;
    for (std::size_t i = 0; i < kSessions; ++i) {
        std::string accumulated;
        std::uint8_t buffer[1024];
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const std::size_t count = sessions[i]->readAvailable(buffer, sizeof(buffer), remaining);
            accumulated.append(reinterpret_cast<char*>(buffer), count);
            if (accumulated.find(kMarker) != std::string::npos) {
                break;
            }
        }
        if (accumulated.find(kMarker) != std::string::npos) {
            ++answered;
        }
        EXPECT_NE(accumulated.find(kMarker), std::string::npos)
            << "session " << i << " never ran the command";
    }
    // The log line the CI step looks for: a green tick does not say how many sessions
    // answered.
    std::cout << "[p2-a6] " << kSessions << " real SSH sessions, " << answered
              << " of them ran the command\n";
}

}  // namespace
}  // namespace openscrt::ui
