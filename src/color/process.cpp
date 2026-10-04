#include "color/process.hpp"

#include "color/bt709.hpp"
#include "v210.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace cc
{
namespace
{
void clipLimits(ClipMode mode, int& yMin, int& yMax, int& cMin, int& cMax)
{
    switch (mode)
    {
        case ClipMode::Extended:
            yMin = cMin = 4;
            yMax = cMax = 1019;
            break;
        case ClipMode::Off:
            yMin = cMin = 0;
            yMax = cMax = 1023;
            break;
        case ClipMode::Legal:
        default:
            yMin = 64;
            yMax = 940;
            cMin = 64;
            cMax = 960;
            break;
    }
}

int roundShift(std::int64_t acc)
{
    acc += 32768;
    return static_cast<int>(acc >> 16);
}

int applyRow(FixedMatrix const& matrix, int row, int y, int cb, int cr)
{
    std::int64_t const acc = static_cast<std::int64_t>(matrix.m[row][0]) * y + static_cast<std::int64_t>(matrix.m[row][1]) * cb +
                             static_cast<std::int64_t>(matrix.m[row][2]) * cr + matrix.m[row][3];
    return roundShift(acc);
}

int clampCode(int v, int lo, int hi, bool& clipped)
{
    if (v < lo)
    {
        clipped = true;
        return lo;
    }
    if (v > hi)
    {
        clipped = true;
        return hi;
    }
    return v;
}

// Returns true when the codes were pulled back into the RGB cube.
bool rgbClipCodes(int& Y, int& Cb, int& Cr)
{
    double R, G, B;
    bt709::codeToRgb(Y, Cb, Cr, R, G, B);
    constexpr double kEps = 1e-6;
    bool outside = R < -kEps || R > 1.0 + kEps || G < -kEps || G > 1.0 + kEps || B < -kEps || B > 1.0 + kEps;
    if (!outside)
    {
        return false;
    }
    R = std::clamp(R, 0.0, 1.0);
    G = std::clamp(G, 0.0, 1.0);
    B = std::clamp(B, 0.0, 1.0);
    double oY, oCb, oCr;
    bt709::rgbToCode(R, G, B, oY, oCb, oCr);
    Y = static_cast<int>(std::llround(oY));
    Cb = static_cast<int>(std::llround(oCb));
    Cr = static_cast<int>(std::llround(oCr));
    return true;
}

bool sampleInRange(int y, int cb, int cr, FixedMatrix const& matrix)
{
    return y >= matrix.yMin && y <= matrix.yMax && cb >= matrix.cMin && cb <= matrix.cMax && cr >= matrix.cMin && cr <= matrix.cMax;
}

void processLineScalar(std::uint8_t const* src, std::uint8_t* dst, int width, int stride, FixedMatrix const& matrix, ProcessStats* stats)
{
    if (matrix.bypass || (matrix.identity && !matrix.rgbClip))
    {
        bool clean = matrix.bypass || matrix.yMin == 0;
        if (!clean && matrix.identity)
        {
            clean = true;
            int const groups = v210Groups(width);
            int x = 0;
            for (int g = 0; g < groups && clean; ++g)
            {
                std::uint32_t words[4] = {};
                std::memcpy(words, src + static_cast<std::size_t>(g) * 16u, sizeof(words));
                std::uint16_t ys[6], cbs[3], crs[3];
                unpackV210Group(words, ys, cbs, crs);
                for (int i = 0; i < 6 && x < width; ++i, ++x)
                {
                    int const cb = cbs[i / 2];
                    int const cr = crs[i / 2];
                    if (!sampleInRange(ys[i], cb, cr, matrix))
                    {
                        clean = false;
                        break;
                    }
                }
            }
        }
        if (clean || matrix.bypass)
        {
            std::memcpy(dst, src, static_cast<std::size_t>(stride));
            if (stats)
            {
                stats->samples += static_cast<std::uint64_t>(width) * 2u;
            }
            return;
        }
    }

    int const groups = v210Groups(width);
    std::memset(dst, 0, static_cast<std::size_t>(stride));
    int x = 0;
    for (int g = 0; g < groups; ++g)
    {
        std::uint32_t words[4] = {};
        std::memcpy(words, src + static_cast<std::size_t>(g) * 16u, sizeof(words));
        std::uint16_t ys[6], cbs[3], crs[3];
        unpackV210Group(words, ys, cbs, crs);
        std::uint16_t oy[6] = {};
        std::uint16_t oc[3] = {};
        std::uint16_t orr[3] = {};
        for (int i = 0; i < 6 && x < width; ++i, ++x)
        {
            int const cb = cbs[i / 2];
            int const cr = crs[i / 2];
            bool clipped = false;
            int outY = 0, outCb = 0, outCr = 0;
            correctSample(matrix, ys[i], cb, cr, outY, outCb, outCr, clipped);
            oy[i] = static_cast<std::uint16_t>(outY);
            if ((i % 2) == 0)
            {
                oc[i / 2] = static_cast<std::uint16_t>(outCb);
                orr[i / 2] = static_cast<std::uint16_t>(outCr);
            }
            if (stats)
            {
                stats->samples += 2;
                if (clipped)
                {
                    stats->clipped += 1;
                }
            }
        }
        std::uint32_t outWords[4];
        packV210Group(outWords, oy, oc, orr);
        std::memcpy(dst + static_cast<std::size_t>(g) * 16u, outWords, sizeof(outWords));
    }
}
} // namespace

FixedMatrix buildMatrix(Controls const& controls, std::uint64_t generation)
{
    FixedMatrix matrix;
    matrix.generation = generation;
    matrix.bypass = controls.bypass;
    matrix.rgbClip = controls.rgbClip;
    clipLimits(controls.clip, matrix.yMin, matrix.yMax, matrix.cMin, matrix.cMax);

    RgbAffine affine;
    controlsToAffine(controls, affine);
    auto eval = [&](double Y, double Cb, double Cr) {
        double oY, oCb, oCr;
        referenceYcbcr(affine, Y, Cb, Cr, oY, oCb, oCr);
        return std::array<double, 3>{oY, oCb, oCr};
    };
    // The pipeline without clipping is exactly affine, so four probes define it.
    auto const z = eval(0, 0, 0);
    auto const dY = eval(1, 0, 0);
    auto const dCb = eval(0, 1, 0);
    auto const dCr = eval(0, 0, 1);
    for (int row = 0; row < 3; ++row)
    {
        double const cols[4] = {dY[row] - z[row], dCb[row] - z[row], dCr[row] - z[row], z[row]};
        for (int col = 0; col < 4; ++col)
        {
            long long q = std::llround(cols[col] * 65536.0);
            q = std::clamp(q, static_cast<long long>(std::numeric_limits<std::int32_t>::min()),
                static_cast<long long>(std::numeric_limits<std::int32_t>::max()));
            matrix.m[row][col] = static_cast<std::int32_t>(q);
        }
    }
    static constexpr std::int32_t kIdent[3][4] = {{65536, 0, 0, 0}, {0, 65536, 0, 0}, {0, 0, 65536, 0}};
    matrix.identity = true;
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            if (matrix.m[row][col] != kIdent[row][col])
            {
                matrix.identity = false;
            }
        }
    }
    if (isNeutral(controls))
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                matrix.m[row][col] = kIdent[row][col];
            }
        }
        matrix.identity = true;
    }
    return matrix;
}

void correctSample(FixedMatrix const& matrix, int y, int cb, int cr, int& outY, int& outCb, int& outCr, bool& clipped)
{
    clipped = false;
    if (matrix.bypass)
    {
        outY = y;
        outCb = cb;
        outCr = cr;
        return;
    }
    if (matrix.identity && !matrix.rgbClip)
    {
        outY = y;
        outCb = cb;
        outCr = cr;
    }
    else
    {
        outY = matrix.identity ? y : applyRow(matrix, 0, y, cb, cr);
        outCb = matrix.identity ? cb : applyRow(matrix, 1, y, cb, cr);
        outCr = matrix.identity ? cr : applyRow(matrix, 2, y, cb, cr);
        if (matrix.rgbClip && rgbClipCodes(outY, outCb, outCr))
        {
            clipped = true;
        }
    }
    outY = clampCode(outY, matrix.yMin, matrix.yMax, clipped);
    outCb = clampCode(outCb, matrix.cMin, matrix.cMax, clipped);
    outCr = clampCode(outCr, matrix.cMin, matrix.cMax, clipped);
    outY = std::clamp(outY, 0, 1023);
    outCb = std::clamp(outCb, 0, 1023);
    outCr = std::clamp(outCr, 0, 1023);
}

void processV210Scalar(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats)
{
    if (srcStride <= 0)
    {
        srcStride = v210Stride(width);
    }
    if (dstStride <= 0)
    {
        dstStride = v210Stride(width);
    }
    if (rowBegin < 0)
    {
        rowBegin = 0;
    }
    if (rowEnd > height)
    {
        rowEnd = height;
    }
    for (int row = rowBegin; row < rowEnd; ++row)
    {
        processLineScalar(src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcStride),
            dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstStride), width, dstStride, matrix, stats);
    }
}

bool cpuHasAvx2()
{
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0)
    {
        return false;
    }
    bool const osxsave = (ecx & (1u << 27)) != 0;
    bool const avx = (ecx & (1u << 28)) != 0;
    if (!osxsave || !avx)
    {
        return false;
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0)
    {
        return false;
    }
    if ((ebx & (1u << 5)) == 0)
    {
        return false;
    }
    unsigned int xcr = 0, xcrHi = 0;
    __asm__ volatile("xgetbv" : "=a"(xcr), "=d"(xcrHi) : "c"(0));
    return (xcr & 0x6u) == 0x6u;
#else
    return false;
#endif
}

void processV210(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats)
{
    if (!matrix.bypass && cpuHasAvx2())
    {
        processV210Avx2(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
        return;
    }
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
}

} // namespace cc
