// Binds the escape-sequence parser to the screen grid (completes P1-T4, adds P1-T5).
//
// Until this existed, ControlSequenceParser and ScreenGrid were two separately tested
// pieces with nothing between them. This is that piece: it consumes decoded events, applies
// SGR to a CellStyle, and drives the grid's cursor / erase / scroll / mode operations.
//
// Scope is deliberately the VT100/VT220/xterm subset the roadmap names for Phase 1. Modes
// that Phase 1 does not need (insert mode, origin mode's full semantics, charset
// designations) are recognised and ignored rather than silently mishandled.

#ifndef OPENCRT_TERMINAL_EMULATOR_TERMINAL_EMULATOR_H_
#define OPENCRT_TERMINAL_EMULATOR_TERMINAL_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "terminal/buffer/RingScrollbackBuffer.h"
#include "terminal/buffer/ScreenGrid.h"
#include "terminal/emulator/ControlSequenceParser.h"

namespace openscrt::terminal {

class TerminalEmulator : public IControlSequenceSink {
 public:
    // The grid's lifetime must exceed the emulator's; scrollback may be null.
    explicit TerminalEmulator(ScreenGrid& grid, RingScrollbackBuffer* scrollback = nullptr);

    TerminalEmulator(const TerminalEmulator&) = delete;
    TerminalEmulator& operator=(const TerminalEmulator&) = delete;

    // Primary entry point: feed raw transport bytes.
    void write(const std::uint8_t* data, std::size_t size);
    void write(std::string_view text);

    // RIS. Also clears the pending reply buffer and any half-decoded sequence.
    void reset();

    [[nodiscard]] ScreenGrid& grid() noexcept { return *grid_; }
    [[nodiscard]] const CellStyle& style() const noexcept { return style_; }

    // Bytes the host expects back on the wire (DA/DSR replies). Taken by value so a caller
    // can flush them from the I/O thread and the next reply starts clean.
    [[nodiscard]] std::string takeDeviceResponse();

    // IControlSequenceSink
    void print(std::uint32_t codepoint) override;
    void execute(std::uint8_t control) override;
    void escapeDispatch(std::span<const std::uint8_t> intermediates, char final) override;
    void csiDispatch(const CsiSequence& sequence) override;
    void oscDispatch(const OscSequence& sequence) override;
    void deviceResponse(std::string_view response) override;

 private:
    void applyGraphicRendition(const CsiSequence& sequence);
    void applyModeChange(const CsiSequence& sequence, bool enable);
    void applyCursorMovement(const CsiSequence& sequence, char final);
    void applyErase(const CsiSequence& sequence, char final);
    void applyEraseDisplay(const CsiSequence& sequence);
    void applyEraseLine(const CsiSequence& sequence);
    void applyScroll(const CsiSequence& sequence, char final);

    // Parameter access with the VT default applied. A bare CSI carries a single default
    // parameter, which the parser records as kDefaultParam rather than 0.
    [[nodiscard]] int paramOr(const CsiSequence& sequence, std::size_t index, int fallback) const;
    [[nodiscard]] bool isSubParam(const CsiSequence& sequence, std::size_t index) const;

    void scrollOffLine(std::string_view line);
    void onScrolledOff(std::string_view line);

    ScreenGrid* grid_;
    RingScrollbackBuffer* scrollback_;
    ControlSequenceParser parser_{*this};

    CellStyle style_;
    // DECSC / DECRC.
    int saved_row_ = 0;
    int saved_column_ = 0;
    bool origin_mode_ = false;  // DECOM: CUP becomes row-relative to the scroll region.
    bool auto_wrap_ = true;     // DECAWM
    bool insert_mode_ = false;  // IRM
    bool cursor_visible_ = true;

    std::string device_response_;
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_EMULATOR_TERMINAL_EMULATOR_H_