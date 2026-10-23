// Local shell channel tests (P1-T10).
//
// This is the first end-to-end path in the project: bytes cross a real PTY, come back
// through the lock-free ring, and drive the terminal emulator. It is also the only place the
// roadmap's "local shell works" claim (P1-A1) can be checked without a human at a keyboard.
//
// The same suite runs on both platforms: POSIX uses forkpty and /bin/sh, Windows uses
// ConPTY and cmd.exe. The ConPTY path used to be skipped here on the grounds that it was
// not compiled yet, which stopped being true a while ago -- and a path that no test
// touches is a path whose bugs nobody finds.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "protocols/local/LocalPtyChannel.h"
#include "terminal/emulator/TerminalEmulator.h"

namespace openscrt::protocols::local {
namespace {

namespace {

#if defined(_WIN32)

// ConPTY: cmd.exe is the shell that is certainly there. The interactive form prints a
// prompt, which the tests do not rely on -- they look for a marker.
LocalPtyChannel::Options shellOptions() {
    LocalPtyChannel::Options options;
    options.shell = "cmd.exe";
    options.arguments = {};
    options.ring_capacity = 64 * 1024;
    return options;
}

constexpr char kShellName[] = "cmd.exe";

#else

LocalPtyChannel::Options shellOptions() {
    LocalPtyChannel::Options options;
    options.shell = "/bin/sh";
    options.arguments = {"-i"};  // interactive: prints a prompt we can wait for
    options.ring_capacity = 64 * 1024;
    return options;
}

constexpr char kShellName[] = "/bin/sh";

#endif

// Reads until `needle` appears or the budget runs out. A shell's output is not framed, so a
// test that expects an exact byte count would be testing the scheduler, not the channel.
std::string readUntil(LocalPtyChannel& channel, const std::string& needle,
                      std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    std::string accumulated;
    std::uint8_t buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::size_t count = channel.readAvailable(buffer, sizeof(buffer), remaining);
        accumulated.append(reinterpret_cast<char*>(buffer), count);
        if (accumulated.find(needle) != std::string::npos) {
            break;
        }
        if (count == 0 && channel.state() != TransportState::kConnected) {
            break;
        }
    }
    return accumulated;
}

}  // namespace

TEST(LocalPtyChannelTest, AMissingShellFailsCleanly) {
    // A shell that does not exist must fail cleanly rather than aborting or hanging.
    LocalPtyChannel::Options options;
    options.shell = "openscrt-no-such-shell";
    LocalPtyChannel channel{options};

    const TransportResult result = channel.connectAsync();
#if defined(_WIN32)
    // CreateProcess fails outright, so the caller learns about it at once.
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty()) << "a failure must say why";
#else
    // POSIX forks first and execs in the child, so the failure shows up as the session
    // ending rather than as a failed connect. What matters is that it ends, and quickly.
    if (result.ok) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (channel.state() == TransportState::kConnected &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        EXPECT_NE(channel.state(), TransportState::kConnected)
            << "a shell that cannot be executed must not look like a live session";
    }
#endif
    EXPECT_EQ(channel.kind(), TransportKind::kLocalPty);
    EXPECT_TRUE(channel.disconnect().ok);
}

TEST(LocalPtyChannelTest, ConnectStartsAWorkingShell) {
    LocalPtyChannel channel{shellOptions()};
    const TransportResult result = channel.connectAsync();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(channel.state(), TransportState::kConnected);
    EXPECT_EQ(channel.kind(), TransportKind::kLocalPty);

    // Liveness is a command round-trip, not a prompt. Whether an interactive shell prints a
    // prompt, and whether its line editor consumes one, is the shell's business -- the
    // channel only has to carry bytes in both directions.
    channel.send("echo alive\r");
    EXPECT_NE(readUntil(channel, "alive").find("alive"), std::string::npos);
}

TEST(LocalPtyChannelTest, ReportsItsRemoteAddress) {
    LocalPtyChannel channel{shellOptions()};
    EXPECT_EQ(channel.remoteAddress(), std::string("local:") + kShellName);
}

TEST(LocalPtyChannelTest, RunsACommandAndReturnsItsOutput) {
    LocalPtyChannel channel{shellOptions()};
    ASSERT_TRUE(channel.connectAsync().ok);

    // Raw mode: Enter is CR, not LF -- the same reason a real terminal emits CR.
    channel.send("echo openscrt-marker\r");
    const std::string output = readUntil(channel, "openscrt-marker");
    EXPECT_NE(output.find("openscrt-marker"), std::string::npos);
}

TEST(LocalPtyChannelTest, NonInteractiveModeReturnsCleanOutput) {
    // Without a prompt the test can assert on the content, which is the stronger check.
    LocalPtyChannel::Options options;
#if defined(_WIN32)
    options.shell = "cmd.exe";
    options.arguments = {"/c", "echo openscrt-exact"};
#else
    options.shell = "/bin/sh";
    options.arguments = {"-c", "echo openscrt-exact; exit"};
#endif
    LocalPtyChannel channel{options};

    ASSERT_TRUE(channel.connectAsync().ok);
    const std::string output = readUntil(channel, "openscrt-exact");
#if defined(_WIN32)
    // ConPTY interleaves the console's cursor sequences with the text, so the exact
    // comparison is POSIX's: there the PTY is in raw mode and the output is exactly the
    // command's. Here the marker is what matters.
    EXPECT_NE(output.find("openscrt-exact"), std::string::npos);
#else
    // Raw mode has OPOST off, so the PTY no longer expands \n to \r\n. A terminal
    // application is responsible for its own line endings.
    EXPECT_EQ(output, "openscrt-exact\n");
#endif
}

#if defined(_WIN32)
// Temporary diagnostic (2026-10-24): the disconnect path hung in two of the last four CI
// runs, always right after this scenario. Repeating it turns a race that shows up once in
// four runs into one that shows up in one run out of one.
TEST(LocalPtyChannelTest, RepeatedNonInteractiveRunsDoNotHang) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        LocalPtyChannel::Options options;
        options.shell = "cmd.exe";
        options.arguments = {"/c", "echo openscrt-exact"};
        LocalPtyChannel channel{options};

        ASSERT_TRUE(channel.connectAsync().ok);
        const std::string output =
            readUntil(channel, "openscrt-exact", std::chrono::milliseconds(5000));
        EXPECT_NE(output.find("openscrt-exact"), std::string::npos) << "attempt " << attempt;
    }
}
#endif

TEST(LocalPtyChannelTest, DataCallbackSeesTheSameBytesAsTheRing) {
    LocalPtyChannel channel{shellOptions()};
    std::string via_callback;
    channel.setDataCallback([&via_callback](const std::uint8_t* data, std::size_t size) {
        via_callback.append(reinterpret_cast<const char*>(data), size);
    });
    ASSERT_TRUE(channel.connectAsync().ok);
    channel.send("echo callback-path\r");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (via_callback.find("callback-path") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_NE(via_callback.find("callback-path"), std::string::npos);
}

TEST(LocalPtyChannelTest, ResizeIsAcceptedWhileConnected) {
    LocalPtyChannel channel{shellOptions()};
    EXPECT_FALSE(channel.resizeWindow(WindowSize{10, 20})) << "no session yet";
    ASSERT_TRUE(channel.connectAsync().ok);
    EXPECT_TRUE(channel.resizeWindow(WindowSize{40, 120}));
}

TEST(LocalPtyChannelTest, DisconnectIsIdempotentAndStopsTheChild) {
    LocalPtyChannel channel{shellOptions()};
    ASSERT_TRUE(channel.connectAsync().ok);
    EXPECT_TRUE(channel.disconnect().ok);
    EXPECT_EQ(channel.state(), TransportState::kDisconnected);
    EXPECT_TRUE(channel.disconnect().ok) << "a second disconnect must be a no-op, not an error";
}

TEST(LocalPtyChannelTest, ConnectTwiceIsANoOp) {
    LocalPtyChannel channel{shellOptions()};
    ASSERT_TRUE(channel.connectAsync().ok);
    const int first = channel.processIdForTesting();
    ASSERT_GT(first, 0);
    EXPECT_TRUE(channel.connectAsync().ok);
    EXPECT_EQ(channel.processIdForTesting(), first) << "must not spawn a second shell";
}

TEST(LocalPtyChannelTest, DrivesTheTerminalEmulatorEndToEnd) {
    // The point of the exercise: PTY -> ring -> parser -> grid.
    LocalPtyChannel channel{shellOptions()};
    ::openscrt::terminal::ScreenGrid grid{10, 80};
    ::openscrt::terminal::TerminalEmulator emulator{grid};

    ASSERT_TRUE(channel.connectAsync().ok);
    channel.send("echo end-to-end\r");

    std::uint8_t buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    bool matched = false;
    while (!matched && std::chrono::steady_clock::now() < deadline) {
        const std::size_t count =
            channel.readAvailable(buffer, sizeof(buffer), std::chrono::milliseconds(200));
        if (count > 0) {
            emulator.write(buffer, count);
        }
        for (int row = 0; row < grid.rows(); ++row) {
            if (grid.rowText(row).find("end-to-end") != std::string::npos) {
                matched = true;
                break;
            }
        }
    }
    EXPECT_TRUE(matched) << "command output never reached the screen grid";
}

}  // namespace
}  // namespace openscrt::protocols::local
