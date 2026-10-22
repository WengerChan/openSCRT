// Local shell channel tests (P1-T10).
//
// This is the first end-to-end path in the project: bytes cross a real PTY, come back
// through the lock-free ring, and drive the terminal emulator. It is also the only place the
// roadmap's "local shell works" claim (P1-A1) can be checked without a human at a keyboard.
//
// POSIX-only in practice -- ConPTY is not compiled yet, so the Windows leg skips. That is
// recorded as a known gap rather than hidden behind an unconditional pass.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "protocols/local/LocalPtyChannel.h"
#include "terminal/emulator/TerminalEmulator.h"

#if defined(_WIN32)
#define OPENSCRT_LOCAL_PTY_UNSUPPORTED 1
#endif

namespace openscrt::protocols::local {
namespace {

#if defined(OPENSCRT_LOCAL_PTY_UNSUPPORTED)

TEST(LocalPtyChannelTest, ConPtyNotYetImplemented) {
    LocalPtyChannel channel;
    const TransportResult result = channel.connectAsync();
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("ConPTY"), std::string::npos);
    EXPECT_EQ(channel.kind(), TransportKind::kLocalPty);
}

#else

namespace {

LocalPtyChannel::Options shellOptions() {
    LocalPtyChannel::Options options;
    options.shell = "/bin/sh";
    options.arguments = {"-i"};  // interactive: prints a prompt we can wait for
    options.ring_capacity = 64 * 1024;
    return options;
}

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

TEST(LocalPtyChannelTest, ConnectStartsAWorkingShell) {
    LocalPtyChannel channel{shellOptions()};
    const TransportResult result = channel.connectAsync();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(channel.state(), TransportState::kConnected);
    EXPECT_EQ(channel.kind(), TransportKind::kLocalPty);
    EXPECT_NE(readUntil(channel, "$"), "");
}

TEST(LocalPtyChannelTest, ReportsItsRemoteAddress) {
    LocalPtyChannel channel{shellOptions()};
    EXPECT_EQ(channel.remoteAddress(), "local:/bin/sh");
}

TEST(LocalPtyChannelTest, RunsACommandAndReturnsItsOutput) {
    LocalPtyChannel channel{shellOptions()};
    ASSERT_TRUE(channel.connectAsync().ok);
    readUntil(channel, "$");

    channel.send("echo openscrt-marker\n");
    const std::string output = readUntil(channel, "openscrt-marker");
    EXPECT_NE(output.find("openscrt-marker"), std::string::npos);
}

TEST(LocalPtyChannelTest, NonInteractiveModeReturnsCleanOutput) {
    // Without a prompt the test can assert on exact content, which is the stronger check.
    LocalPtyChannel::Options options;
    options.shell = "/bin/sh";
    options.arguments = {"-c", "echo openscrt-exact; exit"};
    LocalPtyChannel channel{options};

    ASSERT_TRUE(channel.connectAsync().ok);
    EXPECT_EQ(readUntil(channel, "openscrt-exact"), "openscrt-exact\r\n");
}

TEST(LocalPtyChannelTest, DataCallbackSeesTheSameBytesAsTheRing) {
    LocalPtyChannel channel{shellOptions()};
    std::string via_callback;
    channel.setDataCallback([&via_callback](const std::uint8_t* data, std::size_t size) {
        via_callback.append(reinterpret_cast<const char*>(data), size);
    });
    ASSERT_TRUE(channel.connectAsync().ok);

    readUntil(channel, "$");
    channel.send("echo callback-path\n");

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
    readUntil(channel, "$");

    channel.send("echo end-to-end\n");

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

#endif  // _WIN32

}  // namespace
}  // namespace openscrt::protocols::local