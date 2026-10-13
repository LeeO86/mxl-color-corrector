#pragma once

#include <string>

namespace cc
{

// The code that processes v210 (CC_SIMD). Every level gives the same bytes and statistics.
enum class SimdLevel
{
    Scalar,
    Avx2,
    Avx512Vl, // AVX-512 instructions on 256-bit registers (masks, 32 registers)
    Avx512,   // AVX-512 on 512-bit registers
};

[[nodiscard]] char const* simdName(SimdLevel level);
// auto, avx512, avx512vl, avx2 or scalar; false for anything else.
[[nodiscard]] bool validSimdRequest(std::string const& text);

// What this CPU and the OS offer (cpuid, and XGETBV for the register state the OS saves).
struct CpuSimd
{
    bool avx2 = false;   // AVX2, ymm state enabled
    bool avx512 = false; // AVX-512 F, VL, BW and DQ, opmask and zmm state enabled
    // The AVX-512 class that sets the auto choice: "skylake-sp" (Intel without VBMI2: Skylake-SP,
    // Cascade Lake, Cooper Lake), "icelake+" (Intel with VBMI2), "zen4", "zen5", "other"; empty
    // without AVX-512.
    std::string family;
};

[[nodiscard]] CpuSimd detectCpuSimd();
// The auto choice for a CPU (docs/performance.md, "AVX-512").
[[nodiscard]] SimdLevel autoSimdLevel(CpuSimd const& cpu);

struct SimdChoice
{
    SimdLevel level = SimdLevel::Scalar;
    std::string requested = "auto";
    std::string warning; // set when the requested level is not available here
};

// The level for a CC_SIMD value: auto takes autoSimdLevel; a level the CPU lacks falls back to the
// best one below it, with a warning.
[[nodiscard]] SimdChoice chooseSimd(std::string const& requested, CpuSimd const& cpu);

// The level processV210 uses. Until it is set, the auto choice for this CPU.
void setSimdLevel(SimdLevel level);
[[nodiscard]] SimdLevel simdLevel();

} // namespace cc
