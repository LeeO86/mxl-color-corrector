#include "color/process.hpp"

#include "color/bt709.hpp"

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
        // As the scalar reference: a pixel counts when its Y or its own chroma
        // (computed with its Y, also for the odd samples whose chroma is not
        // written) leaves the clip limits.
        int clipped = 0;
        for (int i = 0; i < 6; ++i)
        {
            __m256i flags = _mm256_or_si256(clipY[i], clipC[i / 2]);
            if ((i % 2) != 0)
            {
                __m256i clipCb, clipCr;
                clampRow(k, 1, applyRow(k, 1, y[i], cb[i / 2], cr[i / 2]), clipCb);
                clampRow(k, 2, applyRow(k, 2, y[i], cb[i / 2], cr[i / 2]), clipCr);
                flags = _mm256_or_si256(clipY[i], _mm256_or_si256(clipCb, clipCr));
            }
            clipped += countLanes(flags);
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

// llround (halves away from zero), exactly: round to nearest even, then move the
// exact ties that went towards zero.
__m256d roundAway(__m256d x)
{
    __m256d const r = _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256d const d = _mm256_sub_pd(x, r);
    __m256d const zero = _mm256_setzero_pd();
    __m256d const one = _mm256_set1_pd(1.0);
    __m256d const up = _mm256_and_pd(_mm256_and_pd(_mm256_cmp_pd(d, _mm256_set1_pd(0.5), _CMP_EQ_OQ), _mm256_cmp_pd(x, zero, _CMP_GT_OQ)), one);
    __m256d const down = _mm256_and_pd(_mm256_and_pd(_mm256_cmp_pd(d, _mm256_set1_pd(-0.5), _CMP_EQ_OQ), _mm256_cmp_pd(x, zero, _CMP_LT_OQ)), one);
    return _mm256_sub_pd(_mm256_add_pd(r, up), down);
}

// std::clamp(v, 0.0, 1.0).
__m256d clamp01(__m256d v)
{
    __m256d const lo = _mm256_setzero_pd();
    __m256d const hi = _mm256_set1_pd(1.0);
    __m256d const r = _mm256_blendv_pd(v, hi, _mm256_cmp_pd(hi, v, _CMP_LT_OQ));
    return _mm256_blendv_pd(r, lo, _mm256_cmp_pd(v, lo, _CMP_LT_OQ));
}

// rgbClipCodes in process.cpp for four samples, with the same operations in the
// same order (double precision, no FMA), so the codes are identical. Returns the
// lanes that were pulled back into the RGB cube.
__m256d rgbClip4(__m256d& Y, __m256d& Cb, __m256d& Cr)
{
    using namespace bt709;
    auto c = [](double v) { return _mm256_set1_pd(v); };
    __m256d const y = _mm256_mul_pd(_mm256_sub_pd(Y, c(kYBlack)), c(kInvYSpan));
    __m256d const cb = _mm256_mul_pd(_mm256_sub_pd(Cb, c(kCZero)), c(kInvCSpan));
    __m256d const cr = _mm256_mul_pd(_mm256_sub_pd(Cr, c(kCZero)), c(kInvCSpan));
    __m256d const R = _mm256_add_pd(y, _mm256_mul_pd(c(kCrScale), cr));
    __m256d const G = _mm256_sub_pd(_mm256_sub_pd(y, _mm256_mul_pd(c(kCbToG), cb)), _mm256_mul_pd(c(kCrToG), cr));
    __m256d const B = _mm256_add_pd(y, _mm256_mul_pd(c(kCbScale), cb));
    constexpr double kEps = 1e-6;
    __m256d const lo = c(-kEps);
    __m256d const hi = c(1.0 + kEps);
    __m256d outside = _mm256_or_pd(_mm256_cmp_pd(R, lo, _CMP_LT_OQ), _mm256_cmp_pd(R, hi, _CMP_GT_OQ));
    outside = _mm256_or_pd(outside, _mm256_or_pd(_mm256_cmp_pd(G, lo, _CMP_LT_OQ), _mm256_cmp_pd(G, hi, _CMP_GT_OQ)));
    outside = _mm256_or_pd(outside, _mm256_or_pd(_mm256_cmp_pd(B, lo, _CMP_LT_OQ), _mm256_cmp_pd(B, hi, _CMP_GT_OQ)));
    if (_mm256_movemask_pd(outside) == 0)
    {
        return outside;
    }
    __m256d const Rc = clamp01(R);
    __m256d const Gc = clamp01(G);
    __m256d const Bc = clamp01(B);
    __m256d const yn = _mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(c(kKr), Rc), _mm256_mul_pd(c(kKg), Gc)), _mm256_mul_pd(c(kKb), Bc));
    __m256d const cbn = _mm256_mul_pd(_mm256_sub_pd(Bc, yn), c(kInvCbScale));
    __m256d const crn = _mm256_mul_pd(_mm256_sub_pd(Rc, yn), c(kInvCrScale));
    Y = _mm256_blendv_pd(Y, roundAway(_mm256_add_pd(_mm256_mul_pd(yn, c(kYSpan)), c(kYBlack))), outside);
    Cb = _mm256_blendv_pd(Cb, roundAway(_mm256_add_pd(_mm256_mul_pd(cbn, c(kCSpan)), c(kCZero))), outside);
    Cr = _mm256_blendv_pd(Cr, roundAway(_mm256_add_pd(_mm256_mul_pd(crn, c(kCSpan)), c(kCZero))), outside);
    return outside;
}

// True when all eight lanes are clearly inside the RGB cube, so rgbClip8 would leave them
// alone: codes in the legal ranges, and R, G, B from 20-bit fixed-point coefficients at least
// 2^-10 inside [0, 1]. Within those ranges the coefficients' rounding moves R, G and B by at
// most 0.5 · (876 + 448 + 448) · 2^-20, less than the margin.
bool clearlyInside(__m256i Y, __m256i Cb, __m256i Cr)
{
    using namespace bt709;
    constexpr double kOne = 1 << 20;
    constexpr int kY = static_cast<int>(kOne / kYSpan + 0.5);
    constexpr int kRcr = static_cast<int>(kOne * kCrScale / kCSpan + 0.5);
    constexpr int kGcb = static_cast<int>(kOne * kCbToG / kCSpan + 0.5);
    constexpr int kGcr = static_cast<int>(kOne * kCrToG / kCSpan + 0.5);
    constexpr int kBcb = static_cast<int>(kOne * kCbScale / kCSpan + 0.5);
    constexpr int kMargin = 1 << 10;
    auto const outside = [](__m256i v, int lo, int hi) {
        return _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(lo), v), _mm256_cmpgt_epi32(v, _mm256_set1_epi32(hi)));
    };
    __m256i const y = _mm256_sub_epi32(Y, _mm256_set1_epi32(64));
    __m256i const cb = _mm256_sub_epi32(Cb, _mm256_set1_epi32(512));
    __m256i const cr = _mm256_sub_epi32(Cr, _mm256_set1_epi32(512));
    __m256i const ly = _mm256_mullo_epi32(y, _mm256_set1_epi32(kY));
    __m256i const r = _mm256_add_epi32(ly, _mm256_mullo_epi32(cr, _mm256_set1_epi32(kRcr)));
    __m256i const g = _mm256_sub_epi32(_mm256_sub_epi32(ly, _mm256_mullo_epi32(cb, _mm256_set1_epi32(kGcb))), _mm256_mullo_epi32(cr, _mm256_set1_epi32(kGcr)));
    __m256i const b = _mm256_add_epi32(ly, _mm256_mullo_epi32(cb, _mm256_set1_epi32(kBcb)));
    __m256i bad = _mm256_or_si256(outside(y, 0, 876), _mm256_or_si256(outside(cb, -448, 448), outside(cr, -448, 448)));
    int const lo = kMargin;
    int const hi = (1 << 20) - kMargin;
    bad = _mm256_or_si256(bad, _mm256_or_si256(outside(r, lo, hi), _mm256_or_si256(outside(g, lo, hi), outside(b, lo, hi))));
    return _mm256_testz_si256(bad, bad) != 0;
}

// RGB gamut clip on eight lanes of codes; returns the pulled lanes as a mask.
__m256i rgbClip8(__m256i& Y, __m256i& Cb, __m256i& Cr)
{
    __m128i outY[2], outCb[2], outCr[2];
    int bits = 0;
    for (int h = 0; h < 2; ++h)
    {
        auto half = [h](__m256i v) { return h == 0 ? _mm256_castsi256_si128(v) : _mm256_extracti128_si256(v, 1); };
        __m256d dy = _mm256_cvtepi32_pd(half(Y));
        __m256d dcb = _mm256_cvtepi32_pd(half(Cb));
        __m256d dcr = _mm256_cvtepi32_pd(half(Cr));
        bits |= _mm256_movemask_pd(rgbClip4(dy, dcb, dcr)) << (4 * h);
        outY[h] = _mm256_cvtpd_epi32(dy);
        outCb[h] = _mm256_cvtpd_epi32(dcb);
        outCr[h] = _mm256_cvtpd_epi32(dcr);
    }
    Y = _mm256_set_m128i(outY[1], outY[0]);
    Cb = _mm256_set_m128i(outCb[1], outCb[0]);
    Cr = _mm256_set_m128i(outCr[1], outCr[0]);
    __m256i const laneBit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(bits), laneBit), laneBit);
}

// The block with RGB gamut clip: every pixel gets all three rows (with its own
// Y), the clip, then the clamps; chroma is written from the even samples.
void processBlockRgb(std::uint8_t const* src, std::uint8_t* dst, Lanes const& k, ProcessStats* stats)
{
    __m256i w0 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src));
    __m256i w1 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 32));
    __m256i w2 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 64));
    __m256i w3 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + 96));
    transpose(w0, w1, w2, w3);
    auto field = [&](__m256i w, int shift) { return _mm256_and_si256(_mm256_srli_epi32(w, shift), k.mask); };
    __m256i const y[6] = {field(w0, 10), field(w1, 0), field(w1, 20), field(w2, 10), field(w3, 0), field(w3, 20)};
    __m256i const cb[3] = {field(w0, 0), field(w1, 10), field(w2, 20)};
    __m256i const cr[3] = {field(w0, 20), field(w2, 0), field(w3, 10)};

    __m256i oy[6], oc[6], orr[6];
    int clipped = 0;
    for (int i = 0; i < 6; ++i)
    {
        __m256i outY = applyRow(k, 0, y[i], cb[i / 2], cr[i / 2]);
        __m256i outCb = applyRow(k, 1, y[i], cb[i / 2], cr[i / 2]);
        __m256i outCr = applyRow(k, 2, y[i], cb[i / 2], cr[i / 2]);
        __m256i const pulled = clearlyInside(outY, outCb, outCr) ? _mm256_setzero_si256() : rgbClip8(outY, outCb, outCr);
        __m256i clipY, clipCb, clipCr;
        oy[i] = clampRow(k, 0, outY, clipY);
        oc[i] = clampRow(k, 1, outCb, clipCb);
        orr[i] = clampRow(k, 2, outCr, clipCr);
        if (stats)
        {
            clipped += countLanes(_mm256_or_si256(_mm256_or_si256(pulled, clipY), _mm256_or_si256(clipCb, clipCr)));
        }
    }
    if (stats)
    {
        stats->samples += 96;
        stats->clipped += static_cast<std::uint64_t>(clipped);
    }
    auto word = [](__m256i a, __m256i b, __m256i c) {
        return _mm256_or_si256(_mm256_or_si256(a, _mm256_slli_epi32(b, 10)), _mm256_slli_epi32(c, 20));
    };
    w0 = word(oc[0], oy[0], orr[0]);
    w1 = word(oy[1], oc[2], oy[2]);
    w2 = word(orr[2], oy[3], oc[4]);
    w3 = word(oy[4], orr[4], oy[5]);
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
        std::size_t const offset = static_cast<std::size_t>(b) * 128u;
        if (matrix.rgbClip)
        {
            processBlockRgb(src + offset, dst + offset, lanes, stats);
        }
        else
        {
            processBlock(src + offset, dst + offset, lanes, stats);
        }
    }
    for (int g = blocks * 8; g < groups; ++g)
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
