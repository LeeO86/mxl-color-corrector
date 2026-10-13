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

// AVX-512 on 256-bit registers (masks, 32 registers) and on 512-bit registers. Only on a CPU with
// cpuHasAvx512().
void processV210Avx512Vl(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats);
void processV210Avx512(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats);

[[nodiscard]] bool cpuHasAvx2();
[[nodiscard]] bool cpuHasAvx512();

// Runs the code of simdLevel() (CC_SIMD); every level is bit-exact with the scalar path.
void processV210(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats);

namespace detail
{
// Groups [gBegin, gEnd) of one line, one group at a time (AVX2): the groups after the last block
// and matrices whose sums could leave 32 bits.
void processGroupsAvx2(std::uint8_t const* src, std::uint8_t* dst, int gBegin, int gEnd, int width, FixedMatrix const& matrix,
    ProcessStats* stats);
} // namespace detail

// One pixel, used by tests. chromaOut is meaningful for the co-sited (even) sample.
void correctSample(FixedMatrix const& matrix, int y, int cb, int cr, int& outY, int& outCb, int& outCr, bool& clipped);

} // namespace cc
