#pragma once

#include <cstdint>
#include <vector>

namespace cc
{

[[nodiscard]] std::vector<std::uint8_t> encodePreviewJpeg(std::uint8_t const* v210, int width, int height, int stride, int maxWidth = 320);

} // namespace cc
