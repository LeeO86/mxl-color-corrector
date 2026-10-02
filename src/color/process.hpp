#pragma once

#include "color/controls.hpp"

#include <cstdint>

namespace cc
{

// Fixed-point 3×4 matrix in the Y′CbCr code domain.
// out = (m0*Y + m1*Cb + m2*Cr + m3 + 32768) >> 16
// Coefficients are in 1/65536 units. identity is exact when the controls are neutral.
struct FixedMatrix
{
    std::int32_t m[3][4]{};
    bool identity = true;
    bool bypass = false;
    bool rgbClip = false;
    int yMin = 64;
    int yMax = 940;
    int cMin = 64;
    int cMax = 960;
    std::uint64_t generation = 0;
};

[[nodiscard]] FixedMatrix buildMatrix(Controls const& controls, std::uint64_t generation = 0);

struct ProcessStats
{
    std::uint64_t clipped = 0;
    std::uint64_t samples = 0;
};

// Co-siting: v210 chroma belongs to the even luma sample. Odd luma reuses that
// pair. Output chroma is taken from the even sample only.
void processV210Scalar(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats);

void processV210Avx2(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats);

[[nodiscard]] bool cpuHasAvx2();

// Dispatches to AVX2 when the CPU supports it and RGB gamut clip is off.
void processV210(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats);

// One pixel, used by tests. chromaOut is meaningful for the co-sited (even) sample.
void correctSample(FixedMatrix const& matrix, int y, int cb, int cr, int& outY, int& outCb, int& outCr, bool& clipped);

} // namespace cc
