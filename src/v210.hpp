#pragma once

#include <cstddef>
#include <cstdint>

namespace cc
{

// MXL line length for video/v210, including the 48-pixel / 128-byte padding.
[[nodiscard]] inline int v210Stride(int width)
{
    return ((width + 47) / 48) * 128;
}

[[nodiscard]] inline int v210Groups(int width)
{
    return (width + 5) / 6;
}

void unpackV210Group(std::uint32_t const words[4], std::uint16_t y[6], std::uint16_t cb[3], std::uint16_t cr[3]);
void packV210Group(std::uint32_t words[4], std::uint16_t const y[6], std::uint16_t const cb[3], std::uint16_t const cr[3]);

// Tight helpers used by tests and the preview. Stride is in bytes.
void unpackV210Line(std::uint8_t const* src, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr);
void packV210Line(std::uint8_t* dst, int width, int stride, std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr);

} // namespace cc
