#include "color/controls.hpp"
#include "color/process.hpp"
#include "v210.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
void fill(std::vector<std::uint8_t>& frame, int width, int height, std::uint16_t y0)
{
    int const stride = cc::v210Stride(width);
    frame.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2), 512);
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2), 700);
    for (int row = 0; row < height; ++row)
    {
        for (int x = 0; x < width; ++x) y[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(y0 + (x % 64));
        cc::packV210Line(frame.data() + static_cast<std::size_t>(row * stride), width, stride, y.data(), cb.data(), cr.data());
    }
}

double seconds(int width, int height, cc::Controls const& controls, int frames)
{
    auto matrix = cc::buildMatrix(controls);
    std::vector<std::uint8_t> src;
    fill(src, width, height, 64);
    std::vector<std::uint8_t> dst(src.size());
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i)
    {
        cc::processV210(src.data(), dst.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count() / frames;
}
} // namespace

int main()
{
    cc::Controls plain;
    plain.gain = 110;
    plain.saturation = 90;
    plain.brightness = 2;
    cc::Controls rgb = plain;
    rgb.rgbClip = true;
    struct Case
    {
        char const* name;
        int w;
        int h;
        int fps;
        int frames;
    };
    Case cases[] = {{"1080p50", 1920, 1080, 50, 20}, {"2160p50", 3840, 2160, 50, 8}};
    std::cout << "avx2 " << (cc::cpuHasAvx2() ? "yes" : "no") << "\n";
    for (auto const& item : cases)
    {
        double fast = seconds(item.w, item.h, plain, item.frames);
        double slow = seconds(item.w, item.h, rgb, std::max(2, item.frames / 4));
        double fastChannels = 1.0 / (fast * item.fps);
        double slowChannels = 1.0 / (slow * item.fps);
        std::cout << item.name << " matrix " << fast * 1000.0 << " ms/frame  channels/core " << fastChannels << "\n";
        std::cout << item.name << " rgb-clip " << slow * 1000.0 << " ms/frame  channels/core " << slowChannels << "\n";
    }
    return 0;
}
