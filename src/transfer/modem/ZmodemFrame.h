// The frames of the Zmodem protocol (P3-T7).
//
// A frame is a header and, sometimes, data. The header is either a hex frame -- the
// type and four bytes as text, with a checksum -- or a binary frame -- the type and
// four bytes as they are, with a checksum. The handshake uses the hex form because a
// line that garbles a bit has to stay readable; the file itself uses the binary form
// because speed matters.
//
// The checksums are the two of the Zmodem protocol: a 16-bit one for the headers and
// for the data frames, and a 32-bit one for the frames that carry a file.

#ifndef OPENCRT_TRANSFER_MODEM_ZMODEM_FRAME_H_
#define OPENCRT_TRANSFER_MODEM_ZMODEM_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transfer/modem/ZmodemSniffer.h"

namespace openscrt::transfer::modem {

// How the data frame ends. The letter says what comes next: the frame ends here and
// the next one starts, the frame continues, or the other end has to answer.
enum class FrameEnd : std::uint8_t {
    kEnd = 'h',       // ZCRCE: the frame is over.
    kContinue = 'i',  // ZCRCG: more follows.
    kQuery = 'j',     // ZCRCQ: more follows, and the other end has to answer.
    kWait = 'k',      // ZCRCW: the frame is over and the other end has to answer.
};

struct ZmodemHeader {
    ZmodemFrame frame{ZmodemFrame::kRequestReceiveInit};
    // The four bytes that go with the type. Their meaning depends on the type: for
    // ZRPOS and ZDATA they are the position in the file, for ZRINIT they are the
    // flags of the receiver.
    std::uint8_t bytes[4]{0, 0, 0, 0};

    // The position the header carries, little endian as the protocol has it.
    [[nodiscard]] std::uint32_t position() const noexcept {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    void setPosition(std::uint32_t position) noexcept {
        bytes[0] = static_cast<std::uint8_t>(position & 0xFF);
        bytes[1] = static_cast<std::uint8_t>((position >> 8) & 0xFF);
        bytes[2] = static_cast<std::uint8_t>((position >> 16) & 0xFF);
        bytes[3] = static_cast<std::uint8_t>((position >> 24) & 0xFF);
    }
};

// The checksum of the headers and of the data frames: the 16-bit one of the Zmodem
// protocol, which is the CRC-16 of CCITT with nothing added to it.
[[nodiscard]] std::uint16_t crc16(std::uint16_t crc, std::uint8_t byte);

// The 32-bit one, for the frames that carry a file.
[[nodiscard]] std::uint32_t crc32(std::uint32_t crc, std::uint8_t byte);

// The checksum of a header: the type, the four bytes, and two zero bytes at the end.
[[nodiscard]] std::uint16_t headerChecksum(const ZmodemHeader& header);

// The checksum of a data frame: the data, the way it ends, and two zero bytes.
[[nodiscard]] std::uint16_t dataChecksum(const std::uint8_t* data, std::size_t size,
                                         FrameEnd frame_end);

// A hex header: the type and the four bytes as text, with the checksum and the two
// bytes that end it.
[[nodiscard]] std::vector<std::uint8_t> encodeHexHeader(const ZmodemHeader& header);

// A binary header. With `crc32_frame` the checksum is the 32-bit one and the frame
// is marked as such, which is how the other end knows.
[[nodiscard]] std::vector<std::uint8_t> encodeBinaryHeader(const ZmodemHeader& header,
                                                           bool crc32_frame);

// A data frame: the bytes with the ones that have to be escaped escaped, and the way
// it ends with its checksum.
[[nodiscard]] std::vector<std::uint8_t> encodeDataFrame(const std::uint8_t* data, std::size_t size,
                                                        FrameEnd frame_end);

// Puts the bytes into the output with the ones that have to be escaped escaped.
//
// The bytes that are escaped are the control characters: 0x00 to 0x1f and 0x80 to
// 0x9f. The escape is the ZDLE of the protocol followed by the byte with its top two
// bits flipped, so 0x00 travels as 0x18 0x40.
void appendEscaped(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size);

[[nodiscard]] inline bool needsEscaping(std::uint8_t byte) noexcept {
    return (byte & 0x60) == 0;
}

}  // namespace openscrt::transfer::modem

#endif  // OPENCRT_TRANSFER_MODEM_ZMODEM_FRAME_H_
