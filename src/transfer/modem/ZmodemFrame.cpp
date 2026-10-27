#include "transfer/modem/ZmodemFrame.h"

namespace openscrt::transfer::modem {
namespace {

constexpr std::uint8_t kZbin = 'A';
constexpr std::uint8_t kZbin32 = 'C';

void appendHexDigit(std::vector<std::uint8_t>& out, std::uint8_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out.push_back(static_cast<std::uint8_t>(kDigits[(value >> 4) & 0x0F]));
    out.push_back(static_cast<std::uint8_t>(kDigits[value & 0x0F]));
}

int hexValue(std::uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::uint16_t crc16(std::uint16_t crc, std::uint8_t byte) {
    crc ^= static_cast<std::uint16_t>(byte) << 8;
    for (int bit = 0; bit < 8; ++bit) {
        if ((crc & 0x8000) != 0) {
            crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
        } else {
            crc = static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::uint32_t crc32(std::uint32_t crc, std::uint8_t byte) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
        if ((crc & 1) != 0) {
            crc = (crc >> 1) ^ 0xEDB88320U;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

std::uint16_t headerChecksum(const ZmodemHeader& header) {
    std::uint16_t crc = crc16(0, static_cast<std::uint8_t>(header.frame));
    for (const std::uint8_t byte : header.bytes) {
        crc = crc16(crc, byte);
    }
    // Two zero bytes at the end, the way the protocol counts them.
    crc = crc16(crc16(crc, 0), 0);
    return crc;
}

std::uint16_t dataChecksum(const std::uint8_t* data, std::size_t size, FrameEnd frame_end) {
    std::uint16_t crc = 0;
    for (std::size_t index = 0; index < size; ++index) {
        crc = crc16(crc, data[index]);
    }
    crc = crc16(crc, static_cast<std::uint8_t>(frame_end));
    return crc16(crc16(crc, 0), 0);
}

std::vector<std::uint8_t> encodeHexHeader(const ZmodemHeader& header) {
    std::vector<std::uint8_t> out;
    out.push_back(kZpad);
    out.push_back(kZpad);
    out.push_back(kZdle);
    out.push_back('B');
    appendHexDigit(out, static_cast<std::uint8_t>(header.frame));
    for (const std::uint8_t byte : header.bytes) {
        appendHexDigit(out, byte);
    }
    const std::uint16_t crc = headerChecksum(header);
    appendHexDigit(out, static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    appendHexDigit(out, static_cast<std::uint8_t>(crc & 0xFF));
    // The two bytes that end a hex frame. The second one has its top bit set, which
    // is how a line that eats the line feed still sees the end of the frame.
    out.push_back('\r');
    out.push_back(static_cast<std::uint8_t>('\n' | 0x80));
    return out;
}

std::vector<std::uint8_t> encodeBinaryHeader(const ZmodemHeader& header, bool crc32_frame) {
    std::vector<std::uint8_t> out;
    out.push_back(kZpad);
    out.push_back(kZdle);
    out.push_back(crc32_frame ? kZbin32 : kZbin);
    out.push_back(static_cast<std::uint8_t>(header.frame));
    for (const std::uint8_t byte : header.bytes) {
        out.push_back(byte);
    }
    if (crc32_frame) {
        std::uint32_t crc = 0xFFFFFFFFU;
        crc = crc32(crc, static_cast<std::uint8_t>(header.frame));
        for (const std::uint8_t byte : header.bytes) {
            crc = crc32(crc, byte);
        }
        crc = ~crc;
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFF));
        }
        return out;
    }
    const std::uint16_t crc = headerChecksum(header);
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    return out;
}

void appendEscaped(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t byte = data[index];
        if (needsEscaping(byte)) {
            out.push_back(kZdle);
            out.push_back(static_cast<std::uint8_t>(byte ^ 0x40));
        } else {
            out.push_back(byte);
        }
    }
}

std::vector<std::uint8_t> encodeDataFrame(const std::uint8_t* data, std::size_t size,
                                          FrameEnd frame_end) {
    std::vector<std::uint8_t> out;
    appendEscaped(out, data, size);
    out.push_back(kZdle);
    out.push_back(static_cast<std::uint8_t>(frame_end));
    const std::uint16_t crc = dataChecksum(data, size, frame_end);
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    return out;
}

bool decodeHeader(const std::uint8_t* data, std::size_t size, ZmodemHeader& header,
                  bool& crc32_frame, std::size_t& used) {
    // A hex header: the padding, the padding again, the escape, the marker, the type as
    // two digits, the four bytes as eight digits, the checksum as four digits, and the
    // two bytes that end it.
    constexpr std::size_t kHexHeaderLength = 4 + 14 + 2;
    // A binary header: the padding, the escape, the marker, the type, the four bytes
    // and the checksum -- which is two bytes, or four when it is the 32-bit one.
    constexpr std::size_t kBinaryHeaderLength = 3 + 1 + 4 + 2;
    constexpr std::size_t kBinary32HeaderLength = 3 + 1 + 4 + 4;

    for (std::size_t at = 0; at < size; ++at) {
        if (data[at] != kZpad) {
            continue;
        }
        if (at + 1 >= size) {
            break;
        }
        if (data[at + 1] == kZpad) {
            // The hex form.
            if (at + 4 > size) {
                break;
            }
            if (data[at + 2] != kZdle || data[at + 3] != 'B') {
                continue;
            }
            if (at + kHexHeaderLength > size) {
                break;
            }
            // Fourteen hex digits are seven bytes: the type, the four bytes and the
            // checksum. Each byte is two digits, so seven pairs are read.
            int digits[7];
            bool ok = true;
            for (int index = 0; index < 7 && ok; ++index) {
                const int high = hexValue(data[at + 4 + static_cast<std::size_t>(index) * 2]);
                const int low = hexValue(data[at + 5 + static_cast<std::size_t>(index) * 2]);
                if (high < 0 || low < 0) {
                    ok = false;
                    break;
                }
                digits[index] = (high << 4) | low;
            }
            if (!ok) {
                continue;
            }
            header.frame = static_cast<ZmodemFrame>(digits[0]);
            for (int index = 0; index < 4; ++index) {
                header.bytes[index] = static_cast<std::uint8_t>(digits[1 + index]);
            }
            const auto sent = static_cast<std::uint16_t>((digits[5] << 8) | digits[6]);
            if (headerChecksum(header) != sent) {
                continue;
            }
            crc32_frame = false;
            used = at + kHexHeaderLength;
            return true;
        }
        // The binary form.
        if (at + 3 > size) {
            break;
        }
        if (data[at + 1] != kZdle) {
            continue;
        }
        const bool is_32 = data[at + 2] == 'C';
        if (data[at + 2] != 'A' && !is_32) {
            continue;
        }
        const std::size_t length = is_32 ? kBinary32HeaderLength : kBinaryHeaderLength;
        if (at + length > size) {
            break;
        }
        header.frame = static_cast<ZmodemFrame>(data[at + 3]);
        for (std::size_t index = 0; index < 4; ++index) {
            header.bytes[index] = data[at + 4 + index];
        }
        crc32_frame = is_32;
        if (!is_32) {
            const auto sent = static_cast<std::uint16_t>((data[at + 8] << 8) | data[at + 9]);
            if (headerChecksum(header) != sent) {
                continue;
            }
        }
        used = at + length;
        return true;
    }
    return false;
}

}  // namespace openscrt::transfer::modem
