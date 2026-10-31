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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
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
    // implementation computes for the string "123456789".
    //
    // The value is 0xBEEF and not the 0x31C3 of the textbook CRC-16 of the same
    // polynomial: the protocol writes the byte into the low bits of the result and
    // enters the table with the high byte of the checksum, and only that form lets
    // a frame's checksum come out zero when it is read back with the checksum
    // appended -- which is how the far end checks a frame.
    std::uint16_t crc = 0;
    for (const char character : std::string("123456789")) {
        crc = modem::crc16(crc, static_cast<std::uint8_t>(character));
    }
    EXPECT_EQ(crc, 0xBEEF);
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

TEST(ZmodemFrameTest, AHeaderSurvivesTheRoundTrip) {
    // Every type of the protocol, in both forms. This is the test that catches a
    // mistake in the digits of a hex header: the whole transfer takes ten minutes to
    // fail, and the arithmetic is a line.
    for (int type = 0; type <= 19; ++type) {
        modem::ZmodemHeader header;
        header.frame = static_cast<modem::ZmodemFrame>(type);
        header.setPosition(0x12345678);

        const std::vector<std::uint8_t> hex = modem::encodeHexHeader(header);
        modem::ZmodemHeader decoded;
        bool crc32_frame = false;
        std::size_t used = 0;
        ASSERT_TRUE(modem::decodeHeader(hex.data(), hex.size(), decoded, crc32_frame, used));
        EXPECT_EQ(used, hex.size());
        EXPECT_EQ(static_cast<int>(decoded.frame), type);
        EXPECT_EQ(decoded.position(), header.position());
        for (int index = 0; index < 4; ++index) {
            EXPECT_EQ(decoded.bytes[index], header.bytes[index]);
        }

        const std::vector<std::uint8_t> binary = modem::encodeBinaryHeader(header, false);
        ASSERT_TRUE(modem::decodeHeader(binary.data(), binary.size(), decoded, crc32_frame, used));
        EXPECT_EQ(used, binary.size());
        EXPECT_EQ(static_cast<int>(decoded.frame), type);
        EXPECT_EQ(decoded.position(), header.position());
    }
}

TEST(ZmodemFrameTest, AHeaderThatIsCutInHalfIsNotDecoded) {
    modem::ZmodemHeader header;
    header.frame = modem::ZmodemFrame::kResume;
    const std::vector<std::uint8_t> bytes = modem::encodeHexHeader(header);

    modem::ZmodemHeader decoded;
    bool crc32_frame = false;
    std::size_t used = 0;
    EXPECT_FALSE(modem::decodeHeader(bytes.data(), bytes.size() - 1, decoded, crc32_frame, used));
}

TEST(ZmodemFrameTest, TheChecksumOfAHeaderIsChecked) {
    modem::ZmodemHeader header;
    header.frame = modem::ZmodemFrame::kResume;
    std::vector<std::uint8_t> bytes = modem::encodeHexHeader(header);
    // Break the checksum: one of the digits of it.
    bytes[14] = bytes[14] == '0' ? '1' : '0';

    modem::ZmodemHeader decoded;
    bool crc32_frame = false;
    std::size_t used = 0;
    EXPECT_FALSE(modem::decodeHeader(bytes.data(), bytes.size(), decoded, crc32_frame, used));
}

#if defined(__linux__)

namespace {

// Whether the reference implementation of the protocol is on this machine.
bool haveLrzsz() {
    return std::system("command -v sz > /dev/null 2>&1") == 0;
}

std::string writeTemporaryFile(const std::string& name, const std::string& content) {
    // The file the reference implementation sends lives in a directory of its
    // own.
    //
    // The receiver writes the file it is sent into the directory it downloads
    // into, under the last part of the name it was sent with -- and the last
    // part of the name is the same for the source and for the file that comes
    // out. A source in the download directory would be cut short by the
    // receiver while the sender is still reading it.
    const std::filesystem::path directory = "/tmp/openscrt-zmodem-source";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path path = directory / name;
    std::ofstream writer(path, std::ios::binary | std::ios::trunc);
    writer.write(content.data(), static_cast<std::streamsize>(content.size()));
    writer.close();
    return path.string();
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

    openscrt::protocols::local::LocalPtyChannel::Options options;
    options.shell = "sz";
    options.arguments = {source};
    openscrt::protocols::local::LocalPtyChannel sender(options);
    ASSERT_TRUE(sender.connectAsync().ok);

    modem::XyzModemEngine::Options engine_options;
    engine_options.download_directory = "/tmp";
    modem::XyzModemEngine engine(engine_options);

    std::vector<std::uint8_t> buffer(64 * 1024);
    // A probe: the bytes that arrive, kept so that the place where the engine went
    // wrong can be looked at byte for byte. It goes away with the bug it was put in for.
    std::ofstream wire("/tmp/openscrt-zmodem-wire.bin", std::ios::binary | std::ios::trunc);
    std::vector<std::uint8_t> wire_memory;
    wire_memory.reserve(2 * 1024 * 1024);
    // The deadline scales with the size of the file: the transfer is a round trip
    // every eight kilobytes, and the deadline has to be long enough for the
    // biggest file the criterion names. It is capped, so that a transfer that
    // hangs does not hang the job for an hour.
    const long long megabytes = static_cast<long long>(size) / (1024 * 1024) + 1;
    // The budget has to fit inside the timeout of the test itself, so that a
    // transfer that does not finish says so instead of being killed.
    const long long budget = std::min<long long>(120 + megabytes * 12, 540);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(budget);
    while (engine.state() != modem::ModemState::kFinished &&
           std::chrono::steady_clock::now() < deadline) {
        const std::size_t count =
            sender.readAvailable(buffer.data(), buffer.size(), std::chrono::milliseconds(200));
        if (count == 0) {
            continue;
        }
        if (wire_memory.size() < 2 * 1024 * 1024) {
            wire.write(reinterpret_cast<const char*>(buffer.data()),
                       static_cast<std::streamsize>(count));
            wire_memory.insert(wire_memory.end(), buffer.begin(), buffer.begin() + count);
        }
        const std::vector<std::uint8_t> answer = engine.feed(buffer.data(), count);
        if (!answer.empty()) {
            sender.send(answer.data(), answer.size());
        }
    }
    wire.close();

    EXPECT_EQ(engine.badFrames(), 0u) << "frames arrived with a checksum that does not match";
    if (engine.sawBadFrame()) {
        // The bytes around the frame whose checksum did not match. Without them
        // "the transfer stopped" says nothing about why.
        const std::size_t at = engine.badFrameOffset();
        const std::size_t from = at > 120 ? at - 120 : 0;
        const std::size_t to = std::min(wire_memory.size(), at + 120);
        std::fprintf(stderr, "zmodem probe: the first bad frame is at byte %zu of %zu\n", at,
                     wire_memory.size());
        std::fprintf(stderr, "zmodem probe: the bytes around it:");
        for (std::size_t index = from; index < to; ++index) {
            std::fprintf(stderr, " %02x", wire_memory[index]);
        }
        std::fprintf(stderr, "\n");
    }

    EXPECT_EQ(engine.state(), modem::ModemState::kFinished) << engine.error();
    EXPECT_TRUE(engine.receivedPath().empty() == false) << "no file was received";

    const std::string received = readFile(engine.receivedPath());
    EXPECT_EQ(received.size(), payload.size());
    if (received.size() == payload.size()) {
        // The bytes are compared one by one, and only the first difference is
        // reported. A failure that prints two hundred megabytes fills the log of the
        // run and hides everything else in it.
        const auto difference = std::mismatch(received.begin(), received.end(), payload.begin());
        EXPECT_TRUE(difference.first == difference.second)
            << "the first difference is at byte " << (difference.first - received.begin());
    }

    sender.disconnect();
    std::remove(source.c_str());
    if (!engine.receivedPath().empty()) {
        std::remove(engine.receivedPath().c_str());
    }
    std::error_code error;
    std::filesystem::remove("/tmp/openscrt-zmodem-source", error);
}

#endif  // __linux__
