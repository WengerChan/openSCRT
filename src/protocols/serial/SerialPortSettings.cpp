#include "protocols/serial/SerialPortSettings.h"

namespace openscrt::protocols::serial {
namespace {

QSerialPort::Parity toQSerialPortParity(SerialParity parity) noexcept {
    switch (parity) {
        case SerialParity::kNone:
            return QSerialPort::NoParity;
        case SerialParity::kEven:
            return QSerialPort::EvenParity;
        case SerialParity::kOdd:
            return QSerialPort::OddParity;
        case SerialParity::kMark:
            return QSerialPort::MarkParity;
        case SerialParity::kSpace:
            return QSerialPort::SpaceParity;
    }
    return QSerialPort::NoParity;
}

QSerialPort::DataBits toQSerialPortDataBits(SerialDataBits bits) noexcept {
    switch (bits) {
        case SerialDataBits::k5:
            return QSerialPort::Data5Bits;
        case SerialDataBits::k6:
            return QSerialPort::Data6Bits;
        case SerialDataBits::k7:
            return QSerialPort::Data7Bits;
        case SerialDataBits::k8:
            return QSerialPort::Data8Bits;
    }
    return QSerialPort::Data8Bits;
}

QSerialPort::StopBits toQSerialPortStopBits(SerialStopBits bits) noexcept {
    switch (bits) {
        case SerialStopBits::kOne:
            return QSerialPort::OneStop;
        case SerialStopBits::kOneAndHalf:
            return QSerialPort::OneAndHalfStop;
        case SerialStopBits::kTwo:
            return QSerialPort::TwoStop;
    }
    return QSerialPort::OneStop;
}

QSerialPort::FlowControl toQSerialPortFlowControl(SerialFlowControl flow) noexcept {
    switch (flow) {
        case SerialFlowControl::kNone:
            return QSerialPort::NoFlowControl;
        case SerialFlowControl::kRtsCts:
            return QSerialPort::HardwareFlowControl;
        case SerialFlowControl::kXonXoff:
            return QSerialPort::SoftwareFlowControl;
    }
    return QSerialPort::NoFlowControl;
}

void appendRefusal(std::string& refusals, const char* name) {
    if (!refusals.empty()) {
        refusals += ", ";
    }
    refusals += name;
}

}  // namespace

std::string applySerialSettings(QSerialPort& port, const SerialSettings& settings) {
    std::string refusals;

    // The order is the one QSerialPort documents: the data bits and the parity have to
    // be in place before the baud rate, because some drivers refuse a baud rate that
    // does not fit the current word length.
    if (!port.setDataBits(toQSerialPortDataBits(settings.data_bits))) {
        appendRefusal(refusals, "the data bits");
    }
    if (!port.setParity(toQSerialPortParity(settings.parity))) {
        appendRefusal(refusals, "the parity");
    }
    if (!port.setStopBits(toQSerialPortStopBits(settings.stop_bits))) {
        appendRefusal(refusals, "the stop bits");
    }
    if (!port.setFlowControl(toQSerialPortFlowControl(settings.flow_control))) {
        appendRefusal(refusals, "the flow control");
    }
    // Any positive rate, including the non-standard ones the roadmap asks for.
    if (!port.setBaudRate(settings.baud_rate)) {
        appendRefusal(refusals, "the baud rate");
    }

    return refusals;
}

}  // namespace openscrt::protocols::serial
