#include "color/process.hpp"

#include "color/bt709.hpp"

#include "v210.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace cc
{
namespace
{
#if defined(__x86_64__) || defined(_M_X64)
// AVX2 by function attribute, not by a file flag: the inline functions of the headers this file
// uses stay plain x86-64 code, so the linker cannot hand another file an AVX2 copy.
#define CC_KERNEL __attribute__((target("avx2")))

CC_KERNEL __m256i srai64_16(__m256i v)
{
    __m256i const shifted = _mm256_srli_epi64(v, 16);
    __m256i const sign = _mm256_cmpgt_epi64(_mm256_setzero_si256(), v);
    __m256i const fill = _mm256_slli_epi64(sign, 48);
    return _mm256_or_si256(shifted, fill);
}

// A function, not a lambda: GCC does not give a lambda its enclosing function's target.
CC_KERNEL void mulAdd(__m256i a, __m256i b, __m256i& even, __m256i& odd)
{
    __m256i const aOdd = _mm256_srli_si256(a, 4);
    __m256i const bOdd = _mm256_srli_si256(b, 4);
    even = _mm256_add_epi64(even, _mm256_mul_epi32(a, b));
    odd = _mm256_add_epi64(odd, _mm256_mul_epi32(aOdd, bOdd));
}

CC_KERNEL void dot8(std::int32_t const* y, std::int32_t const* cb, std::int32_t const* cr, std::int32_t m0, std::int32_t m1, std::int32_t m2, std::int32_t m3,
    std::int32_t* out)
{
    __m256i const vy = _mm256_load_si256(reinterpret_cast<__m256i const*>(y));
    __m256i const vcb = _mm256_load_si256(reinterpret_cast<__m256i const*>(cb));
    __m256i const vcr = _mm256_load_si256(reinterpret_cast<__m256i const*>(cr));
    __m256i const c0 = _mm256_set1_epi32(m0);
    __m256i const c1 = _mm256_set1_epi32(m1);
    __m256i const c2 = _mm256_set1_epi32(m2);

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
CC_KERNEL void processGroupDot8(std::uint8_t const* src, std::uint8_t* dst, int g, int width, FixedMatrix const& matrix, ProcessStats* stats)
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
        // Every sample's chroma counts for the statistics (as in the scalar
        // reference); only the co-sited (even) one is written.
        int cc = clampTo(oC[i], matrix.cMin, matrix.cMax, clipped);
        int rr = clampTo(oR[i], matrix.cMin, matrix.cMax, clipped);
        if ((i % 2) == 0)
        {
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

// One group exactly as the scalar reference (correctSample per pixel): the tail
// of a line and matrices beyond 32-bit sums when RGB gamut clip is on.
void processGroupScalar(std::uint8_t const* src, std::uint8_t* dst, int g, int width, FixedMatrix const& matrix, ProcessStats* stats)
{
    std::uint32_t words[4] = {};
    std::memcpy(words, src + static_cast<std::size_t>(g) * 16u, sizeof(words));
    std::uint16_t ys[6], cbs[3], crs[3];
    unpackV210Group(words, ys, cbs, crs);
    std::uint16_t oy[6] = {};
    std::uint16_t oc[3] = {};
    std::uint16_t orr[3] = {};
    int x = g * 6;
    for (int i = 0; i < 6 && x < width; ++i, ++x)
    {
        bool clipped = false;
        int outY = 0, outCb = 0, outCr = 0;
        correctSample(matrix, ys[i], cbs[i / 2], crs[i / 2], outY, outCb, outCr, clipped);
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


// AVX2: 8 lanes of 32 bits, a lane mask is all ones, the double halves are 4 lanes.
struct Avx2
{
    using I = __m256i;
    using M = __m256i;
    using D = __m256d;
    using DM = __m256d;
    using Tally = __m256i; // per lane, minus the count (a set mask lane is -1)
    static constexpr int kGroups = 8;

    CC_KERNEL static I load(std::uint8_t const* p, int i) { return _mm256_loadu_si256(reinterpret_cast<__m256i const*>(p) + i); }
    CC_KERNEL static void store(std::uint8_t* p, int i, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p) + i, v); }
    CC_KERNEL static I set1(int v) { return _mm256_set1_epi32(v); }
    CC_KERNEL static I add(I a, I b) { return _mm256_add_epi32(a, b); }
    CC_KERNEL static I sub(I a, I b) { return _mm256_sub_epi32(a, b); }
    CC_KERNEL static I mul(I a, I b) { return _mm256_mullo_epi32(a, b); }
    // a · k per 32-bit lane for a in int16 and 0 <= k < 2^15 (the high half of k is 0).
    CC_KERNEL static I mul16(I a, I k) { return _mm256_madd_epi16(a, k); }
    CC_KERNEL static I shr16(I a) { return _mm256_srai_epi32(a, 16); }
    template <int n>
    CC_KERNEL static I srl(I a)
    {
        return _mm256_srli_epi32(a, n);
    }
    CC_KERNEL static I and_(I a, I b) { return _mm256_and_si256(a, b); }
    CC_KERNEL static I word(I a, I b, I c) { return _mm256_or_si256(_mm256_or_si256(a, _mm256_slli_epi32(b, 10)), _mm256_slli_epi32(c, 20)); }
    CC_KERNEL static I clamp(I v, I lo, I hi) { return _mm256_min_epi32(_mm256_max_epi32(v, lo), hi); }
    CC_KERNEL static M same(I a, I b) { return _mm256_cmpeq_epi32(a, b); }
    CC_KERNEL static M inside(I v, I lo, I hi) { return same(clamp(v, lo, hi), v); }
    CC_KERNEL static M outside(I v, I lo, I hi) { return _mm256_or_si256(_mm256_cmpgt_epi32(lo, v), _mm256_cmpgt_epi32(v, hi)); }
    CC_KERNEL static M both(M a, M b) { return _mm256_and_si256(a, b); }
    CC_KERNEL static M either(M a, M b) { return _mm256_or_si256(a, b); }
    CC_KERNEL static M butNot(M a, M b) { return _mm256_andnot_si256(b, a); }
    CC_KERNEL static M none() { return _mm256_setzero_si256(); }
    CC_KERNEL static bool any(M m) { return _mm256_testz_si256(m, m) == 0; }
    CC_KERNEL static Tally tally0() { return _mm256_setzero_si256(); }
    CC_KERNEL static void count(Tally& t, M m) { t = _mm256_sub_epi32(t, m); }
    CC_KERNEL static std::uint64_t total(Tally t)
    {
        __m128i v = _mm_add_epi32(_mm256_castsi256_si128(t), _mm256_extracti128_si256(t, 1));
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
        return static_cast<std::uint32_t>(_mm_cvtsi128_si32(v));
    }
    // 4×4 transpose of 32-bit words within each 128-bit lane. Its own inverse.
    CC_KERNEL static void transpose(I& a, I& b, I& c, I& d)
    {
        I const t0 = _mm256_unpacklo_epi32(a, b);
        I const t1 = _mm256_unpacklo_epi32(c, d);
        I const t2 = _mm256_unpackhi_epi32(a, b);
        I const t3 = _mm256_unpackhi_epi32(c, d);
        a = _mm256_unpacklo_epi64(t0, t1);
        b = _mm256_unpackhi_epi64(t0, t1);
        c = _mm256_unpacklo_epi64(t2, t3);
        d = _mm256_unpackhi_epi64(t2, t3);
    }

    CC_KERNEL static D half0(I v) { return _mm256_cvtepi32_pd(_mm256_castsi256_si128(v)); }
    CC_KERNEL static D half1(I v) { return _mm256_cvtepi32_pd(_mm256_extracti128_si256(v, 1)); }
    CC_KERNEL static I join(D a, D b) { return _mm256_set_m128i(_mm256_cvtpd_epi32(b), _mm256_cvtpd_epi32(a)); }
    CC_KERNEL static M joinMask(DM a, DM b)
    {
        int const bits = _mm256_movemask_pd(a) | (_mm256_movemask_pd(b) << 4);
        __m256i const laneBit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
        return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(bits), laneBit), laneBit);
    }
    CC_KERNEL static D dset(double v) { return _mm256_set1_pd(v); }
    CC_KERNEL static D dadd(D a, D b) { return _mm256_add_pd(a, b); }
    CC_KERNEL static D dsub(D a, D b) { return _mm256_sub_pd(a, b); }
    CC_KERNEL static D dmul(D a, D b) { return _mm256_mul_pd(a, b); }
    CC_KERNEL static DM lt(D a, D b) { return _mm256_cmp_pd(a, b, _CMP_LT_OQ); }
    CC_KERNEL static DM gt(D a, D b) { return _mm256_cmp_pd(a, b, _CMP_GT_OQ); }
    CC_KERNEL static DM eq(D a, D b) { return _mm256_cmp_pd(a, b, _CMP_EQ_OQ); }
    CC_KERNEL static DM dboth(DM a, DM b) { return _mm256_and_pd(a, b); }
    CC_KERNEL static DM deither(DM a, DM b) { return _mm256_or_pd(a, b); }
    CC_KERNEL static bool dany(DM m) { return _mm256_movemask_pd(m) != 0; }
    // m ? b : a
    CC_KERNEL static D pick(D a, D b, DM m) { return _mm256_blendv_pd(a, b, m); }
    CC_KERNEL static D roundEven(D x) { return _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    CC_KERNEL static D inc(D x, DM m, D one) { return _mm256_add_pd(x, _mm256_and_pd(m, one)); }
    CC_KERNEL static D dec(D x, DM m, D one) { return _mm256_sub_pd(x, _mm256_and_pd(m, one)); }
};

#include "color/process_kernel.inl"

#undef CC_KERNEL
#endif
} // namespace

#if defined(__x86_64__) || defined(_M_X64)
void detail::processGroupsAvx2(std::uint8_t const* src, std::uint8_t* dst, int gBegin, int gEnd, int width, FixedMatrix const& matrix,
    ProcessStats* stats)
{
    for (int g = gBegin; g < gEnd; ++g)
    {
        if (matrix.rgbClip)
        {
            processGroupScalar(src, dst, g, width, matrix, stats);
        }
        else
        {
            processGroupDot8(src, dst, g, width, matrix, stats);
        }
    }
}
#endif

void processV210Avx2(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin, int rowEnd,
    FixedMatrix const& matrix, ProcessStats* stats)
{
#if defined(__x86_64__) || defined(_M_X64)
    processRows<Avx2, Avx2>(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#else
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#endif
}

} // namespace cc
