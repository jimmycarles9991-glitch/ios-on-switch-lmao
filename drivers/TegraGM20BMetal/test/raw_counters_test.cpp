// SPDX-License-Identifier: MIT
// raw_counters.h and raw_counter_kernel.h: a counter sample buffer's checks and their errors as the identity's AGX driver
// words them, a sample's index and a resolve's range, PTIMER placed on the CPU's clock by the anchor of its power-up of the
// GPU (TegraGM20BClockABI.h) across simulated power cycles, the CPU's resolve and the GPU kernel's search and arithmetic in
// 32-bit words equal to it, and the words each boundary's report emits.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <random>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BRail.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_counter_kernel.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_counters.h"

namespace {
using namespace raw::counters;

TEST(RawCounters, TheDescriptorIsCheckedInTheDriversOrder)
{
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 1), Check::Ok);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 4096), Check::Ok);
    EXPECT_EQ(checkDescriptor(0, "", "", 4), Check::SetSize);
    EXPECT_EQ(checkDescriptor(2, "timestamp", "GPUTimestamp", 4), Check::SetSize);
    EXPECT_EQ(checkDescriptor(1, "stage", "GPUTimestamp", 4), Check::SetName);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "Cycles", 0), Check::CounterName);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 0), Check::Length);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 4097), Check::Length);
    // sampleCount << 3 wraps as the driver's shift does: 2^61 samples are 0 bytes
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 1ull << 61), Check::Length);
}

TEST(RawCounters, ALengthIsOneByteToThirtyTwoKiBInSamples)
{
    EXPECT_EQ(checkLength(0), Check::Length);
    EXPECT_EQ(checkLength(1), Check::Alignment);
    EXPECT_EQ(checkLength(8), Check::Ok);
    EXPECT_EQ(checkLength(12), Check::Alignment);
    EXPECT_EQ(checkLength(0x8000), Check::Ok);
    EXPECT_EQ(checkLength(0x8001), Check::Length);
    EXPECT_EQ(checkLength(~0ull), Check::Length);
}

TEST(RawCounters, ErrorsAreTheDriversWords)
{
    EXPECT_EQ(checkMessage(Check::SetSize, "", 0), "Invalid counter set size. Expecting 1 counter");
    EXPECT_EQ(checkMessage(Check::SetName, "stage", 0), "Invalid counter set name. Expecting: timestamp. Found: stage");
    EXPECT_EQ(checkMessage(Check::CounterName, "(null)", 0),
              "Invalid counter name. Expecting: GPUTimestamp. Found: (null)");
    EXPECT_EQ(checkMessage(Check::Length, "", 32776),
              "Invalid sample buffer length: 32776 B. Expected range: 8 -> 32768");
    EXPECT_EQ(checkMessage(Check::Alignment, "", 12),
              "Invalid sample buffer length: 12 B. Length must be aligned to 8 B");
    EXPECT_EQ(checkMessage(Check::Ok, "", 8), "");
}

TEST(RawCounters, AnIndexPastTheSamplesSamplesNothing)
{
    EXPECT_EQ(sampleIndex(~0ull, 4), Index::None);  // MTLCounterDontSample
    EXPECT_EQ(sampleIndex(0xffffffffull, 4), Index::None);
    EXPECT_EQ(sampleIndex(0, 4), Index::Sample);
    EXPECT_EQ(sampleIndex(3, 4), Index::Sample);
    EXPECT_EQ(sampleIndex(4, 4), Index::Invalid);
}

TEST(RawCounters, ResolveRangesAreTheDriversBounds)
{
    EXPECT_TRUE(resolvable(0, 4, 4));
    EXPECT_TRUE(resolvable(3, 1, 4));
    EXPECT_FALSE(resolvable(0, 0, 4));
    EXPECT_FALSE(resolvable(3, 2, 4));
    EXPECT_FALSE(resolvable(5, 1, 4));
    EXPECT_FALSE(resolvable(~0ull, 2, 4));
    EXPECT_TRUE(withinSamples(4, 0, 4));
    EXPECT_TRUE(withinSamples(0, 4, 4));
    EXPECT_FALSE(withinSamples(1, 4, 4));
    EXPECT_FALSE(withinSamples(2, ~0ull, 4));
}

TEST(RawCounters, PtimerUnitsAre625Over384Nanoseconds)
{
    EXPECT_EQ(unitsToNs(0), 0);
    EXPECT_EQ(unitsToNs(384), 625);
    EXPECT_EQ(unitsToNs(32), 52);  // one tick of the 19.2 MHz oscillator: 52.083 ns
    EXPECT_EQ(unitsToNs(19'200'000ll * 32), 1'000'000'000);
    EXPECT_EQ(unitsToNs(-384), -625);
    EXPECT_EQ(unitsToNs(-1), -2);
    EXPECT_EQ(unitsToNs(1), 1);
}

TEST(RawCounters, AnAnchorPlacesUnitsOnTheCpusClock)
{
    const Anchor a{600'000'000, 1'000'002'000, true};
    EXPECT_EQ(nanoseconds(a, 600'000'000 + 384 * 1000), 1'000'002'000u + 625'000u);
    EXPECT_EQ(nanoseconds(a, 600'000'000 - 384 * 1000), 1'000'002'000u - 625'000u);
}

TegraGM20BAbiClockAnchors table(std::initializer_list<TegraGM20BAbiClockAnchor> anchors)
{
    TegraGM20BAbiClockAnchors t = {};
    for (const TegraGM20BAbiClockAnchor &a : anchors)
        *tgm_clock_anchor_slot(&t, t.published++) = a;
    return t;
}

TEST(RawCounters, AValuesAnchorIsTheNewestAtOrBelowIt)
{
    const TegraGM20BAbiClockAnchors t = table({{1000, 5000}, {9000, 20000}, {9500, 90000}});
    EXPECT_FALSE(anchorOf(t, 999).valid);
    EXPECT_EQ(anchorOf(t, 1000).ns, 5000u);
    EXPECT_EQ(anchorOf(t, 8999).ns, 5000u);
    EXPECT_EQ(anchorOf(t, 9000).ns, 20000u);
    EXPECT_EQ(anchorOf(t, 9499).ns, 20000u);
    EXPECT_EQ(anchorOf(t, ~0ull).ns, 90000u);
    EXPECT_FALSE(anchorOf(TegraGM20BAbiClockAnchors{}, 5).valid);
    EXPECT_EQ(resolved(Record{0, 0, 9000}, t), 0u);
    EXPECT_EQ(resolved(Record{kPayload, 0, 9384}, t), 20625u);
    EXPECT_EQ(resolved(Record{kPayload, 0, 999}, t), kErrorValue);
}

TEST(RawCounters, TheTableKeepsAllButTheSlotTheNextPowerUpWrites)
{
    TegraGM20BAbiClockAnchors t = {};
    for (uint64_t n = 0; n < 200; n++)
        *tgm_clock_anchor_slot(&t, t.published++) = {1000 * (n + 1), 7 * (n + 1)};
    // anchors 138..199 are kept; 137's slot is the next one written
    EXPECT_EQ(anchorOf(t, 1000 * 200 + 5).ns, 7u * 200);
    EXPECT_EQ(anchorOf(t, 1000 * 139).ns, 7u * 139);
    EXPECT_FALSE(anchorOf(t, 1000 * 139 - 1).valid);
}

// The kernel and the GPU over power cycles: PTIMER restarts from the CPU's clock at each wake (nvkm_timer_init), is reseeded
// past the last value before the power-down (gpu_power.c stamp_reseed), counts 32 units a tick of 19.2 MHz while powered,
// and each power-up's anchor is published before any channel runs. Samples are taken across 90 cycles, some of whose wakes
// the GPU's clock is ahead of the CPU's, then resolved: each to its true time, within the tick it was taken in, until more
// power-ups than the table keeps have passed since; the GPU kernel's words equal the CPU's resolve throughout.
struct PoweredGpu {
    uint64_t wokeAt = 0, startUnits = 0, last = 0;
    TegraGM20BAbiClockAnchors anchors = {};

    uint64_t ptimer(uint64_t ns) const
    {
        return startUnits + (ns - wokeAt) * 192 / 10'000 * 32;
    }

    void wake(uint64_t ns)
    {
        wokeAt = ns;
        startUnits = ns;
        if (const uint64_t floor = tgm_stamp_reseed(last, ptimer(ns)))
            startUnits = floor;
        *tgm_clock_anchor_slot(&anchors, anchors.published++) = {ptimer(ns), ns};
    }

    void sleep(uint64_t ns)
    {
        last = std::max(last, ptimer(ns));
    }
};

TEST(RawCounters, SamplesResolveToTheirTimeAcrossPowerUps)
{
    std::mt19937_64 random(0xc10c);
    PoweredGpu gpu;

    struct Sample {
        uint64_t ns, units;
        uint32_t cycle;
    };

    std::vector<Sample> samples;
    uint64_t now = 3'000'000'000;
    const uint32_t cycles = 90;
    for (uint32_t cycle = 0; cycle < cycles; cycle++) {
        // a handed-out GPU time ahead of the CPU's clock at the wake (tgm_timestamp_ns), every seventh cycle
        if (cycle % 7 == 3)
            gpu.last = std::max(gpu.last, now + 40'000 + random() % 1'000'000);
        gpu.wake(now);
        const uint64_t up = 1'000'000 + random() % 2'000'000'000;
        for (int i = 0; i < 40; i++) {
            const uint64_t at = now + random() % up;
            samples.push_back({at, gpu.ptimer(at), cycle});
        }
        now += up;
        gpu.sleep(now);
        now += 1000 + random() % 60'000'000'000ull;
    }
    uint32_t exact = 0, beyond = 0;
    for (const Sample &s : samples) {
        const Record r{kPayload, 0, s.units};
        const uint64_t ns = resolved(r, gpu.anchors);
        ASSERT_EQ(resolvedInWords(r, gpu.anchors), ns) << "cycle " << s.cycle;
        if (s.cycle + TegraGM20BClockAnchorSlots - 1 <= cycles - 1) {
            EXPECT_EQ(ns, kErrorValue) << "cycle " << s.cycle << ": its anchor is no longer kept";
            beyond++;
            continue;
        }
        ASSERT_LE(ns, s.ns) << "cycle " << s.cycle;
        ASSERT_LT(s.ns - ns, 53u) << "cycle " << s.cycle << ": more than the tick it was taken in from its time";
        exact++;
    }
    EXPECT_EQ(exact, 62u * 40);
    EXPECT_EQ(beyond, (cycles - 62) * 40);
}

TEST(RawCounters, AnEarlierPowerUpsSampleTakesItsOwnAnchor)
{
    PoweredGpu gpu;
    gpu.wake(1'000'000'000);
    const Record before{kPayload, 0, gpu.ptimer(1'500'000'000)};
    gpu.sleep(2'000'000'000);
    gpu.wake(12'000'000'000);
    const Record after{kPayload, 0, gpu.ptimer(12'250'000'000)};
    EXPECT_EQ(resolved(before, gpu.anchors), 1'500'000'000u);
    EXPECT_EQ(resolved(after, gpu.anchors), 12'250'000'000u);
    // the later power-up's anchor alone does not place it
    EXPECT_EQ(resolved(before, table({gpu.anchors.slot[1]})), kErrorValue);
}

TEST(RawCounters, TheKernelsWordArithmeticIsTheCpusResolve)
{
    std::mt19937_64 random(0x5eed);
    const uint64_t bases[] = {
        0, 1, 0xffffffffull, 0x1'0000'0000ull, 600'000'000, 0x7fff'ffff'0000'0000ull, 0x0000'1234'ffff'ffffull};
    uint64_t compared = 0;
    for (const uint64_t base : bases)
        for (const uint64_t ns : {uint64_t(0), uint64_t(0xffff'fff0), uint64_t(123'456'789'012'345)})
            for (int i = 0; i < 2000; i++) {
                const TegraGM20BAbiClockAnchors t = table({{base, ns}});
                const uint64_t span = random() % (1ull << 51);
                const uint64_t units = i < 8 ? base + uint64_t(int64_t(i) - 4) : base + span;
                const Record r{kPayload, 0, units};
                ASSERT_EQ(resolvedInWords(r, t), resolved(r, t))
                    << "anchor " << base << " ns " << ns << " units " << units;
                compared++;
            }
    EXPECT_EQ(compared, 7u * 3 * 2000);
    const TegraGM20BAbiClockAnchors one = table({{1, 2}});
    EXPECT_EQ(resolvedInWords(Record{0, 0, 5}, one), 0u);
    EXPECT_EQ(resolvedInWords(Record{kPayload, 0, 5}, TegraGM20BAbiClockAnchors{}), kErrorValue);
    EXPECT_EQ(resolvedInWords(Record{kPayload, 0, 0}, one), kErrorValue);
}

// What the GPU does with a report: its payload and PTIMER at the report's address, the record a sample is resolved from
Record released(const uint32_t *words, size_t addressWord, uint64_t units)
{
    return Record{words[addressWord + 2], 0, units};
}

TEST(RawCounters, ThreeDReportsAreNvksTimestampAtEachStagesLocation)
{
    const uint64_t va = 0x12'3456'7890ull;
    // SET_REPORT_SEMAPHORE_D: OPERATION RELEASE (1:0 = 0), RELEASE AFTER_ALL_PRECEEDING_WRITES_COMPLETE (4:4 = 1),
    // PIPELINE_LOCATION (15:12), STRUCTURE_SIZE FOUR_WORDS (28:28 = 0) (clb197.h)
    const std::pair<Point, uint32_t> locations[] = {
        {Point::VertexStart, 0x1},    // DATA_ASSEMBLER
        {Point::VertexEnd, 0x6},      // GEOMETRY_SHADER
        {Point::FragmentStart, 0xA},  // PIXEL_SHADER
        {Point::FragmentEnd, 0xF},    // ALL
    };
    for (const auto &[point, location] : locations) {
        uint32_t words[kTimestampWords] = {};
        raw::Push p{words, words + kTimestampWords};
        emit(p, {va, point});
        ASSERT_EQ(p.at - words, 6) << int(point);
        EXPECT_EQ(words[0], raw::Push::hdr(4, raw::SUBC_3D, 0x1144, 0));  // FLUSH_PENDING_WRITES
        EXPECT_EQ(words[1], raw::Push::hdr(1, raw::SUBC_3D, 0x1b00, 4));
        EXPECT_EQ(words[2], 0x12u);
        EXPECT_EQ(words[3], 0x34567890u);
        EXPECT_EQ(words[4], kPayload);
        EXPECT_EQ(words[5], 1u << 4 | location << 12) << int(point);
        const Record r = released(words, 2, 77);
        EXPECT_EQ(r.payload, kPayload);
    }
}

TEST(RawCounters, ComputeReportsAreTheClasssFourWordRelease)
{
    uint32_t words[kTimestampWords] = {};
    raw::Push p{words, words + kTimestampWords};
    emit(p, {0x1'0000'0040ull, Point::ComputeStart});
    ASSERT_EQ(p.at - words, 5);
    EXPECT_EQ(words[0], raw::Push::hdr(1, raw::SUBC_COMPUTE, 0x1b00, 4));
    EXPECT_EQ(words[1], 0x1u);
    EXPECT_EQ(words[2], 0x40u);
    EXPECT_EQ(words[3], kPayload);
    EXPECT_EQ(words[4], 0u);  // RELEASE, FOUR_WORDS (clb1c0.h)
    uint32_t end[kTimestampWords] = {};
    raw::Push e{end, end + kTimestampWords};
    emit(e, {0x1'0000'0040ull, Point::ComputeEnd});
    EXPECT_EQ(memcmp(words, end, sizeof words), 0);
}

TEST(RawCounters, BlitReportsAreTheCopyEnginesSemaphoreOfAnEmptyLaunch)
{
    const uint64_t va = 0xab'cdef'0010ull;
    uint32_t start[kTimestampWords] = {}, end[kTimestampWords] = {};
    raw::Push ps{start, start + kTimestampWords}, pe{end, end + kTimestampWords};
    emit(ps, {va, Point::BlitStart});
    emit(pe, {va, Point::BlitEnd});
    // SET_SEMAPHORE_A..PAYLOAD; LAUNCH_DMA: DATA_TRANSFER_TYPE (1:0) NONE or NON_PIPELINED (2), FLUSH_ENABLE (2:2),
    // SEMAPHORE_TYPE (4:3) RELEASE_FOUR_WORD_SEMAPHORE (2) (clb0b5.h)
    ASSERT_EQ(ps.at - start, 5);
    EXPECT_EQ(start[0], raw::Push::hdr(1, raw::SUBC_COPY, 0x240, 3));
    EXPECT_EQ(start[1], 0xabu);
    EXPECT_EQ(start[2], 0xcdef0010u);
    EXPECT_EQ(start[3], kPayload);
    EXPECT_EQ(start[4], raw::Push::hdr(4, raw::SUBC_COPY, 0x300, 1u << 2 | 2u << 3));
    ASSERT_EQ(pe.at - end, 8);
    EXPECT_EQ(memcmp(start, end, 4 * sizeof(uint32_t)), 0);
    EXPECT_EQ(end[4], raw::Push::hdr(1, raw::SUBC_COPY, 0x418, 2));  // LINE_LENGTH_IN, LINE_COUNT 0
    EXPECT_EQ(end[5], 0u);
    EXPECT_EQ(end[6], 0u);
    EXPECT_EQ(end[7], raw::Push::hdr(4, raw::SUBC_COPY, 0x300, 2u | 1u << 2 | 2u << 3));
}

TEST(RawCounters, StartsAreTheEncodersFirstBoundaries)
{
    for (const Point p : {Point::VertexStart, Point::FragmentStart, Point::ComputeStart, Point::BlitStart})
        EXPECT_TRUE(isStart(p));
    for (const Point p : {Point::VertexEnd, Point::FragmentEnd, Point::ComputeEnd, Point::BlitEnd})
        EXPECT_FALSE(isStart(p));
}

}  // namespace
