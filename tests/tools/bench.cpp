// mxl-cc-bench: colour processing time per frame on one thread.
//
//   mxl-cc-bench
//     the CPU's levels, then the dispatched level (CC_SIMD) for matrix and RGB gamut clip at
//     1080p50 and 2160p50 (the CI compares this part with main), then every level this CPU has
//     at 1080p with the statistics on, as the engine runs.
//   mxl-cc-bench cpu
//     only the first line: what the CPU offers and the auto level.
//   mxl-cc-bench table
//     the first line and the table of every level.
//   mxl-cc-bench run <level> <matrix|rgb> <bench|camera> <height> <frames> <stats on|off> <seconds> [fps]
//     one case; level auto|scalar|avx2|avx512vl|avx512; frames > 1 rotates through that many
//     different frames (8 × 5.5 MB is more than the L3 cache: a new grain per call, as in production);
//     fps > 0 paces the calls (sleeping between frames) for co-located measurements.
#include "color/controls.hpp"
#include "color/process.hpp"
#include "color/simd.hpp"
#include "v210.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
// A ramp on Cb 512, Cr 700: most pixels take the exact RGB gamut clip test.
void benchPicture(std::vector<std::uint8_t>& frame, int width, int height)
{
    int const stride = cc::v210Stride(width);
    frame.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2), 512);
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2), 700);
    for (int row = 0; row < height; ++row)
    {
        for (int x = 0; x < width; ++x) y[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(64 + (x % 64));
        cc::packV210Line(frame.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), width, stride, y.data(), cb.data(), cr.data());
    }
}

// Camera-like: smooth gradients plus noise inside the legal range; most pixels skip the exact test.
void cameraPicture(std::vector<std::uint8_t>& frame, int width, int height, int seed)
{
    std::uint32_t rng = 12345u + static_cast<std::uint32_t>(seed);
    auto noise = [&rng](int span) {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<int>((rng >> 8) % static_cast<std::uint32_t>(span)) - span / 2;
    };
    int const stride = cc::v210Stride(width);
    frame.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2));
    for (int row = 0; row < height; ++row)
    {
        for (int x = 0; x < width; ++x)
        {
            y[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(200 + ((x + seed * 7) * 500) / width + (row * 150) / height + noise(16));
        }
        for (int c = 0; c < width / 2; ++c)
        {
            cb[static_cast<std::size_t>(c)] = static_cast<std::uint16_t>(512 + ((c - width / 4) * 120) / width + noise(8));
            cr[static_cast<std::size_t>(c)] = static_cast<std::uint16_t>(512 + ((row - height / 2) * 100) / height + noise(8));
        }
        cc::packV210Line(frame.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), width, stride, y.data(), cb.data(), cr.data());
    }
}

cc::Controls benchControls(bool rgb)
{
    cc::Controls controls;
    controls.gain = 110;
    controls.saturation = 90;
    controls.brightness = 2;
    controls.rgbClip = rgb;
    return controls;
}

void process(cc::SimdLevel level, std::uint8_t const* src, std::uint8_t* dst, int width, int height, cc::FixedMatrix const& matrix,
    cc::ProcessStats* stats)
{
    switch (level)
    {
        case cc::SimdLevel::Avx512:
            cc::processV210Avx512(src, dst, width, height, 0, 0, 0, height, matrix, stats);
            break;
        case cc::SimdLevel::Avx512Vl:
            cc::processV210Avx512Vl(src, dst, width, height, 0, 0, 0, height, matrix, stats);
            break;
        case cc::SimdLevel::Avx2:
            cc::processV210Avx2(src, dst, width, height, 0, 0, 0, height, matrix, stats);
            break;
        case cc::SimdLevel::Scalar:
            cc::processV210Scalar(src, dst, width, height, 0, 0, 0, height, matrix, stats);
            break;
    }
}

struct Result
{
    double msPerFrame = 0;
    long frames = 0;
    std::uint64_t clipped = 0;
};

// Warm-up for 1 s (page faults, caches, clock), then the timed loop. With fps > 0 the time is
// the busy time per frame.
Result run(cc::SimdLevel level, bool rgb, bool camera, int width, int height, int rotation, bool withStats, double seconds, double fps)
{
    auto const matrix = cc::buildMatrix(benchControls(rgb));
    std::vector<std::vector<std::uint8_t>> src(static_cast<std::size_t>(rotation)), dst(static_cast<std::size_t>(rotation));
    for (int i = 0; i < rotation; ++i)
    {
        auto& frame = src[static_cast<std::size_t>(i)];
        camera ? cameraPicture(frame, width, height, i) : benchPicture(frame, width, height);
        dst[static_cast<std::size_t>(i)].assign(frame.size(), 0);
    }
    cc::ProcessStats stats;
    auto step = [&](long n) {
        auto const i = static_cast<std::size_t>(n % rotation);
        process(level, src[i].data(), dst[i].data(), width, height, matrix, withStats ? &stats : nullptr);
    };
    using clock = std::chrono::steady_clock;
    auto const warmEnd = clock::now() + std::chrono::seconds(1);
    long n = 0;
    while (clock::now() < warmEnd) step(n++);
    Result result;
    auto const t0 = clock::now();
    auto const end = t0 + std::chrono::duration<double>(seconds);
    auto now = t0;
    double busy = 0;
    while (now < end)
    {
        auto const s = clock::now();
        step(result.frames++);
        now = clock::now();
        busy += std::chrono::duration<double, std::milli>(now - s).count();
        if (fps > 0)
        {
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(result.frames / fps)));
            now = clock::now();
        }
    }
    result.msPerFrame = busy / static_cast<double>(result.frames);
    result.clipped = stats.clipped;
    return result;
}

double seconds(int width, int height, cc::Controls const& controls, int frames)
{
    auto matrix = cc::buildMatrix(controls);
    std::vector<std::uint8_t> src;
    benchPicture(src, width, height);
    std::vector<std::uint8_t> dst(src.size());
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i)
    {
        cc::processV210(src.data(), dst.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count() / frames;
}

bool parseLevel(std::string const& text, cc::SimdLevel& level)
{
    if (!cc::validSimdRequest(text)) return false;
    auto const choice = cc::chooseSimd(text, cc::detectCpuSimd());
    if (!choice.warning.empty()) return false;
    level = choice.level;
    return true;
}

void printRun(cc::SimdLevel level, std::string const& path, std::string const& picture, int width, int height, int rotation, bool withStats,
    Result const& r)
{
    double const nsPerPixel = r.msPerFrame * 1e6 / (static_cast<double>(width) * height);
    std::cout << cc::simdName(level) << " " << path << " " << picture << " " << width << "x" << height << " frames=" << rotation
              << " stats=" << (withStats ? "on" : "off") << " ms/frame=" << r.msPerFrame << " ns/px=" << nsPerPixel
              << " cores@50fps=" << r.msPerFrame * 50.0 / 1000.0 << " clipped=" << r.clipped << " n=" << r.frames << "\n";
}
} // namespace

int main(int argc, char** argv)
{
    auto const cpu = cc::detectCpuSimd();
    if (argc >= 9 && std::string(argv[1]) == "run")
    {
        cc::SimdLevel level{};
        if (!parseLevel(argv[2], level))
        {
            std::cerr << "level " << argv[2] << " is not available on this CPU\n";
            return 2;
        }
        std::string const path = argv[3];
        std::string const picture = argv[4];
        int const height = std::atoi(argv[5]);
        int const width = height == 2160 ? 3840 : height == 720 ? 1280 : 1920;
        int const rotation = std::max(1, std::atoi(argv[6]));
        bool const withStats = std::string(argv[7]) == "on";
        double const secs = std::atof(argv[8]);
        double const fps = argc > 9 ? std::atof(argv[9]) : 0.0;
        auto const r = run(level, path == "rgb", picture == "camera", width, height, rotation, withStats, secs, fps);
        printRun(level, path, picture, width, height, rotation, withStats, r);
        return 0;
    }

    cc::Controls plain = benchControls(false);
    cc::Controls rgb = benchControls(true);
    struct Case
    {
        char const* name;
        int w;
        int h;
        int fps;
        int frames;
    };
    std::cout << "avx2 " << (cpu.avx2 ? "yes" : "no") << " avx512 " << (cpu.avx512 ? "yes" : "no") << " family " << (cpu.family.empty() ? "-" : cpu.family)
              << " auto " << cc::simdName(cc::autoSimdLevel(cpu)) << "\n";
    if (argc >= 2 && std::string(argv[1]) == "cpu")
    {
        return 0;
    }
    // The dispatched level at 1080p50 and 2160p50 (the CI compares this part with main).
    std::vector<Case> cases;
    if (!(argc >= 2 && std::string(argv[1]) == "table"))
    {
        std::cout << "level " << cc::simdName(cc::simdLevel()) << "\n";
        cases = {{"1080p50", 1920, 1080, 50, 20}, {"2160p50", 3840, 2160, 50, 8}};
    }
    for (auto const& item : cases)
    {
        double fast = seconds(item.w, item.h, plain, item.frames);
        double slow = seconds(item.w, item.h, rgb, std::max(2, item.frames / 4));
        double fastChannels = 1.0 / (fast * item.fps);
        double slowChannels = 1.0 / (slow * item.fps);
        std::cout << item.name << " matrix " << fast * 1000.0 << " ms/frame  channels/core " << fastChannels << "\n";
        std::cout << item.name << " rgb-clip " << slow * 1000.0 << " ms/frame  channels/core " << slowChannels << "\n";
    }
    std::cout << "--- every level, 1080p, statistics on, 8 frames (cold)\n";
    std::vector<cc::SimdLevel> levels{cc::SimdLevel::Scalar};
    if (cpu.avx2) levels.push_back(cc::SimdLevel::Avx2);
    if (cpu.avx512)
    {
        levels.push_back(cc::SimdLevel::Avx512Vl);
        levels.push_back(cc::SimdLevel::Avx512);
    }
    for (auto const level : levels)
    {
        for (char const* path : {"matrix", "rgb"})
        {
            for (char const* picture : {"camera", "bench"})
            {
                bool const isRgb = std::string(path) == "rgb";
                double const secs = level == cc::SimdLevel::Scalar ? 1.0 : 2.0;
                auto const r = run(level, isRgb, std::string(picture) == "camera", 1920, 1080, 8, true, secs, 0);
                printRun(level, path, picture, 1920, 1080, 8, true, r);
            }
        }
    }
    return 0;
}
