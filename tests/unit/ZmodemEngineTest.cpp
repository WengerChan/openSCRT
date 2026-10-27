// The Zmodem frames and the engine (P3-T7).
//
// The frames are pure logic and are tested everywhere. The engine is tested against
// the real sender: the reference implementation of the protocol, lrzsz, runs on the
// other end of a pty and sends a file, and the file that arrives is compared with the
// one that went out.
//
// The reference implementation is what makes this evidence. An engine tested against
// itself proves only that it agrees with itself.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "transfer/modem/XyzModemEngine.h"

#if defined(__linux__)
#include <unistd.h>

#include "protocols/local/LocalPtyChannel.h"
#endif

namespace modem = openscrt::transfer::modem;

namespace {

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

std::string textOf(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

// ------------------------------------------------------------------------- frames

TEST(ZmodemFrameTest, TheChecksumIsTheOneOfTheProtocol) {
    // The 16-bit checksum of the protocol, checked against the value the reference
    // implementation publishes for the string "123456789".
    std::uint16_t crc = 0;
    for (const char character : std::string("123456789")) {
        crc = modem::crc16(crc, static_cast<std::uint8_t>(character));
    }
    EXPECT_EQ(crc, 0x31C3);
}

TEST(ZmodemFrameTest, AControlCharacterIsEscaped) {
    std::vector<std::uint8_t> out;
    const std::vector<std::uint8_t> payload = {0x00, 0x41, 0x1B, 0x7F};
    modem::appendEscaped(out, payload.data(), payload.size());

    // The escape is the ZDLE of the protocol followed by the byte with its top two
    // bits flipped.
    ASSERT_EQ(out.size(), 6u);
    EXPECT_EQ(out[0], 0x18);
    EXPECT_EQ(out[1], 0x40);
    EXPECT_EQ(out[2], 'A');
    EXPECT_EQ(out[3], 0x18);
    EXPECT_EQ(out[4], 0x5B);
    EXPECT_EQ(out[5], 0x7F);
}

TEST(ZmodemFrameTest, ATextIsNotEscaped) {
    std::vector<std::uint8_t> out;
    const std::vector<std::uint8_t> payload = bytesOf("hello");
    modem::appendEscaped(out, payload.data(), payload.size());
    EXPECT_EQ(textOf(out), "hello");
}

TEST(ZmodemFrameTest, AHeaderCarriesItsTypeAndItsBytes) {
    modem::ZmodemHeader header;
    header.frame = modem::ZmodemFrame::kResume;
    header.setPosition(0x12345678);

    const std::vector<std::uint8_t> bytes = modem::encodeHexHeader(header);
    // The padding, the escape, the marker, the type as two digits, the four bytes as
    // eight digits, the checksum as four digits and the two bytes that end it.
    ASSERT_EQ(bytes.size(), 20u);
    EXPECT_EQ(bytes[0], '*');
    EXPECT_EQ(bytes[1], '*');
    EXPECT_EQ(bytes[2], 0x18);
    EXPECT_EQ(bytes[3], 'B');
    EXPECT_EQ(bytes[4], '0');
    EXPECT_EQ(bytes[5], '9');
    EXPECT_EQ(header.position(), 0x12345678u);
}

TEST(ZmodemFrameTest, ADataFrameEndsWithItsChecksum) {
    const std::vector<std::uint8_t> payload = bytesOf("data");
    const std::vector<std::uint8_t> frame =
        modem::encodeDataFrame(payload.data(), payload.size(), modem::FrameEnd::kEnd);
    // The data, then the escape, the way it ends, and the two bytes of the checksum.
    ASSERT_EQ(frame.size(), payload.size() + 4u);
    EXPECT_EQ(frame[payload.size()], 0x18);
    EXPECT_EQ(frame[payload.size() + 1], 'h');
}

// ------------------------------------------------------------------------- engine

#if defined(__linux__)

namespace {

// Whether the reference implementation of the protocol is on this machine.
bool haveLrzsz() {
    return std::system("command -v sz > /dev/null 2>&1") == 0;
}

std::string writeTemporaryFile(const std::string& name, const std::string& content) {
    const std::string path = "/tmp/" + name;
    std::ofstream writer(path, std::ios::binary | std::ios::trunc);
    writer.write(content.data(), static_cast<std::streamsize>(content.size()));
    writer.close();
    return path;
}

std::string readFile(const std::string& path) {
    std::ifstream reader(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(reader)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(ZmodemEngineTest, AFileArrivesFromTheRealSender) {
    if (!haveLrzsz()) {
        GTEST_SKIP() << "the reference implementation of the protocol is not installed";
    }

    // How big the file is. The criterion names a hundred megabytes, so that is what CI
    // asks for; the default is smaller so that the test is quick on a developer
    // machine.
    std::size_t size = 8 * 1024;
    if (const char* from_environment = std::getenv("OPENCRT_ZMODEM_TEST_BYTES")) {
        const long long asked = std::atoll(from_environment);
        if (asked > 0) {
            size = static_cast<std::size_t>(asked);
        }
    }

    // A file with bytes that are not text, so a transfer that mangles them shows.
    std::string payload;
    payload.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        payload.push_back(static_cast<char>(index % 256));
    }
    const std::string source = writeTemporaryFile("openscrt-zmodem-in.bin", payload);
    const std::string target = writeTemporaryFile("openscrt-zmodem-out.bin", "");

    openscrt::protocols::local::LocalPtyChannel::Options options;
    options.shell = "sz";
    options.arguments = {source};
    openscrt::protocols::local::LocalPtyChannel sender(options);
    ASSERT_TRUE(sender.connectAsync().ok);

    modem::XyzModemEngine::Options engine_options;
    engine_options.download_directory = "/tmp";
    modem::XyzModemEngine engine(engine_options);

    std::vector<std::uint8_t> buffer(64 * 1024);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (engine.state() != modem::ModemState::kFinished &&
           std::chrono::steady_clock::now() < deadline) {
        const std::size_t count =
            sender.readAvailable(buffer.data(), buffer.size(), std::chrono::milliseconds(200));
        if (count == 0) {
            continue;
        }
        const std::vector<std::uint8_t> answer = engine.feed(buffer.data(), count);
        if (!answer.empty()) {
            sender.send(answer.data(), answer.size());
        }
    }

    EXPECT_EQ(engine.state(), modem::ModemState::kFinished) << engine.error();
    EXPECT_TRUE(engine.receivedPath().empty() == false) << "no file was received";

    const std::string received = readFile(engine.receivedPath());
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);

    sender.disconnect();
    std::remove(source.c_str());
    std::remove(target.c_str());
    if (!engine.receivedPath().empty()) {
        std::remove(engine.receivedPath().c_str());
    }
}

#endif  // __linux__
