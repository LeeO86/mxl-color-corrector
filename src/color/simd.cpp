#include "color/simd.hpp"

#include <atomic>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

namespace cc
{
namespace
{
std::atomic<int> gLevel{-1};

#if defined(__x86_64__) || defined(_M_X64)
unsigned long long xcr0()
{
    unsigned int lo = 0, hi = 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<unsigned long long>(hi) << 32) | lo;
}
#endif
} // namespace

char const* simdName(SimdLevel level)
{
    switch (level)
    {
        case SimdLevel::Avx512:
            return "avx512";
        case SimdLevel::Avx512Vl:
            return "avx512vl";
        case SimdLevel::Avx2:
            return "avx2";
        case SimdLevel::Scalar:
        default:
            return "scalar";
    }
}

bool validSimdRequest(std::string const& text)
{
    return text == "auto" || text == "avx512" || text == "avx512vl" || text == "avx2" || text == "scalar";
}

CpuSimd detectCpuSimd()
{
    CpuSimd cpu;
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0)
    {
        return cpu;
    }
    char vendor[13] = {};
    std::memcpy(vendor, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 4);
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    unsigned int const baseFamily = (eax >> 8) & 0xf;
    unsigned int const family = baseFamily == 0xf ? baseFamily + ((eax >> 20) & 0xff) : baseFamily;
    bool const osxsave = (ecx & (1u << 27)) != 0;
    bool const avx = (ecx & (1u << 28)) != 0;
    if (!osxsave || !avx)
    {
        return cpu;
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0)
    {
        return cpu;
    }
    unsigned long long const xcr = xcr0();
    // XCR0 bits 1, 2: SSE and AVX state; 5, 6, 7: opmask, upper halves of zmm0-15, zmm16-31.
    bool const ymmState = (xcr & 0x6u) == 0x6u;
    bool const zmmState = (xcr & 0xe6u) == 0xe6u;
    cpu.avx2 = ymmState && (ebx & (1u << 5)) != 0;
    bool const f = (ebx & (1u << 16)) != 0;
    bool const dq = (ebx & (1u << 17)) != 0;
    bool const bw = (ebx & (1u << 30)) != 0;
    bool const vl = (ebx & (1u << 31)) != 0;
    cpu.avx512 = cpu.avx2 && zmmState && f && dq && bw && vl;
    if (cpu.avx512)
    {
        bool const vbmi2 = (ecx & (1u << 6)) != 0;
        if (std::strcmp(vendor, "GenuineIntel") == 0)
        {
            cpu.family = vbmi2 ? "icelake+" : "skylake-sp";
        }
        else if (std::strcmp(vendor, "AuthenticAMD") == 0 && family == 0x19)
        {
            cpu.family = "zen4";
        }
        else if (std::strcmp(vendor, "AuthenticAMD") == 0 && family >= 0x1a)
        {
            cpu.family = "zen5";
        }
        else
        {
            cpu.family = "other";
        }
    }
#endif
    return cpu;
}

SimdLevel autoSimdLevel(CpuSimd const& cpu)
{
    if (!cpu.avx2)
    {
        return SimdLevel::Scalar;
    }
    // AVX-512 is the auto choice only where it saves at least 15 % of the process's CPU and costs a
    // workload on the same core less than 3 % (docs/performance.md, "AVX-512"). Skylake-SP class:
    // 512-bit code drops the core into the lowest frequency licence, which slows the other
    // hyperthread by 3.4–4 % (measured), and 256-bit AVX-512 saves too little. Ice Lake and later,
    // Zen 4 and Zen 5 keep their clock with AVX-512 (published; not measured here).
    if (cpu.avx512 && (cpu.family == "icelake+" || cpu.family == "zen4" || cpu.family == "zen5"))
    {
        return SimdLevel::Avx512;
    }
    return SimdLevel::Avx2;
}

SimdChoice chooseSimd(std::string const& requested, CpuSimd const& cpu)
{
    SimdChoice choice;
    choice.requested = requested.empty() ? "auto" : requested;
    SimdLevel const best = cpu.avx512 ? SimdLevel::Avx512 : cpu.avx2 ? SimdLevel::Avx2 : SimdLevel::Scalar;
    SimdLevel want = autoSimdLevel(cpu);
    if (choice.requested == "avx512")
    {
        want = SimdLevel::Avx512;
    }
    else if (choice.requested == "avx512vl")
    {
        want = SimdLevel::Avx512Vl;
    }
    else if (choice.requested == "avx2")
    {
        want = SimdLevel::Avx2;
    }
    else if (choice.requested == "scalar")
    {
        want = SimdLevel::Scalar;
    }
    choice.level = want;
    if (static_cast<int>(want) > static_cast<int>(best))
    {
        choice.level = best;
        choice.warning = "CC_SIMD=" + choice.requested + " but this CPU offers " + simdName(best) + "; using " + simdName(best);
    }
    return choice;
}

void setSimdLevel(SimdLevel level)
{
    gLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

SimdLevel simdLevel()
{
    int level = gLevel.load(std::memory_order_relaxed);
    if (level < 0)
    {
        level = static_cast<int>(autoSimdLevel(detectCpuSimd()));
        gLevel.store(level, std::memory_order_relaxed);
    }
    return static_cast<SimdLevel>(level);
}

} // namespace cc
