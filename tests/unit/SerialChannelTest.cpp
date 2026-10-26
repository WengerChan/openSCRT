// The serial channel and the parameter matrix P3-A2 names (P3-T1).
//
// The matrix is checked in two halves, because they need different things.
//
// The first half is the mapping: every combination of baud rate, data width, parity,
// stop width and flow control has to land on the port as the matching QSerialPort
// property. That half needs no device, so it runs on all three platforms.
//
// The second half is the data path: bytes have to travel through a real port in both
// directions. That needs a virtual serial pair, which is a pty pair on Linux. The
// expectation tables below are written out by hand rather than derived from the
// production mapping, so a copy of the same mistake in both places would still show up.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSerialPort>
#include <chrono>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include "protocols/ITransportChannel.h"
#include "protocols/serial/SerialChannel.h"
#include "protocols/serial/SerialPortSettings.h"
#include "protocols/serial/SerialSettings.h"

namespace serial = openscrt::protocols::serial;

namespace {

// The expectations, written out one by one. These are the statements of intent; the
// production code maps the same values and the test checks that the two agree.
struct ParityCase {
    serial::SerialParity ours;
    QSerialPort::Parity theirs;
};

struct DataBitsCase {
    serial::SerialDataBits ours;
    QSerialPort::DataBits theirs;
};

struct StopBitsCase {
    serial::SerialStopBits ours;
    QSerialPort::StopBits theirs;
};

struct FlowControlCase {
    serial::SerialFlowControl ours;
    QSerialPort::FlowControl theirs;
};

const ParityCase kParityCases[] = {
    {serial::SerialParity::kNone, QSerialPort::NoParity},
    {serial::SerialParity::kEven, QSerialPort::EvenParity},
    {serial::SerialParity::kOdd, QSerialPort::OddParity},
    {serial::SerialParity::kMark, QSerialPort::MarkParity},
    {serial::SerialParity::kSpace, QSerialPort::SpaceParity},
};

const DataBitsCase kDataBitsCases[] = {
    {serial::SerialDataBits::k5, QSerialPort::Data5},
    {serial::SerialDataBits::k6, QSerialPort::Data6},
    {serial::SerialDataBits::k7, QSerialPort::Data7},
    {serial::SerialDataBits::k8, QSerialPort::Data8},
};

const StopBitsCase kStopBitsCases[] = {
    {serial::SerialStopBits::kOne, QSerialPort::OneStop},
    {serial::SerialStopBits::kOneAndHalf, QSerialPort::OneAndHalfStop},
    {serial::SerialStopBits::kTwo, QSerialPort::TwoStop},
};

const FlowControlCase kFlowControlCases[] = {
    {serial::SerialFlowControl::kNone, QSerialPort::NoFlowControl},
    {serial::SerialFlowControl::kRtsCts, QSerialPort::HardwareControl},
    {serial::SerialFlowControl::kXonXoff, QSerialPort::SoftwareControl},
};

// Baud rates, including one that no standard table lists. The roadmap asks for
// non-standard rates, so the matrix has to carry one.
const int kBaudRates[] = {9600, 19200, 38400, 57600, 115200, 12345};

}  // namespace

TEST(SerialSettingsTest, EveryNameIsParsedBackIntoTheValueItNames) {
    for (const auto& entry : kParityCases) {
        EXPECT_EQ(serial::serialParityFromString(serial::toString(entry.ours)), entry.ours);
    }
    for (const auto& entry : kDataBitsCases) {
        EXPECT_EQ(serial::serialDataBitsFromString(serial::toString(entry.ours)), entry.ours);
    }
    for (const auto& entry : kStopBitsCases) {
        EXPECT_EQ(serial::serialStopBitsFromString(serial::toString(entry.ours)), entry.ours);
    }
    for (const auto& entry : kFlowControlCases) {
        EXPECT_EQ(serial::serialFlowControlFromString(serial::toString(entry.ours)), entry.ours);
    }
}

TEST(SerialSettingsTest, ATextThatNamesNoValueIsRejected) {
    EXPECT_FALSE(serial::serialParityFromString("").has_value());
    EXPECT_FALSE(serial::serialParityFromString("weird").has_value());
    EXPECT_FALSE(serial::serialDataBitsFromString("9").has_value());
    EXPECT_FALSE(serial::serialStopBitsFromString("3").has_value());
    EXPECT_FALSE(serial::serialFlowControlFromString("xonxoff").has_value());
}

TEST(SerialSettingsTest, SettingsWithoutAPortOrWithoutARateAreNotValid) {
    serial::SerialSettings settings;
    EXPECT_FALSE(settings.valid());
    EXPECT_FALSE(settings.validationError().empty());

    settings.port_name = "/dev/ttyUSB0";
    EXPECT_TRUE(settings.valid());
    EXPECT_TRUE(settings.validationError().empty());

    settings.baud_rate = 0;
    EXPECT_FALSE(settings.valid());
    EXPECT_FALSE(settings.validationError().empty());

    settings.baud_rate = -9600;
    EXPECT_FALSE(settings.valid());
}

TEST(SerialPortSettingsTest, EveryCombinationOfTheMatrixLandsOnThePort) {
    int combinations = 0;
    for (const int baud : kBaudRates) {
        for (const auto& data_bits : kDataBitsCases) {
            for (const auto& parity : kParityCases) {
                for (const auto& stop_bits : kStopBitsCases) {
                    for (const auto& flow : kFlowControlCases) {
                        serial::SerialSettings settings;
                        settings.port_name = "COM1";
                        settings.baud_rate = baud;
                        settings.data_bits = data_bits.ours;
                        settings.parity = parity.ours;
                        settings.stop_bits = stop_bits.ours;
                        settings.flow_control = flow.ours;

                        QSerialPort port;
                        serial::applySerialSettings(port, settings);

                        EXPECT_EQ(port.baudRate(), baud);
                        EXPECT_EQ(port.dataBits(), data_bits.theirs);
                        EXPECT_EQ(port.parity(), parity.theirs);
                        EXPECT_EQ(port.stopBits(), stop_bits.theirs);
                        EXPECT_EQ(port.flowControl(), flow.theirs);
                        ++combinations;
                    }
                }
            }
        }
    }
    // The matrix has to stay the matrix: a combination that quietly dropped out of the
    // loops would make this test pass while covering less.
    EXPECT_EQ(combinations, 6 * 4 * 5 * 3 * 3);
}

TEST(SerialChannelTest, APortThatIsNotThereSaysSo) {
    serial::SerialSettings settings;
    settings.port_name = "openscrt-no-such-serial-port";

    serial::SerialChannel channel(settings);
    const openscrt::protocols::TransportResult result = channel.connectAsync();
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(SerialChannelTest, ASerialPortHasNoWindow) {
    serial::SerialSettings settings;
    settings.port_name = "COM1";
    serial::SerialChannel channel(settings);

    openscrt::protocols::WindowSize size{40, 120};
    EXPECT_FALSE(channel.resizeWindow(size));
}

TEST(SerialChannelTest, ABreakIsAcceptedAndAnImpossibleOneIsNot) {
    serial::SerialSettings settings;
    settings.port_name = "COM1";
    serial::SerialChannel channel(settings);

    EXPECT_TRUE(channel.sendBreak(100));
    EXPECT_TRUE(channel.sendBreak(0)) << "zero means as short as the hardware allows";
    EXPECT_FALSE(channel.sendBreak(-1));
    EXPECT_FALSE(channel.sendBreak(1001)) << "a break cannot last longer than a second";
}

TEST(SerialChannelTest, TheChannelSaysWhichPortItIs) {
    serial::SerialSettings settings;
    settings.port_name = "/dev/ttyS7";
    serial::SerialChannel channel(settings);
    EXPECT_EQ(channel.remoteAddress(), "/dev/ttyS7");
    EXPECT_EQ(channel.kind(), openscrt::protocols::TransportKind::kSerial);
}

#if defined(__linux__)

namespace {

// A virtual serial pair: a pty master and the slave device the other end sees.
// This is the same thing socat builds when it is asked for a pair of ptys, without
// needing another process and without having to parse its output.
class VirtualSerialPair {
 public:
    VirtualSerialPair() {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) {
            return;
        }
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            ::close(master_);
            master_ = -1;
            return;
        }
        const char* name = ::ptsname(master_);
        if (name == nullptr) {
            ::close(master_);
            master_ = -1;
            return;
        }
        slave_path_ = name;
    }

    ~VirtualSerialPair() {
        if (master_ >= 0) {
            ::close(master_);
        }
    }

    VirtualSerialPair(const VirtualSerialPair&) = delete;
    VirtualSerialPair& operator=(const VirtualSerialPair&) = delete;

    [[nodiscard]] bool valid() const { return master_ >= 0 && !slave_path_.empty(); }
    [[nodiscard]] const std::string& slavePath() const { return slave_path_; }
    [[nodiscard]] int master() const { return master_; }

 private:
    int master_{-1};
    std::string slave_path_;
};

}  // namespace

TEST(SerialChannelTest, BytesTravelThroughARealPortInBothDirections) {
    VirtualSerialPair pair;
    ASSERT_TRUE(pair.valid()) << "this machine has no pty pair";

    serial::SerialSettings settings;
    settings.port_name = pair.slavePath();
    settings.baud_rate = 9600;

    serial::SerialChannel channel(settings);
    const openscrt::protocols::TransportResult result = channel.connectAsync();
    ASSERT_TRUE(result.ok) << result.error;

    // From the device into the channel.
    const std::string payload = "hello serial";
    const ssize_t written =
        ::write(pair.master(), payload.data(), static_cast<size_t>(payload.size()));
    ASSERT_EQ(written, static_cast<ssize_t>(payload.size()));

    std::vector<std::uint8_t> buffer(payload.size());
    const std::size_t count =
        channel.readAvailable(buffer.data(), buffer.size(), std::chrono::seconds(10));
    ASSERT_EQ(count, payload.size());
    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), payload);

    // From the channel out to the device.
    channel.send(std::string_view("pong"));

    struct pollfd watched{};
    watched.fd = pair.master();
    watched.events = POLLIN;
    const int ready = ::poll(&watched, 1, 10000);
    ASSERT_EQ(ready, 1) << "nothing came back from the serial port";

    std::string answer(4, '\0');
    const ssize_t received = ::read(pair.master(), answer.data(), answer.size());
    ASSERT_EQ(received, static_cast<ssize_t>(answer.size()));
    EXPECT_EQ(answer, "pong");

    // A second connect is refused: the port is open, and opening it twice would leave
    // two readers on the same device.
    const openscrt::protocols::TransportResult again = channel.connectAsync();
    EXPECT_FALSE(again.ok);
}

#endif  // __linux__

int main(int argc, char** argv) {
    // QSerialPort wants an application object around it, even when it is used without
    // an event loop.
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
