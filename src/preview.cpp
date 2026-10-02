#include "preview.hpp"

#include "color/bt709.hpp"
#include "v210.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_image_write.h"
#pragma GCC diagnostic pop

namespace cc
{

std::vector<std::uint8_t> encodePreviewJpeg(std::uint8_t const* v210, int width, int height, int stride, int maxWidth)
{
    if (v210 == nullptr || width < 2 || height < 1 || stride <= 0) return {};
    int outW = std::min(width, maxWidth);
    if (outW % 2) --outW;
    if (outW < 2) outW = 2;
    int const outH = std::max(1, height * outW / width);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(outW * outH * 3));
    for (int y = 0; y < outH; ++y)
    {
        int const srcY = std::min(height - 1, y * height / outH);
        std::vector<std::uint16_t> lineY(static_cast<std::size_t>(width));
        std::vector<std::uint16_t> lineCb(static_cast<std::size_t>(width / 2));
        std::vector<std::uint16_t> lineCr(static_cast<std::size_t>(width / 2));
        unpackV210Line(v210 + static_cast<std::size_t>(srcY) * static_cast<std::size_t>(stride), width, lineY.data(), lineCb.data(), lineCr.data());
        for (int x = 0; x < outW; ++x)
        {
            int const srcX = std::min(width - 1, x * width / outW);
            double R, G, B;
            bt709::codeToRgb(lineY[static_cast<std::size_t>(srcX)], lineCb[static_cast<std::size_t>(srcX / 2)],
                lineCr[static_cast<std::size_t>(srcX / 2)], R, G, B);
            auto byte = [](double v) {
                v = std::clamp(v, 0.0, 1.0);
                return static_cast<std::uint8_t>(std::lround(v * 255.0));
            };
            auto* px = rgb.data() + static_cast<std::size_t>((y * outW + x) * 3);
            px[0] = byte(R);
            px[1] = byte(G);
            px[2] = byte(B);
        }
    }
    std::vector<std::uint8_t> jpeg;
    stbi_write_jpg_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<std::uint8_t>*>(context);
            auto* bytes = static_cast<std::uint8_t*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &jpeg, outW, outH, 3, rgb.data(), 70);
    return jpeg;
}

} // namespace cc
