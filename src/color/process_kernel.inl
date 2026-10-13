// The block kernel of every SIMD level, written once over a traits type V: Avx2 in
// process_avx2.cpp, Ymm (AVX-512VL) and Zmm (AVX-512) in process_avx512.cpp. Each of those files
// includes this one inside an anonymous namespace, after its headers, CC_KERNEL (its target
// attribute) and its traits. No includes here.
//
// A block is V::kGroups v210 groups (6 pixels, 4 words each). After the transpose each register
// holds one word position of the block's groups, so every sample is a shift and a mask. The group
// order inside the registers is permuted, the same way on the way back, so it does not matter.
//
// Bit-exact with the scalar reference (correctSample):
// - the rows are the same 32-bit sums (fits32), only added in another order: the pair's shared part
//   m1·Cb + m2·Cr + m3 first, then m0·Y per sample (wrapping sums do not depend on the order);
// - the gamut clip repeats the double-precision operations in the same order (no FMA);
// - a pixel counts as clipped when its Y or its own chroma (computed with its Y, also for the odd
//   samples whose chroma is not written) leaves the clip limits, or the gamut clip moved it.
//   The statistics are counted as "unchanged" lanes and subtracted once per line.

// The per-block steps are inlined into the block loop (GCC leaves the pair functions out of line).
#define CC_INLINE CC_KERNEL __attribute__((always_inline))

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

// Named members, not arrays: GCC 13 keeps arrays of vector registers on the stack.
template <class V>
struct Coeffs
{
    typename V::I m00, m01, m02, m10, m11, m12, m20, m21, m22;
    typename V::I b0, b1, b2; // m3 + 32768 (rounding)
    typename V::I yLo, yHi, cLo, cHi;
    typename V::I mask10;
};

std::int32_t withRounding(std::int32_t m3)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(m3) + 32768u);
}

template <class V>
CC_KERNEL Coeffs<V> coeffs(FixedMatrix const& matrix)
{
    Coeffs<V> k;
    k.m00 = V::set1(matrix.m[0][0]);
    k.m01 = V::set1(matrix.m[0][1]);
    k.m02 = V::set1(matrix.m[0][2]);
    k.m10 = V::set1(matrix.m[1][0]);
    k.m11 = V::set1(matrix.m[1][1]);
    k.m12 = V::set1(matrix.m[1][2]);
    k.m20 = V::set1(matrix.m[2][0]);
    k.m21 = V::set1(matrix.m[2][1]);
    k.m22 = V::set1(matrix.m[2][2]);
    k.b0 = V::set1(withRounding(matrix.m[0][3]));
    k.b1 = V::set1(withRounding(matrix.m[1][3]));
    k.b2 = V::set1(withRounding(matrix.m[2][3]));
    // The clip limits lie inside 0..1023 (clipLimits), so one clamp does both clamps of the scalar
    // code, and a lane is clipped exactly when the clamp changes it.
    k.yLo = V::set1(std::max(matrix.yMin, 0));
    k.yHi = V::set1(std::min(matrix.yMax, 1023));
    k.cLo = V::set1(std::max(matrix.cMin, 0));
    k.cHi = V::set1(std::min(matrix.cMax, 1023));
    k.mask10 = V::set1(0x3ff);
    return k;
}

// One chroma pair's results: the even (co-sited) and the odd luma, and the even sample's chroma.
template <class V>
struct Pair
{
    typename V::I ye, yo, cb, cr;
};

// The part of each row that both samples of a pair share: m1·Cb + m2·Cr + m3 + 32768.
template <class V>
struct Shared
{
    typename V::I y, cb, cr;
};

template <class V>
CC_INLINE Shared<V> shared(Coeffs<V> const& k, typename V::I cb, typename V::I cr)
{
    return {V::add(V::add(V::mul(cb, k.m01), V::mul(cr, k.m02)), k.b0), V::add(V::add(V::mul(cb, k.m11), V::mul(cr, k.m12)), k.b1),
        V::add(V::add(V::mul(cb, k.m21), V::mul(cr, k.m22)), k.b2)};
}

// (shared + m0·Y) >> 16 with an arithmetic shift, as roundShift.
template <class V>
CC_INLINE typename V::I rowValue(typename V::I part, typename V::I y, typename V::I m0)
{
    return V::shr16(V::add(part, V::mul(y, m0)));
}

template <class V, bool kStats>
CC_INLINE void matrixPair(Coeffs<V> const& k, typename V::I ye, typename V::I yo, typename V::I cb, typename V::I cr, Pair<V>& out,
    typename V::Tally& unchanged)
{
    Shared<V> const s = shared<V>(k, cb, cr);
    typename V::I const y0 = rowValue<V>(s.y, ye, k.m00);
    typename V::I const y1 = rowValue<V>(s.y, yo, k.m00);
    typename V::I const c0 = rowValue<V>(s.cb, ye, k.m10);
    typename V::I const r0 = rowValue<V>(s.cr, ye, k.m20);
    out.ye = V::clamp(y0, k.yLo, k.yHi);
    out.yo = V::clamp(y1, k.yLo, k.yHi);
    out.cb = V::clamp(c0, k.cLo, k.cHi);
    out.cr = V::clamp(r0, k.cLo, k.cHi);
    if constexpr (kStats)
    {
        // The odd sample's chroma is only needed for the statistics.
        typename V::I const c1 = rowValue<V>(s.cb, yo, k.m10);
        typename V::I const r1 = rowValue<V>(s.cr, yo, k.m20);
        V::count(unchanged, V::both(V::same(out.ye, y0), V::both(V::same(out.cb, c0), V::same(out.cr, r0))));
        V::count(unchanged, V::both(V::same(out.yo, y1), V::both(V::inside(c1, k.cLo, k.cHi), V::inside(r1, k.cLo, k.cHi))));
    }
}

// std::clamp(v, 0.0, 1.0).
template <class V>
CC_KERNEL typename V::D clamp01(typename V::D v)
{
    typename V::D const lo = V::dset(0.0);
    typename V::D const hi = V::dset(1.0);
    typename V::D const r = V::pick(v, hi, V::lt(hi, v));
    return V::pick(r, lo, V::lt(v, lo));
}

// llround (halves away from zero), exactly: round to nearest even, then move the exact ties that
// went towards zero.
template <class V>
CC_KERNEL typename V::D roundAway(typename V::D x)
{
    typename V::D const r = V::roundEven(x);
    typename V::D const d = V::dsub(x, r);
    typename V::D const zero = V::dset(0.0);
    typename V::D const one = V::dset(1.0);
    typename V::DM const up = V::dboth(V::eq(d, V::dset(0.5)), V::gt(x, zero));
    typename V::DM const down = V::dboth(V::eq(d, V::dset(-0.5)), V::lt(x, zero));
    return V::dec(V::inc(r, up, one), down, one);
}

// rgbClipCodes in process.cpp on the double lanes, with the same operations in the same order.
// Returns the lanes that were pulled back into the RGB cube.
template <class V>
CC_KERNEL typename V::DM rgbClipD(typename V::D& Y, typename V::D& Cb, typename V::D& Cr)
{
    using namespace bt709;
    typename V::D const y = V::dmul(V::dsub(Y, V::dset(kYBlack)), V::dset(kInvYSpan));
    typename V::D const cb = V::dmul(V::dsub(Cb, V::dset(kCZero)), V::dset(kInvCSpan));
    typename V::D const cr = V::dmul(V::dsub(Cr, V::dset(kCZero)), V::dset(kInvCSpan));
    typename V::D const R = V::dadd(y, V::dmul(V::dset(kCrScale), cr));
    typename V::D const G = V::dsub(V::dsub(y, V::dmul(V::dset(kCbToG), cb)), V::dmul(V::dset(kCrToG), cr));
    typename V::D const B = V::dadd(y, V::dmul(V::dset(kCbScale), cb));
    constexpr double kEps = 1e-6;
    typename V::D const lo = V::dset(-kEps);
    typename V::D const hi = V::dset(1.0 + kEps);
    typename V::DM outside = V::deither(V::lt(R, lo), V::gt(R, hi));
    outside = V::deither(outside, V::deither(V::lt(G, lo), V::gt(G, hi)));
    outside = V::deither(outside, V::deither(V::lt(B, lo), V::gt(B, hi)));
    if (!V::dany(outside))
    {
        return outside;
    }
    typename V::D const Rc = clamp01<V>(R);
    typename V::D const Gc = clamp01<V>(G);
    typename V::D const Bc = clamp01<V>(B);
    typename V::D const yn = V::dadd(V::dadd(V::dmul(V::dset(kKr), Rc), V::dmul(V::dset(kKg), Gc)), V::dmul(V::dset(kKb), Bc));
    typename V::D const cbn = V::dmul(V::dsub(Bc, yn), V::dset(kInvCbScale));
    typename V::D const crn = V::dmul(V::dsub(Rc, yn), V::dset(kInvCrScale));
    Y = V::pick(Y, roundAway<V>(V::dadd(V::dmul(yn, V::dset(kYSpan)), V::dset(kYBlack))), outside);
    Cb = V::pick(Cb, roundAway<V>(V::dadd(V::dmul(cbn, V::dset(kCSpan)), V::dset(kCZero))), outside);
    Cr = V::pick(Cr, roundAway<V>(V::dadd(V::dmul(crn, V::dset(kCSpan)), V::dset(kCZero))), outside);
    return outside;
}

// The RGB gamut clip on the integer lanes (two halves of doubles); returns the pulled lanes.
template <class V>
CC_KERNEL typename V::M rgbClip(typename V::I& Y, typename V::I& Cb, typename V::I& Cr)
{
    typename V::D y0 = V::half0(Y), y1 = V::half1(Y);
    typename V::D b0 = V::half0(Cb), b1 = V::half1(Cb);
    typename V::D r0 = V::half0(Cr), r1 = V::half1(Cr);
    typename V::DM const p0 = rgbClipD<V>(y0, b0, r0);
    typename V::DM const p1 = rgbClipD<V>(y1, b1, r1);
    Y = V::join(y0, y1);
    Cb = V::join(b0, b1);
    Cr = V::join(r0, r1);
    return V::joinMask(p0, p1);
}

// True when every lane is clearly inside the RGB cube, so rgbClip would leave it alone: codes in
// the legal ranges, and R, G, B from 20-bit fixed-point coefficients at least 2^-10 inside [0, 1].
// Within those ranges the coefficients' rounding moves R, G and B by at most
// 0.5 · (876 + 448 + 448) · 2^-20, less than the margin.
template <class V>
CC_INLINE bool clearlyInside(typename V::I Y, typename V::I Cb, typename V::I Cr)
{
    using namespace bt709;
    constexpr double kOne = 1 << 20;
    constexpr int kY = static_cast<int>(kOne / kYSpan + 0.5);
    constexpr int kRcr = static_cast<int>(kOne * kCrScale / kCSpan + 0.5);
    constexpr int kGcb = static_cast<int>(kOne * kCbToG / kCSpan + 0.5);
    constexpr int kGcr = static_cast<int>(kOne * kCrToG / kCSpan + 0.5);
    constexpr int kBcb = static_cast<int>(kOne * kCbScale / kCSpan + 0.5);
    constexpr int kMargin = 1 << 10;
    static_assert(kY < 32768 && kRcr < 32768 && kGcb < 32768 && kGcr < 32768 && kBcb < 32768);
    typename V::I const y = V::sub(Y, V::set1(64));
    typename V::I const cb = V::sub(Cb, V::set1(512));
    typename V::I const cr = V::sub(Cr, V::set1(512));
    // 16-bit multiplies (one instruction where a 32-bit multiply is two): every coefficient is below
    // 2^15, and in each lane that passes the range test below the code fits 16 bits, so the product is
    // exact there. A lane that fails it is outside anyway, whatever its R, G and B.
    typename V::I const ly = V::mul16(y, V::set1(kY));
    typename V::I const r = V::add(ly, V::mul16(cr, V::set1(kRcr)));
    typename V::I const g = V::sub(V::sub(ly, V::mul16(cb, V::set1(kGcb))), V::mul16(cr, V::set1(kGcr)));
    typename V::I const b = V::add(ly, V::mul16(cb, V::set1(kBcb)));
    typename V::I const cLo = V::set1(-448);
    typename V::I const cHi = V::set1(448);
    typename V::M bad = V::either(V::outside(y, V::set1(0), V::set1(876)), V::either(V::outside(cb, cLo, cHi), V::outside(cr, cLo, cHi)));
    typename V::I const lo = V::set1(kMargin);
    typename V::I const hi = V::set1((1 << 20) - kMargin);
    bad = V::either(bad, V::either(V::outside(r, lo, hi), V::either(V::outside(g, lo, hi), V::outside(b, lo, hi))));
    return !V::any(bad);
}

template <class V>
CC_INLINE typename V::M gamut(typename V::I& Y, typename V::I& Cb, typename V::I& Cr)
{
    if (clearlyInside<V>(Y, Cb, Cr))
    {
        return V::none();
    }
    return rgbClip<V>(Y, Cb, Cr);
}

// With RGB gamut clip every sample gets all three rows (with its own Y), the clip, then the clamps.
template <class V, bool kStats>
CC_INLINE void rgbPair(Coeffs<V> const& k, typename V::I ye, typename V::I yo, typename V::I cb, typename V::I cr, Pair<V>& out,
    typename V::Tally& unchanged)
{
    Shared<V> const s = shared<V>(k, cb, cr);
    typename V::I y0 = rowValue<V>(s.y, ye, k.m00);
    typename V::I c0 = rowValue<V>(s.cb, ye, k.m10);
    typename V::I r0 = rowValue<V>(s.cr, ye, k.m20);
    typename V::I y1 = rowValue<V>(s.y, yo, k.m00);
    typename V::I c1 = rowValue<V>(s.cb, yo, k.m10);
    typename V::I r1 = rowValue<V>(s.cr, yo, k.m20);
    typename V::M const pulled0 = gamut<V>(y0, c0, r0);
    typename V::M const pulled1 = gamut<V>(y1, c1, r1);
    out.ye = V::clamp(y0, k.yLo, k.yHi);
    out.yo = V::clamp(y1, k.yLo, k.yHi);
    out.cb = V::clamp(c0, k.cLo, k.cHi);
    out.cr = V::clamp(r0, k.cLo, k.cHi);
    if constexpr (kStats)
    {
        typename V::M const same0 = V::both(V::same(out.ye, y0), V::both(V::same(out.cb, c0), V::same(out.cr, r0)));
        typename V::M const same1 = V::both(V::same(out.yo, y1), V::both(V::inside(c1, k.cLo, k.cHi), V::inside(r1, k.cLo, k.cHi)));
        V::count(unchanged, V::butNot(same0, pulled0));
        V::count(unchanged, V::butNot(same1, pulled1));
    }
}

template <class V, bool kRgb, bool kStats>
CC_INLINE void block(std::uint8_t const* src, std::uint8_t* dst, Coeffs<V> const& k, typename V::Tally& unchanged)
{
    using I = typename V::I;
    I w0 = V::load(src, 0);
    I w1 = V::load(src, 1);
    I w2 = V::load(src, 2);
    I w3 = V::load(src, 3);
    V::transpose(w0, w1, w2, w3);
    I const m = k.mask10;
    // v210 words: Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5.
    I const cb0 = V::and_(w0, m);
    I const y0 = V::and_(V::template srl<10>(w0), m);
    I const cr0 = V::and_(V::template srl<20>(w0), m);
    I const y1 = V::and_(w1, m);
    I const cb1 = V::and_(V::template srl<10>(w1), m);
    I const y2 = V::and_(V::template srl<20>(w1), m);
    I const cr1 = V::and_(w2, m);
    I const y3 = V::and_(V::template srl<10>(w2), m);
    I const cb2 = V::and_(V::template srl<20>(w2), m);
    I const y4 = V::and_(w3, m);
    I const cr2 = V::and_(V::template srl<10>(w3), m);
    I const y5 = V::and_(V::template srl<20>(w3), m);
    Pair<V> p0, p1, p2;
    if constexpr (kRgb)
    {
        rgbPair<V, kStats>(k, y0, y1, cb0, cr0, p0, unchanged);
        rgbPair<V, kStats>(k, y2, y3, cb1, cr1, p1, unchanged);
        rgbPair<V, kStats>(k, y4, y5, cb2, cr2, p2, unchanged);
    }
    else
    {
        matrixPair<V, kStats>(k, y0, y1, cb0, cr0, p0, unchanged);
        matrixPair<V, kStats>(k, y2, y3, cb1, cr1, p1, unchanged);
        matrixPair<V, kStats>(k, y4, y5, cb2, cr2, p2, unchanged);
    }
    w0 = V::word(p0.cb, p0.ye, p0.cr);
    w1 = V::word(p0.yo, p1.cb, p1.ye);
    w2 = V::word(p1.cr, p1.yo, p2.cb);
    w3 = V::word(p2.ye, p2.cr, p2.yo);
    V::transpose(w0, w1, w2, w3);
    V::store(dst, 0, w0);
    V::store(dst, 1, w1);
    V::store(dst, 2, w2);
    V::store(dst, 3, w3);
}

// Blocks of V over the first `full` groups, then one block of the narrower N when it still fits.
// Returns the groups done; adds the unchanged pixels to `unchanged`.
template <class V, class N, bool kRgb, bool kStats>
CC_KERNEL int blocks(std::uint8_t const* src, std::uint8_t* dst, int full, Coeffs<V> const& k, Coeffs<N> const& kn, std::uint64_t& unchanged)
{
    int g = 0;
    typename V::Tally tally = V::tally0();
    for (; g + V::kGroups <= full; g += V::kGroups)
    {
        block<V, kRgb, kStats>(src + static_cast<std::size_t>(g) * 16u, dst + static_cast<std::size_t>(g) * 16u, k, tally);
    }
    if constexpr (kStats)
    {
        unchanged += V::total(tally);
    }
    if constexpr (!std::is_same_v<V, N>)
    {
        if (g + N::kGroups <= full)
        {
            typename N::Tally narrow = N::tally0();
            block<N, kRgb, kStats>(src + static_cast<std::size_t>(g) * 16u, dst + static_cast<std::size_t>(g) * 16u, kn, narrow);
            g += N::kGroups;
            if constexpr (kStats)
            {
                unchanged += N::total(narrow);
            }
        }
    }
    return g;
}

template <class V, class N, bool kStats>
CC_KERNEL void processLine(std::uint8_t const* src, std::uint8_t* dst, int width, int stride, FixedMatrix const& matrix, Coeffs<V> const& k,
    Coeffs<N> const& kn, bool wide, ProcessStats* stats)
{
    if (matrix.bypass || matrix.identity)
    {
        processV210Scalar(src, dst, width, 1, stride, stride, 0, 1, matrix, stats);
        return;
    }
    int const groups = v210Groups(width);
    // Blocks only where all 6 pixels of every group are inside the line.
    int const full = wide ? width / 6 : 0;
    std::uint64_t unchanged = 0;
    int const done = matrix.rgbClip ? blocks<V, N, true, kStats>(src, dst, full, k, kn, unchanged)
                                    : blocks<V, N, false, kStats>(src, dst, full, k, kn, unchanged);
    if constexpr (kStats)
    {
        stats->samples += 12u * static_cast<std::uint64_t>(done);
        stats->clipped += 6u * static_cast<std::uint64_t>(done) - unchanged;
    }
    detail::processGroupsAvx2(src, dst, done, groups, width, matrix, stats);
    std::size_t const written = static_cast<std::size_t>(groups) * 16u;
    if (static_cast<std::size_t>(stride) > written)
    {
        std::memset(dst + written, 0, static_cast<std::size_t>(stride) - written);
    }
}

// V for the blocks, N (no wider than V) for one more block at the end of a line.
template <class V, class N>
CC_KERNEL void processRows(std::uint8_t const* src, std::uint8_t* dst, int width, int height, int srcStride, int dstStride, int rowBegin,
    int rowEnd, FixedMatrix const& matrix, ProcessStats* stats)
{
    if (srcStride <= 0)
    {
        srcStride = v210Stride(width);
    }
    if (dstStride <= 0)
    {
        dstStride = v210Stride(width);
    }
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, height);
    Coeffs<V> const k = coeffs<V>(matrix);
    Coeffs<N> const kn = coeffs<N>(matrix);
    bool const wide = fits32(matrix);
    for (int row = rowBegin; row < rowEnd; ++row)
    {
        std::uint8_t const* in = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcStride);
        std::uint8_t* out = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstStride);
        if (stats)
        {
            processLine<V, N, true>(in, out, width, dstStride, matrix, k, kn, wide, stats);
        }
        else
        {
            processLine<V, N, false>(in, out, width, dstStride, matrix, k, kn, wide, nullptr);
        }
    }
}

#undef CC_INLINE
