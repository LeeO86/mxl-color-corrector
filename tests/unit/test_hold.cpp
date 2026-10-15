// The held output (hold.hpp): its indices, where the output resumes, the limit and black.

#include "doctest/doctest.h"

#include "hold.hpp"
#include "v210.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace cc;

TEST_CASE("held grains keep the output's phase, also for a source behind the clock")
{
    constexpr std::uint64_t kUp = 8;
    // A gateway indexing by capture time: its grains are 12 behind the clock, so the output
    // committed grain 988 when the clock was at 1000.
    auto const phase = hold::phaseOf(988, 1000);
    CHECK(phase == -12);
    auto range = hold::heldRange(1003, phase, 988, kUp);
    CHECK(range.first == 989);
    CHECK(range.last == 991);
    // Nothing to write while the clock has not moved on.
    range = hold::heldRange(1000, phase, 988, kUp);
    CHECK(range.first > range.last);
    // A long gap jumps to the target instead of writing every grain.
    range = hold::heldRange(2000, phase, 988, kUp);
    CHECK(range.first == 1988);
    CHECK(range.last == 1988);
    // A source ahead of the clock.
    CHECK(hold::heldRange(1000, 5, 1003, kUp).first == 1004);
    CHECK(hold::heldRange(1000, 5, 1003, kUp).last == 1005);
    CHECK(hold::heldRange(hold::kUndefined, 0, 10, kUp).first > hold::heldRange(hold::kUndefined, 0, 10, kUp).last);
}

TEST_CASE("a re-created source 12 grains behind the clock resumes after the held grains, never below them")
{
    // 1.2.1's first build held at the clock and resumed at head − offset when that was less than
    // 8 grains behind: with a source 12 behind, the output index went back and MXL refused it.
    std::uint64_t clock = 1'000'000;
    constexpr std::uint64_t lag = 12;
    std::uint64_t lastOut = clock - lag - 1; // whole-grain mode, offset 1
    auto const phase = hold::phaseOf(lastOut, clock);
    // Held for 50 grains.
    for (int i = 0; i < 50; ++i)
    {
        ++clock;
        auto const range = hold::heldRange(clock, phase, lastOut, 21);
        for (auto next = range.first; next <= range.last; ++next)
        {
            CHECK(next > lastOut);
            lastOut = next;
        }
    }
    CHECK(lastOut == clock - lag - 1);
    // The writer is back with the same lag: its head is the grain at the clock − lag.
    int waits = 0;
    hold::Resume resume;
    for (;; ++clock, ++waits)
    {
        resume = hold::resumeAt(clock - lag, 1, false, lastOut, 21);
        if (!resume.wait()) break;
        REQUIRE(waits < 3);
    }
    CHECK(resume.action == hold::Action::Read);
    CHECK(resume.index > lastOut);
    CHECK(resume.index == lastOut + 1);
    CHECK(waits <= 1);

    // The new writer runs 10 grains later than the old one: the channel waits for it (10 grains
    // without output) rather than going back.
    auto const later = hold::resumeAt(clock - lag - 10, 1, false, lastOut, 21);
    CHECK(later.wait());
    CHECK(later.lag == 10);
    // A writer indexing by UTC, 37 s (1850 grains) behind: the output flow is made again.
    auto const utc = hold::resumeAt(clock - lag - 1850, 1, false, lastOut, 21);
    CHECK(utc.action == hold::Action::Rephase);
    CHECK(utc.lag == 1850);
    CHECK(utc.index == clock - lag - 1851);
    // In slice mode the head itself is read.
    CHECK(hold::resumeAt(lastOut + 1, 1, true, lastOut, 21).index == lastOut + 1);
}

TEST_CASE("resume never goes at or below the last committed grain")
{
    std::mt19937_64 rng(7);
    for (int i = 0; i < 20000; ++i)
    {
        std::uint64_t const lastOut = 1000 + rng() % 1000;
        std::uint64_t const head = 900 + rng() % 1200;
        int const offset = static_cast<int>(rng() % 3);
        bool const slice = (rng() & 1) != 0;
        std::uint64_t const up = 8 + rng() % 30;
        auto const resume = hold::resumeAt(head, offset, slice, lastOut, up);
        if (resume.action != hold::Action::Rephase) CHECK(resume.index > lastOut);
        if (resume.action == hold::Action::Wait) CHECK(resume.lag <= up);
        if (resume.action == hold::Action::Rephase) CHECK(resume.lag > up);
    }
    // Without an output yet, the head (less the whole-grain offset) is read.
    CHECK(hold::resumeAt(500, 1, false, hold::kUndefined, 21).index == 499);
    CHECK(hold::resumeAt(500, 1, true, hold::kUndefined, 21).index == 500);
}

TEST_CASE("CC_HOLD_MAX_S: the hold turns black after the limit, 0 holds forever")
{
    CHECK_FALSE(hold::expired(109.99, 100.0, 10));
    CHECK(hold::expired(110.0, 100.0, 10));
    CHECK_FALSE(hold::expired(1e9, 0.0, 0));
    CHECK(hold::expired(101.0, 100.0, 1));
}

TEST_CASE("black v210 is Y 64, Cb and Cr 512 on every line, padding untouched")
{
    for (int width : {2, 6, 48, 640, 1920})
    {
        int const height = 4;
        int const stride = v210Stride(width);
        auto const size = static_cast<std::size_t>(stride) * height + 256;
        auto const frame = hold::blackV210(width, height, size);
        REQUIRE(frame.size() == size);
        for (int row = 0; row < height; ++row)
        {
            std::vector<std::uint16_t> y(static_cast<std::size_t>(width)), cb(static_cast<std::size_t>(width / 2)), cr(static_cast<std::size_t>(width / 2));
            unpackV210Line(frame.data() + static_cast<std::size_t>(row) * stride, width, y.data(), cb.data(), cr.data());
            for (auto v : y) CHECK(v == 64);
            for (auto v : cb) CHECK(v == 512);
            for (auto v : cr) CHECK(v == 512);
        }
        for (std::size_t i = static_cast<std::size_t>(stride) * height; i < size; ++i) CHECK(frame[i] == 0);
    }
}

TEST_CASE("the hold fills in the stall time without a jump, so no grain is marked invalid")
{
    // 1080p50: the stall hold starts 250 ms (12.5 grains) after the last write; the first held
    // grains must follow the last output grain, not jump (MXL would mark the skipped ones invalid).
    auto const up50 = hold::catchUpGrains(0.25, 50, 1);
    CHECK(up50 >= 13 + hold::kCatchUpMargin);
    auto const range = hold::heldRange(1000 + 14, 0, 1000, up50);
    CHECK(range.first == 1001);
    CHECK(range.last == 1014);
    // 23.98: ten grain periods (417 ms) is the threshold.
    CHECK(hold::catchUpGrains(10.0 * 1001 / 24000, 24000, 1001) >= 10 + hold::kCatchUpMargin);
    // A gap far beyond it still jumps.
    CHECK(hold::heldRange(1000 + 500, 0, 1000, up50).first == 1500);
}
