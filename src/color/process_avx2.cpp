#include "color/process.hpp"

#include "v210.hpp"

#include <algorithm>
#include <cstring>

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

void processLineAvx2(std::uint8_t const* src, std::uint8_t* dst, int width, int stride, FixedMatrix const& matrix, ProcessStats* stats)
{
    if (matrix.bypass || matrix.identity)
    {
        processV210Scalar(src, dst, width, 1, stride, stride, 0, 1, matrix, stats);
        return;
    }
    std::memset(dst, 0, static_cast<std::size_t>(stride));
    // Each v210 group is 6 pixels. The dot product runs in AVX2 across those
    // lanes (2 lanes padded). Chroma is co-sited with the even luma sample.
    int const groups = v210Groups(width);
    for (int g = 0; g < groups; ++g)
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
    for (int row = rowBegin; row < rowEnd; ++row)
    {
        processLineAvx2(src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcStride),
            dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstStride), width, dstStride, matrix, stats);
    }
#else
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#endif
}

} // namespace cc
