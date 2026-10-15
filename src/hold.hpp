#pragma once

// The held output of a channel whose source is gone or stalled (SPECIFICATION §5.3): which
// indices to write, where the output resumes, and when the held picture turns black.

#include <cmath>
#include <cstdint>
#include <vector>

namespace cc::hold
{

inline constexpr std::uint64_t kUndefined = UINT64_MAX;

// Margin of the catch-up below, in grains.
inline constexpr std::uint64_t kCatchUpMargin = 8;

// How many grains the held output fills in when it starts (the time from the last output grain to
// the hold: up to the stall threshold), plus a margin. A longer gap jumps to the target, and MXL
// marks the skipped grains invalid, which a reader takes as no signal. The same bound is the
// longest the channel waits for a source behind the held grains.
[[nodiscard]] inline std::uint64_t catchUpGrains(double stallSeconds, int rateNum, int rateDen)
{
    double const rate = rateDen > 0 && rateNum > 0 ? static_cast<double>(rateNum) / rateDen : 50.0;
    return static_cast<std::uint64_t>(std::ceil(stallSeconds * rate)) + kCatchUpMargin;
}

// The output's position against the grain clock (mxlGetCurrentIndex), measured at its last
// committed grain: index − current index. Negative for a source behind TAI (a gateway indexing by
// capture time), positive for one ahead. The held output keeps it, so a source that comes back
// with the same timing continues right after the held grains.
[[nodiscard]] inline std::int64_t phaseOf(std::uint64_t committedIndex, std::uint64_t currentIndex)
{
    return static_cast<std::int64_t>(committedIndex) - static_cast<std::int64_t>(currentIndex);
}

// The held grains to write now: [first, last], empty when first > last.
struct Range
{
    std::uint64_t first = 1;
    std::uint64_t last = 0;
};
[[nodiscard]] inline Range heldRange(std::uint64_t currentIndex, std::int64_t phase, std::uint64_t lastOut, std::uint64_t catchUp)
{
    if (currentIndex == kUndefined || lastOut == kUndefined) return {};
    auto const signedTarget = static_cast<std::int64_t>(currentIndex) + phase;
    if (signedTarget <= 0) return {};
    auto const target = static_cast<std::uint64_t>(signedTarget);
    if (target <= lastOut) return {};
    return {target - lastOut > catchUp ? target : lastOut + 1, target};
}

// The grains to fill before writing output grain `next`, so the output has no hole: MXL marks
// every grain a writer skips invalid, which a reader takes as no signal. Only the last `ring` grains
// can still be read, so at most that many are filled; the rest of the ring is overwritten by them.
[[nodiscard]] inline Range fillRange(std::uint64_t lastOut, std::uint64_t next, std::uint64_t ring)
{
    if (lastOut == kUndefined || next == kUndefined || next <= lastOut + 1) return {};
    auto const first = next - lastOut - 1 > ring ? next - ring : lastOut + 1;
    return {first, next - 1};
}

// Where the output resumes once the source has a head again. MXL refuses a grain at or below the
// last one committed (PosixDiscreteFlowWriter::openGrain → MXL_ERR_INVALID_ARG), so a read is
// always above lastOut.
// - Read: from `index`.
// - Wait: the source's next grain is not past the held ones, but within `catchUp` grains (it runs a
//   little later than the old source did); it catches up within that many grains.
// - Rephase: the source runs more than `catchUp` grains behind the output (a writer indexing by
//   UTC instead of TAI is 37 s behind). It would never catch up while the output keeps its timing,
//   and the output cannot go back, so the output flow is made again in the source's timing.
//   `lag` is the distance in grains.
enum class Action
{
    Read,
    Wait,
    Rephase,
};
struct Resume
{
    Action action = Action::Read;
    std::uint64_t index = 0;
    std::uint64_t lag = 0;
    [[nodiscard]] bool wait() const { return action == Action::Wait; }
};
[[nodiscard]] inline Resume resumeAt(std::uint64_t head, int wholeGrainOffset, bool slice, std::uint64_t lastOut, std::uint64_t catchUp)
{
    auto const offset = static_cast<std::uint64_t>(wholeGrainOffset > 0 ? wholeGrainOffset : 0);
    std::uint64_t const start = slice || head < offset ? head : head - offset;
    if (lastOut == kUndefined || start > lastOut) return {Action::Read, start, 0};
    auto const lag = lastOut + 1 - start;
    if (lag > catchUp) return {Action::Rephase, start, lag};
    return {Action::Wait, lastOut + 1, lag};
}

// Whether a hold that started at `since` has run past CC_HOLD_MAX_S (0 = never).
[[nodiscard]] inline bool expired(double now, double since, int maxSeconds)
{
    return maxSeconds > 0 && now - since >= static_cast<double>(maxSeconds);
}

// A v210 picture in BT.709 narrow-range black (Y 64, Cb and Cr 512), `size` bytes (a grain may be
// larger than stride × height; the rest stays zero).
[[nodiscard]] std::vector<std::uint8_t> blackV210(int width, int height, std::size_t size);

} // namespace cc::hold
