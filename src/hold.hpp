#pragma once

// The held output of a channel whose source is gone or stalled (SPECIFICATION §5.3): which
// indices to write, where the output resumes, and when the held picture turns black.

#include <cstdint>
#include <vector>

namespace cc::hold
{

inline constexpr std::uint64_t kUndefined = UINT64_MAX;

// Held grains catch up at most this many grains; a longer gap jumps to the target (MXL marks the
// skipped grains invalid).
inline constexpr std::uint64_t kCatchUp = 8;

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
[[nodiscard]] inline Range heldRange(std::uint64_t currentIndex, std::int64_t phase, std::uint64_t lastOut)
{
    if (currentIndex == kUndefined || lastOut == kUndefined) return {};
    auto const signedTarget = static_cast<std::int64_t>(currentIndex) + phase;
    if (signedTarget <= 0) return {};
    auto const target = static_cast<std::uint64_t>(signedTarget);
    if (target <= lastOut) return {};
    return {target - lastOut > kCatchUp ? target : lastOut + 1, target};
}

// Where the output resumes once the source has a head again. MXL refuses a grain at or below the
// last one committed (PosixDiscreteFlowWriter::openGrain → MXL_ERR_INVALID_ARG), so the result is
// always above lastOut. While the output is held and the source's next grain is not past the held
// ones (it runs later than the old source did), the channel waits for it: wait = true.
struct Resume
{
    bool wait = false;
    std::uint64_t index = 0;
};
[[nodiscard]] inline Resume resumeAt(std::uint64_t head, int wholeGrainOffset, bool slice, std::uint64_t lastOut, bool holding)
{
    auto const offset = static_cast<std::uint64_t>(wholeGrainOffset > 0 ? wholeGrainOffset : 0);
    std::uint64_t start = slice || head < offset ? head : head - offset;
    if (lastOut != kUndefined && start <= lastOut)
    {
        if (holding) return {true, lastOut + 1};
        start = lastOut + 1;
    }
    return {false, start};
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
