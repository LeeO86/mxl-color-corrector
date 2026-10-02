#include "v210.hpp"

#include <cstring>

namespace cc
{
namespace
{
std::uint16_t field(std::uint32_t word, int shift)
{
    return static_cast<std::uint16_t>((word >> shift) & 0x3ffu);
}
} // namespace

void unpackV210Group(std::uint32_t const words[4], std::uint16_t y[6], std::uint16_t cb[3], std::uint16_t cr[3])
{
    cb[0] = field(words[0], 0);
    y[0] = field(words[0], 10);
    cr[0] = field(words[0], 20);
    y[1] = field(words[1], 0);
    cb[1] = field(words[1], 10);
    y[2] = field(words[1], 20);
    cr[1] = field(words[2], 0);
    y[3] = field(words[2], 10);
    cb[2] = field(words[2], 20);
    y[4] = field(words[3], 0);
    cr[2] = field(words[3], 10);
    y[5] = field(words[3], 20);
}

void packV210Group(std::uint32_t words[4], std::uint16_t const y[6], std::uint16_t const cb[3], std::uint16_t const cr[3])
{
    auto s = [](std::uint16_t v) { return static_cast<std::uint32_t>(v & 0x3ffu); };
    words[0] = s(cb[0]) | (s(y[0]) << 10) | (s(cr[0]) << 20);
    words[1] = s(y[1]) | (s(cb[1]) << 10) | (s(y[2]) << 20);
    words[2] = s(cr[1]) | (s(y[3]) << 10) | (s(cb[2]) << 20);
    words[3] = s(y[4]) | (s(cr[2]) << 10) | (s(y[5]) << 20);
}

void unpackV210Line(std::uint8_t const* src, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr)
{
    int const groups = v210Groups(width);
    int x = 0;
    for (int g = 0; g < groups; ++g)
    {
        std::uint32_t words[4] = {};
        std::memcpy(words, src + static_cast<std::size_t>(g) * 16u, sizeof(words));
        std::uint16_t ys[6], cbs[3], crs[3];
        unpackV210Group(words, ys, cbs, crs);
        for (int i = 0; i < 6 && x < width; ++i, ++x)
        {
            y[x] = ys[i];
            if ((x & 1) == 0)
            {
                cb[x / 2] = cbs[i / 2];
                cr[x / 2] = crs[i / 2];
            }
        }
    }
}

void packV210Line(std::uint8_t* dst, int width, int stride, std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr)
{
    if (stride <= 0)
    {
        stride = v210Stride(width);
    }
    std::memset(dst, 0, static_cast<std::size_t>(stride));
    int const groups = v210Groups(width);
    int const cw = width / 2;
    for (int g = 0; g < groups; ++g)
    {
        std::uint16_t ys[6] = {};
        std::uint16_t cbs[3] = {};
        std::uint16_t crs[3] = {};
        for (int i = 0; i < 6; ++i)
        {
            int const x = g * 6 + i;
            if (x < width)
            {
                ys[i] = y[x];
                if ((i % 2) == 0)
                {
                    int const cx = x / 2;
                    if (cx < cw)
                    {
                        cbs[i / 2] = cb[cx];
                        crs[i / 2] = cr[cx];
                    }
                }
            }
        }
        std::uint32_t words[4];
        packV210Group(words, ys, cbs, crs);
        std::memcpy(dst + static_cast<std::size_t>(g) * 16u, words, sizeof(words));
    }
}

} // namespace cc
