// The mapping from the protocol layer's serial settings onto QSerialPort (P3-T1).
//
// This is the one header in the protocols layer that includes Qt, and it is a separate
// header on purpose: the channel itself keeps its public surface free of Qt (see
// SerialSettings.h for why), while this one is the seam where the two meet.
//
// It is a separate header and a separate function so the mapping can be checked against
// a QSerialPort that was never opened. QSerialPort keeps the properties it was given,
// so the whole parameter matrix -- every baud rate, data width, parity, stop width and
// flow control the roadmap names -- can be verified without a device attached.

#ifndef OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_SETTINGS_H_
#define OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_SETTINGS_H_

#include <QSerialPort>
#include <string>

#include "protocols/serial/SerialSettings.h"

namespace openscrt::protocols::serial {

// Applies the settings to the port and returns the names of the ones the port refused,
// joined by commas. An empty string means it took all of them.
//
// The refusal list is only meaningful for an open port: on a port that was never
// opened the setters merely record the values, and whether they report success is
// Qt's business rather than the device's.
std::string applySerialSettings(QSerialPort& port, const SerialSettings& settings);

}  // namespace openscrt::protocols::serial

#endif  // OPENCRT_PROTOCOLS_SERIAL_SERIAL_PORT_SETTINGS_H_
