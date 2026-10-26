// The Zmodem sniffer (P3-T6).
//
// When the far end runs "sz file", it does not print the file: it starts a Zmodem
// transfer, and the bytes of that transfer are binary and have no business on the
// screen. The sniffer is what stands between the two.
//
// A Zmodem frame starts with six bytes: the padding, the padding again, the escape,
// the "this is a hex frame" marker, and the type of the frame as two hex digits. The
// two that matter at the start of a transfer are
//
//   **\x18B00   the sender asks to send      (ZRQINIT)
//   **\x18B01   the receiver answers         (ZRINIT)
//
// The sniffer looks for that and says where the text ends and the transfer begins.
// Everything from there on belongs to the Zmodem engine and not to the screen.

#ifndef OPENCRT_TRANSFER_MODEM_ZMODEM_SNIFFER_H_
#define OPENCRT_TRANSFER_MODEM_ZMODEM_SNIFFER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::transfer::modem {

// The six bytes that start a hex frame.
inline constexpr std::uint8_t kZpad = '*';
inline constexpr std::uint8_t kZdle = 0x18;
inline constexpr std::uint8_t kZhex = 'B';
inline constexpr std::size_t kFrameMagicLength = 6;

// The frame types of RFC 854's Zmodem dialect, as they appear in the header.
enum class ZmodemFrame : std::uint8_t {
    kRequestReceiveInit = 0,  // ZRQINIT: the far end wants to send a file
    kReceiveInit = 1,         // ZRINIT: the answer
    kSendInit = 2,
    kAck = 3,
    kFile = 4,  // ZFILE: the name and the details of the file
    kSkip = 5,
    kNak = 6,
    kAbort = 7,
    kFinish = 8,  // ZFIN
    kResume = 9,  // ZRPOS
    kData = 10,
    kEndOfFile = 11,
    kFileError = 12,
    kCrc = 13,
    kChallenge = 14,
    kComplete = 15,
    kCancel = 16,
    kFreeCount = 17,
    kCommand = 18,
    kStandardError = 19,
};

class ZmodemSniffer {
 public:
    struct Result {
        // The bytes that belong on the screen.
        std::vector<std::uint8_t> text;
        // True when a frame was found. Everything from that point on belongs to the
        // Zmodem engine.
        bool zmodem_started{false};
        // The type of the frame that was found.
        ZmodemFrame frame{ZmodemFrame::kRequestReceiveInit};
        // How many bytes of the input were consumed. The bytes that were not are
        // still to come.
        std::size_t consumed{0};
    };

    ZmodemSniffer() = default;

    // Looks at the bytes that arrived. Before a frame is found, they are text; the
    // text stops at the frame.
    Result feed(const std::uint8_t* data, std::size_t size);

    // True once a frame was found. From then on the sniffer does nothing: the whole
    // stream belongs to the engine.
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Forgets everything, so the same sniffer can watch a later transfer.
    void reset();

    // The six bytes, as a string, for the messages the user reads.
    [[nodiscard]] static std::string frameName(ZmodemFrame frame);

 private:
    // The bytes that arrived but are not text yet: either the tail that could still
    // grow into the magic, or nothing.
    std::vector<std::uint8_t> pending_;
    bool active_{false};
};

}  // namespace openscrt::transfer::modem

#endif  // OPENCRT_TRANSFER_MODEM_ZMODEM_SNIFFER_H_
