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
// By function attribute, not by a file flag (see process_avx2.cpp). Called only after
// cpuHasAvx512(), which checks F, VL, BW, DQ and the OS's opmask and zmm state.
#define CC_KERNEL __attribute__((target("avx2,avx512f,avx512vl,avx512bw,avx512dq,popcnt")))

// AVX-512VL: AVX-512 instructions on 256-bit registers. Compares give mask registers, so the
// statistics are a popcount; blends are masked moves; the pack is one ternary logic instruction.
// On Skylake-SP 256-bit code keeps the AVX2 frequency licence.
struct Ymm
{
    using I = __m256i;
    using M = __mmask8;
    using D = __m256d;
    using DM = __mmask8; // 4 lanes used
    using Tally = std::uint64_t;
    static constexpr int kGroups = 8;

    CC_KERNEL static I load(std::uint8_t const* p, int i) { return _mm256_loadu_si256(reinterpret_cast<__m256i const*>(p) + i); }
    CC_KERNEL static void store(std::uint8_t* p, int i, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p) + i, v); }
    CC_KERNEL static I set1(int v) { return _mm256_set1_epi32(v); }
    CC_KERNEL static I add(I a, I b) { return _mm256_add_epi32(a, b); }
    CC_KERNEL static I sub(I a, I b) { return _mm256_sub_epi32(a, b); }
    CC_KERNEL static I mul(I a, I b) { return _mm256_mullo_epi32(a, b); }
    CC_KERNEL static I mul16(I a, I k) { return _mm256_madd_epi16(a, k); }
    CC_KERNEL static I shr16(I a) { return _mm256_srai_epi32(a, 16); }
    template <int n>
    CC_KERNEL static I srl(I a)
    {
        return _mm256_srli_epi32(a, n);
    }
    CC_KERNEL static I and_(I a, I b) { return _mm256_and_si256(a, b); }
    CC_KERNEL static I word(I a, I b, I c) { return _mm256_ternarylogic_epi32(a, _mm256_slli_epi32(b, 10), _mm256_slli_epi32(c, 20), 0xfe); }
    CC_KERNEL static I clamp(I v, I lo, I hi) { return _mm256_min_epi32(_mm256_max_epi32(v, lo), hi); }
    CC_KERNEL static M same(I a, I b) { return _mm256_cmpeq_epi32_mask(a, b); }
    CC_KERNEL static M inside(I v, I lo, I hi) { return _mm256_mask_cmple_epi32_mask(_mm256_cmpge_epi32_mask(v, lo), v, hi); }
    CC_KERNEL static M outside(I v, I lo, I hi) { return static_cast<M>(_mm256_cmplt_epi32_mask(v, lo) | _mm256_cmpgt_epi32_mask(v, hi)); }
    CC_KERNEL static M both(M a, M b) { return static_cast<M>(a & b); }
    CC_KERNEL static M either(M a, M b) { return static_cast<M>(a | b); }
    CC_KERNEL static M butNot(M a, M b) { return static_cast<M>(a & ~b); }
    CC_KERNEL static M none() { return 0; }
    CC_KERNEL static bool any(M m) { return m != 0; }
    CC_KERNEL static Tally tally0() { return 0; }
    CC_KERNEL static void count(Tally& t, M m) { t += static_cast<std::uint64_t>(__builtin_popcount(m)); }
    CC_KERNEL static std::uint64_t total(Tally t) { return t; }
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
    CC_KERNEL static M joinMask(DM a, DM b) { return static_cast<M>(a | (b << 4)); }
    CC_KERNEL static D dset(double v) { return _mm256_set1_pd(v); }
    CC_KERNEL static D dadd(D a, D b) { return _mm256_add_pd(a, b); }
    CC_KERNEL static D dsub(D a, D b) { return _mm256_sub_pd(a, b); }
    CC_KERNEL static D dmul(D a, D b) { return _mm256_mul_pd(a, b); }
    CC_KERNEL static DM lt(D a, D b) { return _mm256_cmp_pd_mask(a, b, _CMP_LT_OQ); }
    CC_KERNEL static DM gt(D a, D b) { return _mm256_cmp_pd_mask(a, b, _CMP_GT_OQ); }
    CC_KERNEL static DM eq(D a, D b) { return _mm256_cmp_pd_mask(a, b, _CMP_EQ_OQ); }
    CC_KERNEL static DM dboth(DM a, DM b) { return static_cast<DM>(a & b); }
    CC_KERNEL static DM deither(DM a, DM b) { return static_cast<DM>(a | b); }
    CC_KERNEL static bool dany(DM m) { return m != 0; }
    // m ? b : a
    CC_KERNEL static D pick(D a, D b, DM m) { return _mm256_mask_blend_pd(m, a, b); }
    CC_KERNEL static D roundEven(D x) { return _mm256_roundscale_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    CC_KERNEL static D inc(D x, DM m, D one) { return _mm256_mask_add_pd(x, m, x, one); }
    CC_KERNEL static D dec(D x, DM m, D one) { return _mm256_mask_sub_pd(x, m, x, one); }
};

// AVX-512 on 512-bit registers: 16 groups (96 pixels) per block, 8 doubles per register.
struct Zmm
{
    using I = __m512i;
    using M = __mmask16;
    using D = __m512d;
    using DM = __mmask8;
    using Tally = std::uint64_t;
    static constexpr int kGroups = 16;

    CC_KERNEL static I load(std::uint8_t const* p, int i) { return _mm512_loadu_si512(p + 64 * i); }
    CC_KERNEL static void store(std::uint8_t* p, int i, I v) { _mm512_storeu_si512(p + 64 * i, v); }
    CC_KERNEL static I set1(int v) { return _mm512_set1_epi32(v); }
    CC_KERNEL static I add(I a, I b) { return _mm512_add_epi32(a, b); }
    CC_KERNEL static I sub(I a, I b) { return _mm512_sub_epi32(a, b); }
    CC_KERNEL static I mul(I a, I b) { return _mm512_mullo_epi32(a, b); }
    CC_KERNEL static I mul16(I a, I k) { return _mm512_madd_epi16(a, k); }
    CC_KERNEL static I shr16(I a) { return _mm512_srai_epi32(a, 16); }
    template <int n>
    CC_KERNEL static I srl(I a)
    {
        return _mm512_srli_epi32(a, n);
    }
    CC_KERNEL static I and_(I a, I b) { return _mm512_and_si512(a, b); }
    CC_KERNEL static I word(I a, I b, I c) { return _mm512_ternarylogic_epi32(a, _mm512_slli_epi32(b, 10), _mm512_slli_epi32(c, 20), 0xfe); }
    CC_KERNEL static I clamp(I v, I lo, I hi) { return _mm512_min_epi32(_mm512_max_epi32(v, lo), hi); }
    CC_KERNEL static M same(I a, I b) { return _mm512_cmpeq_epi32_mask(a, b); }
    CC_KERNEL static M inside(I v, I lo, I hi) { return _mm512_mask_cmple_epi32_mask(_mm512_cmpge_epi32_mask(v, lo), v, hi); }
    CC_KERNEL static M outside(I v, I lo, I hi) { return static_cast<M>(_mm512_cmplt_epi32_mask(v, lo) | _mm512_cmpgt_epi32_mask(v, hi)); }
    CC_KERNEL static M both(M a, M b) { return static_cast<M>(a & b); }
    CC_KERNEL static M either(M a, M b) { return static_cast<M>(a | b); }
    CC_KERNEL static M butNot(M a, M b) { return static_cast<M>(a & ~b); }
    CC_KERNEL static M none() { return 0; }
    CC_KERNEL static bool any(M m) { return m != 0; }
    CC_KERNEL static Tally tally0() { return 0; }
    CC_KERNEL static void count(Tally& t, M m) { t += static_cast<std::uint64_t>(__builtin_popcount(m)); }
    CC_KERNEL static std::uint64_t total(Tally t) { return t; }
    // The same in-lane transpose as on 256 bits, in four 128-bit lanes.
    CC_KERNEL static void transpose(I& a, I& b, I& c, I& d)
    {
        I const t0 = _mm512_unpacklo_epi32(a, b);
        I const t1 = _mm512_unpacklo_epi32(c, d);
        I const t2 = _mm512_unpackhi_epi32(a, b);
        I const t3 = _mm512_unpackhi_epi32(c, d);
        a = _mm512_unpacklo_epi64(t0, t1);
        b = _mm512_unpackhi_epi64(t0, t1);
        c = _mm512_unpacklo_epi64(t2, t3);
        d = _mm512_unpackhi_epi64(t2, t3);
    }

    CC_KERNEL static D half0(I v) { return _mm512_cvtepi32_pd(_mm512_castsi512_si256(v)); }
    CC_KERNEL static D half1(I v) { return _mm512_cvtepi32_pd(_mm512_extracti64x4_epi64(v, 1)); }
    CC_KERNEL static I join(D a, D b) { return _mm512_inserti64x4(_mm512_castsi256_si512(_mm512_cvtpd_epi32(a)), _mm512_cvtpd_epi32(b), 1); }
    CC_KERNEL static M joinMask(DM a, DM b) { return static_cast<M>(a | (b << 8)); }
    CC_KERNEL static D dset(double v) { return _mm512_set1_pd(v); }
    CC_KERNEL static D dadd(D a, D b) { return _mm512_add_pd(a, b); }
    CC_KERNEL static D dsub(D a, D b) { return _mm512_sub_pd(a, b); }
    CC_KERNEL static D dmul(D a, D b) { return _mm512_mul_pd(a, b); }
    CC_KERNEL static DM lt(D a, D b) { return _mm512_cmp_pd_mask(a, b, _CMP_LT_OQ); }
    CC_KERNEL static DM gt(D a, D b) { return _mm512_cmp_pd_mask(a, b, _CMP_GT_OQ); }
    CC_KERNEL static DM eq(D a, D b) { return _mm512_cmp_pd_mask(a, b, _CMP_EQ_OQ); }
    CC_KERNEL static DM dboth(DM a, DM b) { return static_cast<DM>(a & b); }
    CC_KERNEL static DM deither(DM a, DM b) { return static_cast<DM>(a | b); }
    CC_KERNEL static bool dany(DM m) { return m != 0; }
    // m ? b : a
    CC_KERNEL static D pick(D a, D b, DM m) { return _mm512_mask_blend_pd(m, a, b); }
    CC_KERNEL static D roundEven(D x) { return _mm512_roundscale_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    CC_KERNEL static D inc(D x, DM m, D one) { return _mm512_mask_add_pd(x, m, x, one); }
    CC_KERNEL static D dec(D x, DM m, D one) { return _mm512_mask_sub_pd(x, m, x, one); }
};

#include "color/process_kernel.inl"

#undef CC_KERNEL
#endif
} // namespace

void processV210Avx512Vl(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats)
{
#if defined(__x86_64__) || defined(_M_X64)
    processRows<Ymm, Ymm>(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#else
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#endif
}

// 16-group blocks, then one 8-group block (AVX-512VL) when 8 to 15 whole groups are left.
void processV210Avx512(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats)
{
#if defined(__x86_64__) || defined(_M_X64)
    processRows<Zmm, Ymm>(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#else
    processV210Scalar(src, dst, width, height, srcStride, dstStride, rowBegin, rowEnd, matrix, stats);
#endif
}

} // namespace cc
