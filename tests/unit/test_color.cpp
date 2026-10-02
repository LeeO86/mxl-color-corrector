#include "color/bt709.hpp"
#include "color/controls.hpp"
#include "color/process.hpp"
#include "mode.hpp"
#include "v210.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest/doctest.h"

using namespace cc;

namespace
{
std::uint32_t lcg(std::uint32_t& s)
{
    s = s * 1664525u + 1013904223u;
    return s;
}

void fillLegal(std::vector<std::uint8_t>& frame, int width, int height, std::uint32_t seed)
{
    int const stride = v210Stride(width);
    frame.assign(static_cast<std::size_t>(stride * height), 0);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2));
    for (int row = 0; row < height; ++row)
    {
        for (int x = 0; x < width; ++x)
        {
            y[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(64 + (lcg(seed) % 877));
        }
        for (int x = 0; x < width / 2; ++x)
        {
            cb[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(64 + (lcg(seed) % 897));
            cr[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(64 + (lcg(seed) % 897));
        }
        packV210Line(frame.data() + static_cast<std::size_t>(row * stride), width, stride, y.data(), cb.data(), cr.data());
    }
}

} // namespace

TEST_CASE("v210 roundtrip is bit-exact")
{
    int const width = 96;
    int const height = 4;
    std::vector<std::uint8_t> frame;
    fillLegal(frame, width, height, 1);
    int const stride = v210Stride(width);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width)), cb(static_cast<std::size_t>(width / 2)), cr(static_cast<std::size_t>(width / 2));
    unpackV210Line(frame.data(), width, y.data(), cb.data(), cr.data());
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(stride), 0);
    packV210Line(packed.data(), width, stride, y.data(), cb.data(), cr.data());
    CHECK(std::memcmp(packed.data(), frame.data(), static_cast<std::size_t>(stride)) == 0);
}

TEST_CASE("neutral settings are bit-exact on random legal v210")
{
    int const width = 192;
    int const height = 8;
    std::vector<std::uint8_t> src;
    fillLegal(src, width, height, 42);
    auto matrix = buildMatrix(Controls{});
    CHECK(matrix.identity);
    std::vector<std::uint8_t> dst(src.size(), 0);
    processV210Scalar(src.data(), dst.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    CHECK(std::memcmp(src.data(), dst.data(), src.size()) == 0);
    if (cpuHasAvx2())
    {
        std::vector<std::uint8_t> dst2(src.size(), 0);
        processV210Avx2(src.data(), dst2.data(), width, height, 0, 0, 0, height, matrix, nullptr);
        CHECK(std::memcmp(src.data(), dst2.data(), src.size()) == 0);
    }
}

TEST_CASE("each control stays within 1 LSB of the floating-point reference")
{
    Controls base;
    std::vector<Controls> cases;
    cases.push_back(base);
    base.gain = 50;
    cases.push_back(base);
    base = {};
    base.gain = 150;
    cases.push_back(base);
    base = {};
    base.pedestal = 8;
    cases.push_back(base);
    base = {};
    base.pedestal = -6;
    cases.push_back(base);
    base = {};
    base.brightness = 12;
    cases.push_back(base);
    base = {};
    base.brightness = -9;
    cases.push_back(base);
    base = {};
    base.saturation = 0;
    cases.push_back(base);
    base = {};
    base.saturation = 180;
    cases.push_back(base);
    base = {};
    base.white = {12, -4, -8};
    cases.push_back(base);
    base = {};
    base.black = {3, -1, -2};
    cases.push_back(base);
    base = {};
    base.gain = 80;
    base.pedestal = 4;
    base.brightness = -3;
    base.saturation = 40;
    base.white = {5, -2, -3};
    base.black = {1, -0.5, -0.5};
    cases.push_back(base);

    std::uint32_t seed = 7;
    double worst = 0;
    for (auto controls : cases)
    {
        controls.clip = ClipMode::Off;
        controls.rgbClip = false;
        auto matrix = buildMatrix(controls);
        RgbAffine affine;
        controlsToAffine(controls, affine);
        for (int n = 0; n < 40; ++n)
        {
            int const y = 64 + static_cast<int>(lcg(seed) % 877);
            int const cb = 64 + static_cast<int>(lcg(seed) % 897);
            int const cr = 64 + static_cast<int>(lcg(seed) % 897);
            int oy = 0, oc = 0, orr = 0;
            bool clipped = false;
            correctSample(matrix, y, cb, cr, oy, oc, orr, clipped);
            double ry, rc, rr;
            referenceYcbcr(affine, y, cb, cr, ry, rc, rr);
            ry = std::clamp(ry, 0.0, 1023.0);
            rc = std::clamp(rc, 0.0, 1023.0);
            rr = std::clamp(rr, 0.0, 1023.0);
            worst = std::max(worst, std::fabs(oy - ry));
            worst = std::max(worst, std::fabs(oc - rc));
            worst = std::max(worst, std::fabs(orr - rr));
            CHECK(std::fabs(oy - ry) <= 1.0);
            CHECK(std::fabs(oc - rc) <= 1.0);
            CHECK(std::fabs(orr - rr) <= 1.0);
        }
    }
    CHECK(worst <= 1.0);
}

TEST_CASE("avx2 matches the scalar reference")
{
    if (!cpuHasAvx2())
    {
        return;
    }
    Controls controls;
    controls.gain = 120;
    controls.pedestal = 3;
    controls.brightness = -4;
    controls.saturation = 70;
    controls.white = {6, -3, -3};
    controls.clip = ClipMode::Legal;
    auto matrix = buildMatrix(controls);
    int const width = 96;
    int const height = 3;
    std::vector<std::uint8_t> src;
    fillLegal(src, width, height, 99);
    std::vector<std::uint8_t> a(src.size()), b(src.size());
    processV210Scalar(src.data(), a.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    processV210Avx2(src.data(), b.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("gain moves white but not black")
{
    Controls controls;
    controls.gain = 50;
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, 64, 512, 512, oy, oc, orr, clipped);
    CHECK(oy == 64);
    CHECK(oc == 512);
    CHECK(orr == 512);
    correctSample(matrix, 940, 512, 512, oy, oc, orr, clipped);
    CHECK(oy < 940);
    CHECK(oy > 64);
    CHECK(std::abs(oc - 512) <= 1);
    CHECK(std::abs(orr - 512) <= 1);
}

TEST_CASE("pedestal moves black but not white")
{
    Controls controls;
    controls.pedestal = 10;
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, 64, 512, 512, oy, oc, orr, clipped);
    CHECK(oy > 64);
    correctSample(matrix, 940, 512, 512, oy, oc, orr, clipped);
    CHECK(std::abs(oy - 940) <= 1);
}

TEST_CASE("brightness moves black and white")
{
    Controls controls;
    controls.brightness = 10;
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int black = 0, white = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, 64, 512, 512, black, oc, orr, clipped);
    correctSample(matrix, 940, 512, 512, white, oc, orr, clipped);
    CHECK(black > 64);
    CHECK(white > 940);
}

TEST_CASE("saturation zero is black and white")
{
    Controls controls;
    controls.saturation = 0;
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, 500, 200, 800, oy, oc, orr, clipped);
    CHECK(oc == 512);
    CHECK(orr == 512);
    CHECK(std::abs(oy - 500) <= 2);
}

TEST_CASE("colour wheels are luminance-neutral")
{
    double const angles[] = {0.0, 0.4, -0.7, 1.0, -1.0};
    for (double x : angles)
    {
        for (double y : angles)
        {
            auto white = wheelToTrims(x, y, 20);
            auto black = wheelToTrims(x, y, 5);
            double const lumaW = bt709::kKr * white.r + bt709::kKg * white.g + bt709::kKb * white.b;
            double const lumaB = bt709::kKr * black.r + bt709::kKg * black.g + bt709::kKb * black.b;
            CHECK(std::fabs(lumaW) < 1e-6);
            CHECK(std::fabs(lumaB) < 1e-6);
            CHECK(white.r >= -20.0 - 1e-6);
            CHECK(white.r <= 20.0 + 1e-6);
            CHECK(black.b >= -5.0 - 1e-6);
            CHECK(black.b <= 5.0 + 1e-6);
        }
    }
    Controls controls;
    auto trims = wheelToTrims(0.8, -0.3, 20);
    controls.white = {trims.r, trims.g, trims.b};
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    int const mid = 64 + 438;
    correctSample(matrix, mid, 512, 512, oy, oc, orr, clipped);
    CHECK(std::abs(oy - mid) <= 1);
}

TEST_CASE("legal clipping limits the code range")
{
    Controls controls;
    controls.gain = 200;
    controls.clip = ClipMode::Legal;
    auto matrix = buildMatrix(controls);
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, 940, 512, 512, oy, oc, orr, clipped);
    CHECK(oy == 940);
    CHECK(clipped);
    controls.brightness = -20;
    controls.gain = 100;
    matrix = buildMatrix(controls);
    correctSample(matrix, 64, 200, 800, oy, oc, orr, clipped);
    CHECK(oy == 64);
    CHECK(oc >= 64);
    CHECK(oc <= 960);
    CHECK(orr >= 64);
    CHECK(orr <= 960);

    controls = {};
    controls.gain = 200;
    controls.clip = ClipMode::Extended;
    matrix = buildMatrix(controls);
    correctSample(matrix, 940, 512, 512, oy, oc, orr, clipped);
    CHECK(oy == 1019);

    controls.clip = ClipMode::Off;
    matrix = buildMatrix(controls);
    correctSample(matrix, 940, 512, 512, oy, oc, orr, clipped);
    CHECK(oy == 1023);
}

TEST_CASE("rgb gamut clip pulls known out-of-gamut codes back")
{
    Controls controls;
    controls.rgbClip = true;
    controls.clip = ClipMode::Off;
    auto matrix = buildMatrix(controls);
    int const y = 700;
    int const cb = 64;
    int const cr = 960;
    double R, G, B;
    bt709::codeToRgb(y, cb, cr, R, G, B);
    CHECK((R < 0 || R > 1 || G < 0 || G > 1 || B < 0 || B > 1));
    int oy = 0, oc = 0, orr = 0;
    bool clipped = false;
    correctSample(matrix, y, cb, cr, oy, oc, orr, clipped);
    bt709::codeToRgb(oy, oc, orr, R, G, B);
    CHECK(R >= -0.02);
    CHECK(R <= 1.02);
    CHECK(G >= -0.02);
    CHECK(G <= 1.02);
    CHECK(B >= -0.02);
    CHECK(B <= 1.02);
    CHECK(clipped);
}

TEST_CASE("slice processing matches whole-grain processing")
{
    Controls controls;
    controls.gain = 110;
    controls.saturation = 80;
    controls.pedestal = -2;
    controls.brightness = 3;
    auto matrix = buildMatrix(controls);
    int const width = 96;
    int const height = 48;
    std::vector<std::uint8_t> src;
    fillLegal(src, width, height, 1234);
    std::vector<std::uint8_t> whole(src.size(), 0);
    processV210Scalar(src.data(), whole.data(), width, height, 0, 0, 0, height, matrix, nullptr);
    int const layouts[] = {1, 3, 7, 16, 48};
    for (int lines : layouts)
    {
        std::vector<std::uint8_t> sliced(src.size(), 0);
        for (int row = 0; row < height; row += lines)
        {
            int const end = std::min(height, row + lines);
            processV210Scalar(src.data(), sliced.data(), width, height, 0, 0, row, end, matrix, nullptr);
        }
        CHECK(std::memcmp(whole.data(), sliced.data(), whole.size()) == 0);
    }
}

TEST_CASE("settings swap is latched at grain boundaries")
{
    struct Latch
    {
        FixedMatrix slots[2]{};
        std::uint64_t seq = 0;
        FixedMatrix active{};
        void publish(FixedMatrix m)
        {
            slots[(seq + 1) & 1] = m;
            ++seq;
        }
        void beginGrain()
        {
            active = slots[seq & 1];
        }
    };
    Latch latch;
    auto first = buildMatrix(Controls{}, 1);
    latch.publish(first);
    latch.beginGrain();
    CHECK(latch.active.generation == 1);
    Controls next;
    next.gain = 50;
    latch.publish(buildMatrix(next, 2));
    CHECK(latch.active.generation == 1);
    latch.beginGrain();
    CHECK(latch.active.generation == 2);
    CHECK(latch.active.identity == false);
}

TEST_CASE("fallback mode selection")
{
    int const line = v210Stride(1920);
    auto progressive = decideMode(false, true, 1080, 1080, static_cast<std::uint32_t>(line), line);
    CHECK(progressive.slice);
    CHECK(progressive.reason == FallbackReason::None);
    auto interlaced = decideMode(true, true, 1080, 1080, static_cast<std::uint32_t>(line), line);
    CHECK_FALSE(interlaced.slice);
    CHECK(interlaced.reason == FallbackReason::Interlaced);
    auto whole = decideMode(false, false, 1080, 1080, static_cast<std::uint32_t>(line), line);
    CHECK(whole.reason == FallbackReason::NoSliceCommits);
    auto bad = decideMode(false, true, 1080, 1000, static_cast<std::uint32_t>(line), line);
    CHECK(bad.reason == FallbackReason::SliceLayout);
    auto multi = decideMode(false, true, 1080, 270, static_cast<std::uint32_t>(line * 4), line);
    CHECK(multi.slice);
    auto remainder = decideMode(false, true, 1080, 300, static_cast<std::uint32_t>(line * 3), line);
    CHECK(remainder.reason == FallbackReason::SliceLayout);
}

TEST_CASE("control ranges reject illegal values")
{
    Controls controls;
    CHECK(validateControls(controls).empty());
    controls.gain = 250;
    CHECK_FALSE(validateControls(controls).empty());
    controls.gain = 100;
    controls.white.r = -21;
    CHECK_FALSE(validateControls(controls).empty());
    bool ok = false;
    CHECK(parseClipMode("extended", ok) == ClipMode::Extended);
    CHECK(ok);
    CHECK(parseClipMode("nope", ok) == ClipMode::Legal);
    CHECK_FALSE(ok);
}
