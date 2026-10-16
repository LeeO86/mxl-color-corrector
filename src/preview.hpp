#pragma once

#include <cstdint>
#include <vector>

namespace cc
{

// `height` lines of v210 at `stride` bytes. `displayHeight` (0: `height`) sets the picture's aspect:
// a field of an interlaced flow (half the frame's lines) is shown at the frame's.
[[nodiscard]] std::vector<std::uint8_t> encodePreviewJpeg(std::uint8_t const* v210, int width, int height, int stride, int maxWidth = 320,
    int displayHeight = 0);

} // namespace cc
