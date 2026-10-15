#include "hold.hpp"

#include "v210.hpp"

#include <algorithm>

namespace cc::hold
{

std::vector<std::uint8_t> blackV210(int width, int height, std::size_t size)
{
    std::vector<std::uint8_t> frame(size, 0);
    if (width < 2 || height < 1) return frame;
    int const stride = v210Stride(width);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width), 64);
    std::vector<std::uint16_t> c(static_cast<std::size_t>((width + 1) / 2), 512);
    std::vector<std::uint8_t> line(static_cast<std::size_t>(stride), 0);
    packV210Line(line.data(), width, stride, y.data(), c.data(), c.data());
    for (int row = 0; row < height; ++row)
    {
        auto const at = static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
        if (at >= size) break;
        std::copy_n(line.begin(), std::min<std::size_t>(line.size(), size - at), frame.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return frame;
}

} // namespace cc::hold
