// Serial port parameters (P3-T1).
//
// The roadmap names five parity kinds, four data-bit widths, three stop-bit widths and
// non-standard baud rates. They are modelled here as plain enumerations rather than as
// QSerialPort's, for two reasons.
//
// First, the transport layer's public headers must not drag Qt in: a consumer of
// ITransportChannel should not need Qt's include directories to use it, and the
// protocols layer is otherwise free of a display stack.
//
// Second, a session file has to be able to name these values as text. The names here
// are the ones written to the INI, so they are part of the file format and cannot
// change with a Qt release.

#ifndef OPENCRT_PROTOCOLS_SERIAL_SERIAL_SETTINGS_H_
#define OPENCRT_PROTOCOLS_SERIAL_SERIAL_SETTINGS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openscrt::protocols::serial {

// The five parity kinds. Mark and Space are the ones nobody uses and everybody needs:
// they are what a device that speaks 8 bits with a parity bit in the eighth place
// actually uses.
enum class SerialParity : std::uint8_t { kNone, kEven, kOdd, kMark, kSpace };

enum class SerialDataBits : std::uint8_t { k5, k6, k7, k8 };

enum class SerialStopBits : std::uint8_t { kOne, kOneAndHalf, kTwo };

// Only the two flow controls the roadmap names. DTR/DSR is a third mode that some
// hardware uses, and it is not offered here on purpose: it is a Windows-only concept
// in QSerialPort, and a mode that only exists on one of the three platforms is a mode
// that behaves differently on the other two.
enum class SerialFlowControl : std::uint8_t { kNone, kRtsCts, kXonXoff };

const char* toString(SerialParity parity) noexcept;
const char* toString(SerialDataBits bits) noexcept;
const char* toString(SerialStopBits bits) noexcept;
const char* toString(SerialFlowControl flow) noexcept;

// The parsers answer with nothing when the text names no known value, so a session file
// that was written by a newer build does not silently turn into a different setting.
std::optional<SerialParity> serialParityFromString(std::string_view text) noexcept;
std::optional<SerialDataBits> serialDataBitsFromString(std::string_view text) noexcept;
std::optional<SerialStopBits> serialStopBitsFromString(std::string_view text) noexcept;
std::optional<SerialFlowControl> serialFlowControlFromString(std::string_view text) noexcept;

struct SerialSettings {
    // The device: "/dev/ttyUSB0" on Linux and macOS, "COM3" on Windows.
    std::string port_name;

    // Any positive rate. The roadmap asks for non-standard baud rates, so this is an
    // int and not one of the named constants QSerialPort offers.
    int baud_rate{9600};

    SerialDataBits data_bits{SerialDataBits::k8};
    SerialParity parity{SerialParity::kNone};
    SerialStopBits stop_bits{SerialStopBits::kOne};
    SerialFlowControl flow_control{SerialFlowControl::kNone};

    // A port with no name cannot be opened, and a baud rate of zero is not a rate.
    [[nodiscard]] bool valid() const noexcept;

    // The reason the settings cannot be used, or an empty string when they can. The
    // message is written for the person looking at the dialog, not for a log.
    [[nodiscard]] std::string validationError() const;
};

}  // namespace openscrt::protocols::serial

#endif  // OPENCRT_PROTOCOLS_SERIAL_SERIAL_SETTINGS_H_
