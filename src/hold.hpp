#pragma once

// The held output of a channel whose source is gone or stalled (SPECIFICATION §5.3): which
// indices to write, where the output resumes, and when the held picture turns black.

#include <algorithm>
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

// How many grains one fill may write (a gap before the next output grain, or the start of a hold):
// about the catch-up (twice it, 42 grains at 50p) and at most kFillBudgetBytes, so a fill never
// blocks the channel long (2160p v210 is about 22 MB a grain). The older grains of a longer gap
// stay unwritten and MXL marks them invalid; readers that follow the head read only the newest.
inline constexpr std::uint64_t kFillBudgetBytes = 256ull << 20;
[[nodiscard]] inline std::uint64_t fillCap(std::uint64_t catchUp, std::size_t grainBytes)
{
    std::uint64_t const byGrains = 2 * std::max<std::uint64_t>(catchUp, 1);
    std::uint64_t const byBytes = grainBytes > 0 ? kFillBudgetBytes / grainBytes : byGrains;
    return std::max<std::uint64_t>(1, std::min(byGrains, byBytes));
}

// The held grains to write now: [first, last], empty when first > last.
struct Range
{
    std::uint64_t first = 1;
    std::uint64_t last = 0;
};
// At most `catchUp` grains, the newest.
[[nodiscard]] inline Range heldRange(std::uint64_t currentIndex, std::int64_t phase, std::uint64_t lastOut, std::uint64_t catchUp)
{
    if (currentIndex == kUndefined || lastOut == kUndefined) return {};
    auto const signedTarget = static_cast<std::int64_t>(currentIndex) + phase;
    if (signedTarget <= 0) return {};
    auto const target = static_cast<std::uint64_t>(signedTarget);
    if (target <= lastOut) return {};
    return {target - lastOut > catchUp ? target - catchUp + 1 : lastOut + 1, target};
}

// The grains to fill before writing output grain `next`, so the output has no hole: MXL marks
// every grain a writer skips invalid, which a reader takes as no signal. At most `ring` grains,
// the newest (see fillCap).
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

// The held output picture, repeated by the hold and the gap fill. For progressive video it is
// the last output grain. An interlaced flow's grains are fields (even indices the first field), so
// it keeps the last grain of each parity and writes each index with the field of its own parity:
// a held frame keeps both fields instead of repeating one into both (1.2.3).
class HeldPicture
{
public:
    struct Grain
    {
        std::vector<std::uint8_t> data;
        std::uint32_t flags = 0;
    };

    void clear()
    {
        slots_[0] = {};
        slots_[1] = {};
    }
    [[nodiscard]] bool empty() const { return slots_[0].data.empty() && slots_[1].data.empty(); }
    // Keeps output grain `index` (in the slot of its parity when `fields`).
    void set(std::uint64_t index, std::uint8_t const* data, std::size_t size, std::uint32_t flags, bool fields)
    {
        fields_ = fields;
        auto& slot = slots_[fields ? index % 2 : 0];
        slot.data.assign(data, data + size);
        slot.flags = flags;
    }
    // The grain to write at output index `index`: the field of its parity, or the other one when
    // only one was kept. Empty when nothing is held.
    [[nodiscard]] Grain const& at(std::uint64_t index) const
    {
        auto const& own = slots_[fields_ ? index % 2 : 0];
        return own.data.empty() ? slots_[fields_ ? 1 - index % 2 : 0] : own;
    }
    [[nodiscard]] std::size_t grainBytes() const { return std::max(slots_[0].data.size(), slots_[1].data.size()); }
    // Past CC_HOLD_MAX_S: every kept grain turns black (`lines` lines of `width`), flagged `flags`.
    void black(int width, int lines, std::uint32_t flags);

private:
    Grain slots_[2];
    bool fields_ = false;
};

// Whether the grain read back at `asked` may be kept as the held picture's other field. MXL's
// non-blocking read returns whatever its ring slot holds, without checking the slot's own index,
// and returns grains flagged invalid as OK. So after a route change that re-opens the same output
// flow, the grain before the last can be the previous route's field, or invalid (1.2.3 review).
[[nodiscard]] inline bool keepOtherField(std::uint64_t asked, std::uint64_t grainIndex, bool invalid)
{
    return grainIndex == asked && !invalid;
}

// A v210 picture in BT.709 narrow-range black (Y 64, Cb and Cr 512), `size` bytes (a grain may be
// larger than stride × height; the rest stays zero).
[[nodiscard]] std::vector<std::uint8_t> blackV210(int width, int height, std::size_t size);

} // namespace cc::hold
