// Frame pacing (P1-T8).
//
// The roadmap's single most repeated warning is "never call update() per packet". A comment
// cannot enforce that, so this makes it structural: the parser thread can only set an atomic
// flag, and the UI thread decides when a frame actually happens.
//
// The behaviour under test is the one that matters for the 60 FPS budget: a burst of N
// output notifications between two timer ticks must produce exactly one repaint, and a
// continuously busy session must still not exceed the configured refresh rate.

#ifndef OPENCRT_TERMINAL_RENDERER_FRAME_COALESCER_H_
#define OPENCRT_TERMINAL_RENDERER_FRAME_COALESCER_H_

#include <atomic>
#include <chrono>

namespace openscrt::terminal {

inline constexpr int kDefaultRefreshHz = 60;
inline constexpr int kHighRefreshHz = 120;

class FrameCoalescer {
 public:
    explicit FrameCoalescer(int refresh_hz = kDefaultRefreshHz) noexcept
        : interval_(std::chrono::milliseconds(refresh_hz > 0 ? 1000 / refresh_hz : 16)) {}

    // Any thread, including the parser worker. Never blocks, never touches the UI.
    void signal() noexcept {
        pending_.store(true, std::memory_order_release);
        signals_.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread. Cheap enough to poll from a debug overlay.
    [[nodiscard]] bool framePending() const noexcept {
        return pending_.load(std::memory_order_acquire);
    }

    // UI thread only, called on the timer tick. Returns true when a repaint is due: either
    // there is pending work, or the refresh interval has elapsed since the last frame.
    [[nodiscard]] bool shouldRepaint(std::chrono::steady_clock::time_point now) noexcept {
        const bool dirty = pending_.load(std::memory_order_acquire);
        const bool first_frame = last_frame_ == time_point{};

        // The very first tick always paints: a freshly shown widget has to draw itself even
        // when the session has produced no output yet, otherwise the canvas stays undefined
        // until the first byte arrives.
        if (!forced_ && !first_frame && now - last_frame_ < interval_) {
            return false;  // Too soon. pending_ stays set so the work is not lost.
        }
        if (!dirty && !first_frame) {
            return false;  // Idle and already painted: do not burn a core.
        }

        pending_.store(false, std::memory_order_release);
        forced_ = false;
        last_frame_ = now;
        frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Forces the next shouldRepaint() to repaint regardless of the interval. Used when the
    // window is exposed or resized, where waiting a frame would visibly lag the interaction.
    // UI thread only: `forced_` is deliberately not atomic because it always accompanies a
    // pending flag that is.
    void invalidate() noexcept {
        pending_.store(true, std::memory_order_release);
        forced_ = true;
    }

    [[nodiscard]] std::chrono::milliseconds interval() const noexcept { return interval_; }

    struct Stats {
        std::uint64_t signals{0};    // times output arrived
        std::uint64_t frames{0};     // times a repaint happened
        std::uint64_t coalesced{0};  // signals absorbed into an already-pending frame
    };

    [[nodiscard]] Stats stats() const noexcept {
        const std::uint64_t signals = signals_.load(std::memory_order_relaxed);
        const std::uint64_t frames = frames_.load(std::memory_order_relaxed);
        return Stats{signals, frames, signals > frames ? signals - frames : 0};
    }

 private:
    using time_point = std::chrono::steady_clock::time_point;

    const std::chrono::milliseconds interval_;
    std::atomic<bool> pending_{false};
    std::atomic<std::uint64_t> signals_{0};
    std::atomic<std::uint64_t> frames_{0};
    // UI-thread only; does not need synchronisation because only that thread reads or writes it.
    time_point last_frame_{};
    bool forced_{false};
};

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_RENDERER_FRAME_COALESCER_H_