// An independent VT implementation to compare ours against.
//
// P1-A2 asks for a per-case baseline comparison. A baseline produced by our own emulator
// would only prove that we did not change -- it would happily lock in a bug. A baseline
// produced by another implementation is evidence about the escape sequences themselves.
//
// tmux is the reference here: it is a widely deployed C implementation of the same
// sequences, and it can be driven headlessly, which is what makes it usable in CI.
//
// The comparison only works if both sides are asked the same question, so the reference
// side rebuilds a ScreenGrid out of tmux's captured pane and then goes through the very
// same snapshotScreen() the emulator's side uses. Any disagreement is then a real
// disagreement about the screen, not about the format.

#ifndef OPENCRT_TESTS_SUPPORT_VT_REFERENCE_H_
#define OPENCRT_TESTS_SUPPORT_VT_REFERENCE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::test {

// Renders the byte stream through tmux and returns the canonical snapshot of the result.
//
// Returns an empty string when tmux is not installed or the session could not be driven.
// An empty string is not an error the caller should treat as a mismatch: it means "no
// reference available on this machine".
[[nodiscard]] std::string renderWithTmux(const std::vector<std::uint8_t>& bytes, int rows,
                                         int cols);

// True when a usable tmux is on the PATH.
[[nodiscard]] bool tmuxAvailable();

}  // namespace openscrt::test

#endif  // OPENCRT_TESTS_SUPPORT_VT_REFERENCE_H_
