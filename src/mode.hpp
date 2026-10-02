#pragma once

#include <cstdint>
#include <string>

namespace cc
{

enum class FallbackReason
{
    None,
    Interlaced,
    NoSliceCommits,
    SliceLayout
};

[[nodiscard]] inline char const* fallbackName(FallbackReason reason)
{
    switch (reason)
    {
        case FallbackReason::Interlaced:
            return "interlaced";
        case FallbackReason::NoSliceCommits:
            return "no_slice_commits";
        case FallbackReason::SliceLayout:
            return "slice_layout";
        case FallbackReason::None:
        default:
            return "none";
    }
}

struct ModeDecision
{
    bool slice = true;
    FallbackReason reason = FallbackReason::None;
};

// Slice mode requires progressive video whose MXL slice size is an integer
// number of v210 lines and whose height divides that slice.
[[nodiscard]] inline ModeDecision decideMode(bool interlaced, bool sourceCommitsSlices, int height, int totalSlices, std::uint32_t sliceBytes,
    int v210LineBytes)
{
    if (interlaced)
    {
        return {false, FallbackReason::Interlaced};
    }
    if (!sourceCommitsSlices)
    {
        return {false, FallbackReason::NoSliceCommits};
    }
    if (height <= 0 || totalSlices <= 0 || sliceBytes == 0 || v210LineBytes <= 0 || (sliceBytes % static_cast<std::uint32_t>(v210LineBytes)) != 0)
    {
        return {false, FallbackReason::SliceLayout};
    }
    int const linesPerSlice = static_cast<int>(sliceBytes / static_cast<std::uint32_t>(v210LineBytes));
    if (linesPerSlice <= 0 || (height % linesPerSlice) != 0 || totalSlices != height / linesPerSlice)
    {
        return {false, FallbackReason::SliceLayout};
    }
    return {true, FallbackReason::None};
}

} // namespace cc
