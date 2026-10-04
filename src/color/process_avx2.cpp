#include "color/process.hpp"

#include "v210.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace cc
{
namespace
{
#if defined(__x86_64__) || defined(_M_X64)
__m256i srai64_16(__m256i v)
{
    __m256i const shifted = _mm256_srli_epi64(v, 16);
    __m256i const sign = _mm256_cmpgt_epi64(_mm256_setzero_si256(), v);
    __m256i const fill = _mm256_slli_epi64(sign, 48);
    return _mm256_or_si256(shifted, fill);
}

void dot8(std::int32_t const* y, std::int32_t const* cb, std::int32_t const* cr, std::int32_t m0, std::int32_t m1, std::int32_t m2, std::int32_t m3,
    std::int32_t* out)
{
    __m256i const vy = _mm256_load_si256(reinterpret_cast<__m256i const*>(y));
    __m256i const vcb = _mm256_load_si256(reinterpret_cast<__m256i const*>(cb));
    __m256i const vcr = _mm256_load_si256(reinterpret_cast<__m256i const*>(cr));
    __m256i const c0 = _mm256_set1_epi32(m0);
    __m256i const c1 = _mm256_set1_epi32(m1);
    __m256i const c2 = _mm256_set1_epi32(m2);

    auto mulAdd = [](__m256i a, __m256i b, __m256i& even, __m256i& odd) {
        __m256i const aOdd = _mm256_srli_si256(a, 4);
        __m256i const bOdd = _mm256_srli_si256(b, 4);
        even = _mm256_add_epi64(even, _mm256_mul_epi32(a, b));
        odd = _mm256_add_epi64(odd, _mm256_mul_epi32(aOdd, bOdd));
    };
    __m256i even = _mm256_setzero_si256();
    __m256i odd = _mm256_setzero_si256();
    mulAdd(vy, c0, even, odd);
    mulAdd(vcb, c1, even, odd);
    mulAdd(vcr, c2, even, odd);
    __m256i const bias = _mm256_set1_epi64x(static_cast<long long>(m3) + 32768);
    even = srai64_16(_mm256_add_epi64(even, bias));
    odd = srai64_16(_mm256_add_epi64(odd, bias));

    alignas(32) std::int64_t e[4];
    alignas(32) std::int64_t o[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(e), even);
    _mm256_store_si256(reinterpret_cast<__m256i*>(o), odd);
    // even lanes are products 0,2,4,6 and odd lanes are 1,3,5,7.
    out[0] = static_cast<std::int32_t>(e[0]);
    out[1] = static_cast<std::int32_t>(o[0]);
    out[2] = static_cast<std::int32_t>(e[1]);
    out[3] = static_cast<std::int32_t>(o[1]);
    out[4] = static_cast<std::int32_t>(e[2]);
    out[5] = static_cast<std::int32_t>(o[2]);
    out[6] = static_cast<std::int32_t>(e[3]);
    out[7] = static_cast<std::int32_t>(o[3]);
}

int clampTo(int v, int lo, int hi, bool& clipped)
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

// One group the old way: the scalar unpack, the dot product across 8 lanes, the
// scalar clamp and pack. For the groups after the last block of eight, and for
// matrices whose sums could leave 32 bits.
void processGroupDot8(std::uint8_t const* src, std::uint8_t* dst, int g, int width, FixedMatrix const& matrix, ProcessStats* stats)
{
    std::uint32_t words[4] = {};
    std::memcpy(words, src + static_cast<std::size_t>(g) * 16u, sizeof(words));
    std::uint16_t gy[6], gc[3], gr[3];
    unpackV210Group(words, gy, gc, gr);
    alignas(32) std::int32_t y[8] = {};
    alignas(32) std::int32_t cb[8] = {};
    alignas(32) std::int32_t cr[8] = {};
    int const live = std::min(6, width - g * 6);
    for (int i = 0; i < live; ++i)
    {
        y[i] = gy[i];
        cb[i] = gc[i / 2];
        cr[i] = gr[i / 2];
    }
    alignas(32) std::int32_t oY[8], oC[8], oR[8];
    dot8(y, cb, cr, matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3], oY);
    dot8(y, cb, cr, matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3], oC);
    dot8(y, cb, cr, matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3], oR);
    std::uint16_t oy[6] = {};
    std::uint16_t oc[3] = {};
    std::uint16_t orr[3] = {};
    for (int i = 0; i < live; ++i)
    {
        bool clipped = false;
        int yy = clampTo(oY[i], matrix.yMin, matrix.yMax, clipped);
        yy = std::max(0, std::min(1023, yy));
        oy[i] = static_cast<std::uint16_t>(yy);
        if ((i % 2) == 0)
        {
            int cc = clampTo(oC[i], matrix.cMin, matrix.cMax, clipped);
            int rr = clampTo(oR[i], matrix.cMin, matrix.cMax, clipped);
            cc = std::max(0, std::min(1023, cc));
            rr = std::max(0, std::min(1023, rr));
            oc[i / 2] = static_cast<std::uint16_t>(cc);
            orr[i / 2] = static_cast<std::uint16_t>(rr);
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

// True when every row's sum fits a 32-bit lane for 10-bit inputs, rounding included.
bool fits32(FixedMatrix const& matrix)
{
    for (auto const& row : matrix.m)
    {
        std::int64_t const bound = (std::llabs(row[0]) + std::llabs(row[1]) + std::llabs(row[2])) * 1023 + std::llabs(row[3]) + 32768;
        if (bound > std::numeric_limits<std::int32_t>::max())
        {
            return false;
        }
    }
    return true;
}

// 4×4 transpose of 32-bit words within each 128-bit lane. Its own inverse.
void transpose(__m256i& a, __m256i& b, __m256i& c, __m256i& d)
{
    __m256i const t0 = _mm256_unpacklo_epi32(a, b);
    __m256i const t1 = _mm256_unpacklo_epi32(c, d);
    __m256i const t2 = _mm256_unpackhi_epi32(a, b);
    __m256i const t3 = _mm256_unpackhi_epi32(c, d);
    a = _mm256_unpacklo_epi64(t0, t1);
    b = _mm256_unpackhi_epi64(t0, t1);
    c = _mm256_unpacklo_epi64(t2, t3);
    d = _mm256_unpackhi_epi64(t2, t3);
}

struct Lanes
{
    __m256i m[3][3];
    __m256i bias[3];
    __m256i lo[3];
    __m256i hi[3];
    __m256i mask;
    __m256i zero;
    __m256i max10;

    explicit Lanes(FixedMatrix const& matrix)
    {
        int const lo3[3] = {matrix.yMin, matrix.cMin, matrix.cMin};
        int const hi3[3] = {matrix.yMax, matrix.cMax, matrix.cMax};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                m[r][c] = _mm256_set1_epi32(matrix.m[r][c]);
            }
            bias[r] = _mm256_set1_epi32(matrix.m[r][3] + 32768);
            lo[r] = _mm256_set1_epi32(lo3[r]);
            hi[r] = _mm256_set1_epi32(hi3[r]);
        }
        mask = _mm256_set1_epi32(0x3ff);
        zero = _mm256_setzero_si256();
        max10 = _mm256_set1_epi32(1023);
    }
};

// (m0*y + m1*cb + m2*cr + m3 + 32768) >> 16 with an arithmetic shift, as roundShift.
__m256i applyRow(Lanes const& k, int row, __m256i y, __m256i cb, __m256i cr)
{
    __m256i acc = _mm256_add_epi32(_mm256_mullo_epi32(y, k.m[row][0]), _mm256_mullo_epi32(cb, k.m[row][1]));
    acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(cr, k.m[row][2]));
    return _mm256_srai_epi32(_mm256_add_epi32(acc, k.bias[row]), 16);
}

// Clamp to the clip limits (lanes outside are flagged in `clipped`), then to 10 bits.
__m256i clampRow(Lanes const& k, int row, __m256i v, __m256i& clipped)
{
    clipped = _mm256_or_si256(_mm256_cmpgt_epi32(k.lo[row], v), _mm256_cmpgt_epi32(v, k.hi[row]));
    v = _mm256_min_epi32(_mm256_max_epi32(v, k.lo[row]), k.hi[row]);
    return _mm256_min_epi32(_mm256_max_epi32(v, k.zero), k.max10);
}

int countLanes(__m256i flags)
{
    return __builtin_popcount(static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(flags))));
}

// Eight groups (48 pixels, 128 bytes). After the transpose each register holds
// one word position of the eight groups, so every sample is a shift and a mask.
// The group order inside the registers is permuted, the same way on the way
// back, so it does not matter.
void processBlock(std::uint8_t const* src, std::uint8_t* dst, Lanes const& k, ProcessStats* stats)
{
    __m256i w0 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src));
    __m256i w1 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 32));
    __m256i w2 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 64));
    __m256i w3 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 96));
    transpose(w0, w1, w2, w3);
    auto field = [&](__m256i w, int shift) { return _mm256_and_si256(_mm256_srli_epi32(w, shift), k.mask); };
    // v210 words: Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5.
    __m256i const y[6] = {field(w0, 10), field(w1, 0), field(w1, 20), field(w2, 10), field(w3, 0), field(w3, 20)};
    __m256i const cb[3] = {field(w0, 0), field(w1, 10), field(w2, 20)};
    __m256i const cr[3] = {field(w0, 20), field(w2, 0), field(w3, 10)};

    __m256i oy[6], oc[3], orr[3];
    __m256i clipY[6], clipC[3];
    for (int i = 0; i < 6; ++i)
    {
        oy[i] = clampRow(k, 0, applyRow(k, 0, y[i], cb[i / 2], cr[i / 2]), clipY[i]);
    }
    for (int c = 0; c < 3; ++c)
    {
        // Output chroma comes from the co-sited (even) sample.
        __m256i clipCb, clipCr;
        oc[c] = clampRow(k, 1, applyRow(k, 1, y[2 * c], cb[c], cr[c]), clipCb);
        orr[c] = clampRow(k, 2, applyRow(k, 2, y[2 * c], cb[c], cr[c]), clipCr);
        clipC[c] = _mm256_or_si256(clipCb, clipCr);
    }
    if (stats)
    {
        int clipped = 0;
        for (int i = 0; i < 6; ++i)
        {
            clipped += countLanes((i % 2) == 0 ? _mm256_or_si256(clipY[i], clipC[i / 2]) : clipY[i]);
        }
        stats->samples += 96;
        stats->clipped += static_cast<std::uint64_t>(clipped);
    }

    auto word = [](__m256i a, __m256i b, __m256i c) {
        return _mm256_or_si256(_mm256_or_si256(a, _mm256_slli_epi32(b, 10)), _mm256_slli_epi32(c, 20));
    };
    w0 = word(oc[0], oy[0], orr[0]);
    w1 = word(oy[1], oc[1], oy[2]);
    w2 = word(orr[1], oy[3], oc[2]);
    w3 = word(oy[4], orr[2], oy[5]);
    transpose(w0, w1, w2, w3);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), w0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 32), w1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 64), w2);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 96), w3);
}

void processLineAvx2(std::uint8_t const* src, std::uint8_t* dst, int width, int stride, FixedMatrix const& matrix, Lanes const& lanes, bool wide,
    ProcessStats* stats)
{
    if (matrix.bypass || matrix.identity)
    {
        processV210Scalar(src, dst, width, 1, stride, stride, 0, 1, matrix, stats);
        return;
    }
    int const groups = v210Groups(width);
    // Blocks only where all 48 pixels are inside the line.
    int const blocks = wide ? (width / 6) / 8 : 0;
    for (int b = 0; b < blocks; ++b)
    {
        processBlock(src + static_cast<std::size_t>(b) * 128u, dst + static_cast<std::size_t>(b) * 128u, lanes, stats);
    }
    for (int g = blocks * 8; g < groups; ++g)
    {
        processGroupDot8(src, dst, g, width, matrix, stats);
    }
    std::size_t const written = static_cast<std::size_t>(groups) * 16u;
    if (static_cast<std::size_t>(stride) > written)
    {
        std::memset(dst + written, 0, static_cast<std::size_t>(stride) - written);
    }
}
#endif
} // namespace

void processV210Avx2(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats)
{
#if defined(__x86_64__) || defined(_M_X64)
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
    Lanes const lanes(matrix);
    bool const wide = fits32(matrix);
    for (int row = rowBegin; row < rowEnd; ++row)
    {
        processLineAvx2(src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcStride),
            dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstStride), width, dstStride, matrix, lanes, wide, stats);
    }
#else
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#endif
}

} // namespace cc
