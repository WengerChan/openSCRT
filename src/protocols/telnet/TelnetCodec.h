// The Telnet protocol, as a state machine over bytes (P3-T3).
//
// RFC 854 is a byte protocol with an escape: the byte 255 (IAC) is not data, it
// starts a command. Everything a Telnet session does -- negotiating the options,
// telling the peer how big the window is, sending a keep-alive -- is that one
// escape used in different ways.
//
// The state machine is pure logic over bytes, with no socket in it. That is what
// makes it testable: a negotiation can be written down as a list of bytes and
// checked, without a server anywhere.
//
// What comes out of it are two streams: the text that belongs on the screen, and the
// bytes that have to go back to the peer.

#ifndef OPENCRT_PROTOCOLS_TELNET_TELNET_CODEC_H_
#define OPENCRT_PROTOCOLS_TELNET_TELNET_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocols/ITransportChannel.h"

namespace openscrt::protocols::telnet {

// The commands of RFC 854. IAC is also the escape byte, which is why the cancel
// character of a terminal (0x18) is the same byte: CAN and IAC are the same value.
enum class TelnetCommand : std::uint8_t {
    kSe = 240,
    kNop = 241,
    kDataMark = 242,
    kBreak = 243,
    kInterrupt = 244,
    kAbort = 245,
    kEof = 246,
    kSuspend = 247,
    kEor = 248,
    kGoAhead = 249,
    kSb = 250,
    kWill = 251,
    kWont = 252,
    kDo = 253,
    kDont = 254,
    kIac = 255,
};

// The options of RFC 854 and its companions. Only the ones this implementation
// understands are named; anything else is answered with a refusal.
enum class TelnetOption : std::uint8_t {
    kEcho = 1,
    kSuppressGoAhead = 3,
    kStatus = 5,
    kTimingMark = 6,
    kTermType = 24,
    kWindowSize = 31,
    kTermSpeed = 32,
    kLinemode = 34,
    kNewEnvironment = 39,
};

class TelnetCodec {
 public:
    TelnetCodec() = default;

    struct Result {
        // What belongs on the screen.
        std::vector<std::uint8_t> text;
        // What has to go back to the peer.
        std::vector<std::uint8_t> replies;
        // True when the peer told us how big its window is.
        bool window_size_changed{false};
        WindowSize window_size{};
    };

    // Feeds the bytes that arrived and says what to do with them.
    Result feed(const std::uint8_t* data, std::size_t size);

    // How big our window is. It is sent to the peer when the peer asks for it and
    // whenever it changes.
    void setWindowSize(const WindowSize& size);
    [[nodiscard]] WindowSize windowSize() const noexcept { return window_size_; }

    // The name we answer with when the peer asks for the terminal type.
    void setTerminalType(std::string terminal_type);

    // The bytes of a keep-alive: IAC NOP. The peer answers with something, and a
    // connection that has gone quiet without a word is a connection that died.
    [[nodiscard]] static std::vector<std::uint8_t> keepAlive();

    // The bytes that tell the peer how big our window is.
    [[nodiscard]] std::vector<std::uint8_t> windowSizeReport() const;

    // How many bytes are waiting inside a half-finished command. A peer that stops
    // in the middle of a negotiation has to be given a chance to finish.
    [[nodiscard]] bool midCommand() const noexcept { return state_ != State::kData; }

 private:
    enum class State {
        kData,
        kIac,        // saw IAC, waiting for the command
        kOption,     // saw WILL/WONT/DO/DONT, waiting for the option
        kSubOption,  // inside a subnegotiation, waiting for the option
        kSubData,    // inside a subnegotiation, waiting for the payload
        kSubIac,     // inside a subnegotiation, saw IAC: SE ends it
    };

    void handleCommand(TelnetCommand command, std::uint8_t option, Result& result);
    void handleSubnegotiation(const std::vector<std::uint8_t>& payload, Result& result);
    void answer(TelnetCommand command, TelnetOption option, bool accept, Result& result);

    State state_{State::kData};
    std::vector<std::uint8_t> subnegotiation_;
    WindowSize window_size_{24, 80};
    std::string terminal_type_{"xterm-256color"};
    // The options we have agreed to, so a repeated request is answered the same way
    // and the negotiation does not go round in circles.
    bool echo_enabled_{false};
    bool window_size_wanted_{false};
    bool terminal_type_wanted_{false};
};

}  // namespace openscrt::protocols::telnet

#endif  // OPENCRT_PROTOCOLS_TELNET_TELNET_CODEC_H_
