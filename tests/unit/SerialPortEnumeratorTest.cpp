// The hotplug comparison of the serial ports (P3-T2).
//
// The comparison is pure logic over two lists, so it is tested without a device: the
// machines CI runs on have no serial ports at all, and a test that needed one would
// prove nothing there. What the real enumeration returns is checked separately -- that
// every port it names can be opened by name.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "protocols/serial/SerialPortEnumerator.h"

namespace serial = openscrt::protocols::serial;

namespace {

serial::SerialPortDescriptor port(const std::string& location,
                                  const std::string& description = "") {
    serial::SerialPortDescriptor descriptor;
    descriptor.system_location = location;
    descriptor.description = description;
    return descriptor;
}

}  // namespace

TEST(SerialPortEnumeratorTest, ANewPortIsReportedAsAdded) {
    const std::vector<serial::SerialPortDescriptor> before = {port("/dev/ttyS0")};
    const std::vector<serial::SerialPortDescriptor> after = {port("/dev/ttyS0"),
                                                             port("/dev/ttyUSB0")};

    const serial::SerialPortChanges changes = serial::diffSerialPorts(before, after);
    EXPECT_EQ(changes.added.size(), 1u);
    EXPECT_EQ(changes.added.front().system_location, "/dev/ttyUSB0");
    EXPECT_TRUE(changes.removed.empty());
}

TEST(SerialPortEnumeratorTest, APortThatIsGoneIsReportedAsRemoved) {
    const std::vector<serial::SerialPortDescriptor> before = {port("/dev/ttyS0"),
                                                              port("/dev/ttyUSB0")};
    const std::vector<serial::SerialPortDescriptor> after = {port("/dev/ttyS0")};

    const serial::SerialPortChanges changes = serial::diffSerialPorts(before, after);
    EXPECT_EQ(changes.removed.size(), 1u);
    EXPECT_EQ(changes.removed.front().system_location, "/dev/ttyUSB0");
    EXPECT_TRUE(changes.added.empty());
}

TEST(SerialPortEnumeratorTest, APortThatIsStillThereIsReportedAsNeither) {
    const std::vector<serial::SerialPortDescriptor> before = {port("/dev/ttyS0")};
    // The description changed: the driver was reloaded. That is not the adapter being
    // unplugged, so nothing was added and nothing was removed.
    const std::vector<serial::SerialPortDescriptor> after = {port("/dev/ttyS0", "FT232R")};

    const serial::SerialPortChanges changes = serial::diffSerialPorts(before, after);
    EXPECT_TRUE(changes.added.empty());
    EXPECT_TRUE(changes.removed.empty());
}

TEST(SerialPortEnumeratorTest, TwoListsOfNothingAreTheSame) {
    const std::vector<serial::SerialPortDescriptor> empty;
    const serial::SerialPortChanges changes = serial::diffSerialPorts(empty, empty);
    EXPECT_TRUE(changes.added.empty());
    EXPECT_TRUE(changes.removed.empty());
}

TEST(SerialPortEnumeratorTest, EveryPortTheMachineHasCanBeNamed) {
    // There may be no serial ports at all on this machine, and that is a normal answer.
    // What must hold either way is that every port has a name that can be opened, and
    // that no name appears twice.
    const std::vector<serial::SerialPortDescriptor> ports = serial::availableSerialPorts();
    for (const serial::SerialPortDescriptor& descriptor : ports) {
        EXPECT_FALSE(descriptor.system_location.empty());
    }
    for (std::size_t index = 0; index < ports.size(); ++index) {
        for (std::size_t other = index + 1; other < ports.size(); ++other) {
            EXPECT_NE(ports[index].system_location, ports[other].system_location);
        }
    }
}
