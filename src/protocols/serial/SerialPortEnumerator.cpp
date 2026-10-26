#include "protocols/serial/SerialPortEnumerator.h"

#include <QSerialPortInfo>
#include <QString>
#include <algorithm>
#include <utility>

namespace openscrt::protocols::serial {
namespace {

bool sameLocation(const SerialPortDescriptor& a, const SerialPortDescriptor& b) {
    return a.system_location == b.system_location;
}

bool contains(const std::vector<SerialPortDescriptor>& ports, const SerialPortDescriptor& port) {
    return std::any_of(ports.begin(), ports.end(), [&port](const SerialPortDescriptor& seen) {
        return sameLocation(seen, port);
    });
}

}  // namespace

std::vector<SerialPortDescriptor> availableSerialPorts() {
    std::vector<SerialPortDescriptor> ports;
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    ports.reserve(static_cast<std::size_t>(infos.size()));
    for (const QSerialPortInfo& info : infos) {
        SerialPortDescriptor descriptor;
        descriptor.system_location = info.systemLocation().toStdString();
        descriptor.description = info.description().toStdString();
        descriptor.manufacturer = info.manufacturer().toStdString();
        descriptor.serial_number = info.serialNumber().toStdString();
        // The identifiers are numbers, not text: they are the USB vendor and product
        // ids, and Qt hands them over as numbers.
        descriptor.vendor_id =
            QString::number(static_cast<uint>(info.vendorIdentifier()), 16).toStdString();
        descriptor.product_id =
            QString::number(static_cast<uint>(info.productIdentifier()), 16).toStdString();
        ports.push_back(std::move(descriptor));
    }
    return ports;
}

SerialPortChanges diffSerialPorts(const std::vector<SerialPortDescriptor>& before,
                                  const std::vector<SerialPortDescriptor>& after) {
    SerialPortChanges changes;
    for (const SerialPortDescriptor& port : after) {
        if (!contains(before, port)) {
            changes.added.push_back(port);
        }
    }
    for (const SerialPortDescriptor& port : before) {
        if (!contains(after, port)) {
            changes.removed.push_back(port);
        }
    }
    return changes;
}

}  // namespace openscrt::protocols::serial
