// The serial ports this machine has, and what changed since the last look (P3-T2).
//
// QSerialPortInfo can list the ports, but it cannot say when one appears: there is no
// hotplug signal in Qt. So the comparison lives here -- the caller takes a list now and
// a list later, and this says what came and what went in between.
//
// The comparison is pure logic over two lists, which means it can be tested without a
// device. That matters: the machines CI runs on have no serial ports at all, so a test
// that needed one would prove nothing.

#ifndef OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_ENUMERATOR_H_
#define OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_ENUMERATOR_H_

#include <cstddef>
#include <string>
#include <vector>

namespace openscrt::protocols::serial {

struct SerialPortDescriptor {
    // The device to open: "/dev/ttyUSB0" on Linux and macOS, "COM3" on Windows.
    std::string system_location;
    // What the device calls itself: "USB Serial", "FT232R USB UART", and so on.
    std::string description;
    std::string manufacturer;
    std::string serial_number;
    std::string vendor_id;
    std::string product_id;
};

// Every port the system knows about right now. The list is empty on a machine with no
// serial ports, which is a normal answer and not an error.
std::vector<SerialPortDescriptor> availableSerialPorts();

// What changed between two lists.
//
// A port is the same port when its system location is the same: the description of a
// device can change when a driver is reloaded, and that is not the adapter being
// unplugged.
struct SerialPortChanges {
    std::vector<SerialPortDescriptor> added;
    std::vector<SerialPortDescriptor> removed;
};

SerialPortChanges diffSerialPorts(const std::vector<SerialPortDescriptor>& before,
                                  const std::vector<SerialPortDescriptor>& after);

}  // namespace openscrt::protocols::serial

#endif  // OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_ENUMERATOR_H_
