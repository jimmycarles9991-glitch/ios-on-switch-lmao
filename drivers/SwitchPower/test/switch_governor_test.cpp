// The CPU's on-demand clock governor (drivers/SwitchPower/SwitchGovernor.h): load traces in, operating points out, with
// the tunables the kernel uses (read from SwitchPerfGovernor.cpp, so a retune is tested as shipped).
//
// Checks: saturation reaches the ceiling within one window and a partial burst within two (a frame at 60 Hz is two
// CPU windows); idle and short dips keep the clock until the down hold has passed, then the clock goes to the highest
// target the hold saw; boosts raise at once and expire; thermal ceilings and the cold floor clamp at once; nothing ever
// leaves [lo, hi] or the table (fuzzed). The table: L4T's T210B01 CVB rows (clk-tegra124-dfll-fcpu.c CPUB01_CVB_TABLE,
// evaluated as cvb.c does) cut at Horizon's 1785 MHz boost clock, re-derived here independently and compared with what
// the header picks. The GPU is not governed: it runs at its fixed clock.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "../SwitchPerfGovernor.h"  // kBootFallbackSeconds
#include "bazel/testing/require.h"
#include "bazel/testing/source_text.h"
#include "switch_governor_harness.h"

namespace {

constexpr uint32_t kNintendoCpuMaxKhz = 1785000;  // Horizon's boost-mode CPU clock (sys-clk: "1785 -> max clock")
constexpr uint32_t kCpuVminMv = 620;              // CPUB01_CVB_TABLE vmin_coefficients {620000, 0, 0}
constexpr uint32_t kCpuColdFloorMv = 800;         // tegra210b01_thermal_floor_table: below 20 C
constexpr uint32_t kCpuCapsMv[2] = {1060, 1010};  // tegra210b01_thermal_cap_table: from 64 C, from 84 C
constexpr uint64_t kMs = 1000;                    // the tests' time unit is the microsecond

// L4T CTCaer/switch-l4t-kernel-4.9 clk-tegra124-dfll-fcpu.c CPUB01_CVB_TABLE (the row every T210B01 speedo id 0/3
// takes): kHz, c0, c1, c2; speedo scale 100, voltage scale 1000.
struct CvbRow {
    uint32_t khz;
    int64_t c0, c1, c2;
};

constexpr CvbRow kCpuB01Cvb[] = {
    {204000, 721589, -12695, 27},   {306000, 747134, -14195, 27},   {408000, 776324, -15705, 27},
    {510000, 809160, -17205, 27},   {612000, 845641, -18715, 27},   {714000, 885768, -20215, 27},
    {816000, 929540, -21725, 27},   {918000, 976958, -23225, 27},   {1020000, 1028021, -24725, 27},
    {1122000, 1082730, -26235, 27}, {1224000, 1141084, -27735, 27}, {1326000, 1203084, -29245, 27},
    {1428000, 1268729, -30745, 27}, {1581000, 1374032, -33005, 27}, {1683000, 1448791, -34505, 27},
    {1785000, 1527196, -36015, 27}, {1887000, 1609246, -37515, 27}, {1963500, 1675751, -38635, 27},
    {2014500, 1716501, -39395, 27}, {2091000, 1716501, -39395, 27}, {2193000, 1775132, -40505, 27},
    {2295000, 1866287, -42005, 27},
};

// Linux DIV_ROUND_CLOSEST for a positive divisor (C's truncating division underneath).
int64_t divRoundClosest(int64_t x, int64_t d)
{
    return x > 0 ? (x + d / 2) / d : (x - d / 2) / d;
}

// Rounding up to a multiple of `d` (d > 0), for any sign.
int64_t ceilDiv(int64_t a, int64_t d)
{
    return a >= 0 ? (a + d - 1) / d : -((-a) / d);
}

using Rows = std::vector<std::pair<uint32_t, uint32_t>>;  // (kHz, mV)

// cvb.c build_opp_table for CPUB01_CVB_TABLE: CVB voltage rounded up to the MAX77812's 5 mV step, clamped to [the CVB
// Vmin, the table's max_millivolts].
Rows cpuRows(int64_t speedo)
{
    Rows rows;
    for (const CvbRow &r : kCpuB01Cvb) {
        int64_t uv = divRoundClosest(r.c2 * speedo, 100);
        uv = divRoundClosest((uv + r.c1) * speedo, 100) + r.c0;
        const int64_t mv = ceilDiv(uv, 5000) * 5;
        rows.emplace_back(r.khz, (uint32_t)std::min<int64_t>(std::max<int64_t>(mv, kCpuVminMv), 1235));
    }
    return rows;
}

// A constexpr tunable of SwitchPerfGovernor.cpp ("name = value"), as the kernel is built with it.
uint64_t tunable(const std::string &source, const std::string &name)
{
    std::smatch match;
    const std::regex pattern("\\b" + name + "\\s*=\\s*([0-9]+)");
    REQUIRE(std::regex_search(source, match, pattern));
    return std::stoull(match[1]);
}

struct Tunables {
    uint64_t window, up, target, hold, touchHold;
};

const Tunables &tunables()
{
    static const Tunables t = [] {
        const std::string cpu = switch_ios::test::SourceText("../SwitchPerfGovernor.cpp");
        return Tunables{tunable(cpu, "kCpuWindowUs"), tunable(cpu, "kCpuUpPermille"),
                        tunable(cpu, "kCpuTargetPermille"), tunable(cpu, "kCpuDownHoldUs"),
                        tunable(cpu, "kBoostTouchUs")};
    }();
    return t;
}

struct Table {
    std::vector<uint32_t> khz, mv;
};

Table cpuTable(int64_t speedo = 1659, uint32_t vmin = kCpuVminMv)
{
    const Rows rows = cpuRows(speedo);
    std::vector<uint32_t> khz, mv;
    for (const auto &[k, v] : rows) {
        khz.push_back(k);
        mv.push_back(v);
    }
    Table out;
    out.khz.resize(24);
    out.mv.resize(24);
    const unsigned count = gov_cpu_table(khz.data(), mv.data(), (unsigned)rows.size(), kNintendoCpuMaxKhz, vmin,
                                         out.khz.data(), out.mv.data(), 24);
    out.khz.resize(count);
    out.mv.resize(count);
    return out;
}

// One governor (the header's state, opaque here).
class Governor {
public:
    explicit Governor(const std::vector<uint32_t> &khz, unsigned start = 0, uint64_t now = 0)
        : Governor(khz, start, tunables().up, tunables().target, tunables().hold, now)
    {}

    Governor(const std::vector<uint32_t> &khz, unsigned start, uint64_t up, uint64_t target, uint64_t hold,
             uint64_t now)
        : storage_((gov_size() + sizeof(uint64_t) - 1) / sizeof(uint64_t))
    {
        gov_init(g(), khz.data(), (unsigned)khz.size(), start, (uint32_t)up, (uint32_t)target, hold, now);
    }

    switch_gov *g()
    {
        return reinterpret_cast<switch_gov *>(storage_.data());
    }

    // busy: one permille per window; returns the index after each window.
    std::vector<unsigned> trace(const std::vector<uint32_t> &busy, uint64_t start = 0, uint64_t window = 0)
    {
        window = window ? window : tunables().window;
        std::vector<uint64_t> times;
        for (size_t i = 0; i < busy.size(); ++i)
            times.push_back(start + (i + 1) * window);
        std::vector<unsigned> out(busy.size());
        gov_run(g(), times.data(), busy.data(), (unsigned)busy.size(), out.data());
        return out;
    }

private:
    std::vector<uint64_t> storage_;
};

std::vector<uint32_t> repeat(std::vector<uint32_t> pattern, size_t times)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < times; ++i)
        out.insert(out.end(), pattern.begin(), pattern.end());
    return out;
}

unsigned indexOf(const std::vector<uint32_t> &khz, uint32_t value)
{
    const auto it = std::find(khz.begin(), khz.end(), value);
    REQUIRE(it != khz.end());
    return (unsigned)(it - khz.begin());
}

// ------------------------------------------------------------------------------------------------------------ tables

// CPU speedo 1659 (the device's SwitchPerf line, 11-05): from the Vmin row (918 MHz, the DVCO's lowest rate without the
// skipper) to Horizon's 1785 MHz; 1683 MHz at 885 mV as SwitchPerf logged from the same CVB row.
TEST(SwitchGovernor, CpuTableForThisChip)
{
    const Table t = cpuTable(1659);
    EXPECT_EQ(t.khz,
              (std::vector<uint32_t>{918000, 1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000}));
    REQUIRE(!t.mv.empty());
    EXPECT_EQ(t.mv[0], kCpuVminMv);
    EXPECT_EQ(t.mv[indexOf(t.khz, 1683000)], 885u);
    EXPECT_EQ(t.mv[indexOf(t.khz, 1785000)], 940u);
    EXPECT_TRUE(std::is_sorted(t.mv.begin(), t.mv.end()));
}

TEST(SwitchGovernor, CpuTableNeverAboveHorizonBoost)
{
    for (int64_t speedo = 1400; speedo <= 1950; speedo += 10) {
        const Table t = cpuTable(speedo);
        ASSERT_FALSE(t.khz.empty()) << speedo;
        const uint32_t top = *std::max_element(t.khz.begin(), t.khz.end());
        EXPECT_LE(top, kNintendoCpuMaxKhz) << speedo;
        EXPECT_EQ(top, kNintendoCpuMaxKhz) << speedo;  // every speedo's CVB reaches it under 1235 mV
        for (uint32_t k : t.khz)
            EXPECT_EQ(k % 25500, 0u) << speedo;  // whole DFLL steps (51 MHz ref / 2)
        const Rows rows = cpuRows(speedo);
        for (size_t i = 0; i < t.khz.size(); ++i)  // each pair is a CVB row, voltage and all
            EXPECT_NE(std::find(rows.begin(), rows.end(), std::make_pair(t.khz[i], t.mv[i])), rows.end())
                << speedo << ": " << t.khz[i] << " kHz at " << t.mv[i] << " mV";
    }
}

// L4T's caps (1060 mV from 64 C, 1010 mV from 84 C) never bind on this chip (1785 MHz needs 940 mV); on a slow chip
// (speedo 1450: 1785 MHz needs 1015 mV) the 84 C cap takes the ceiling to 1683 MHz.
TEST(SwitchGovernor, ThermalCapCeiling)
{
    const std::pair<int64_t, std::pair<uint32_t, uint32_t>> cases[] = {{1659, {1785000, 1785000}},
                                                                       {1450, {1785000, 1683000}}};
    for (const auto &[speedo, ceilings] : cases) {
        const Table t = cpuTable(speedo);
        unsigned got[2];
        for (int c = 0; c < 2; ++c)
            got[c] = gov_index_at_mv(t.mv.data(), (unsigned)t.mv.size(), kCpuCapsMv[c]);
        EXPECT_EQ(t.khz[got[0]], ceilings.first) << speedo;
        EXPECT_EQ(t.khz[got[1]], ceilings.second) << speedo;
        for (int c = 0; c < 2; ++c) {
            EXPECT_LE(t.mv[got[c]], kCpuCapsMv[c]) << speedo;
            if (got[c] + 1 < t.mv.size())
                EXPECT_GT(t.mv[got[c] + 1], kCpuCapsMv[c]) << speedo;
        }
    }
}

// Below 20 C the DFLL keeps VDD_CPU at 800 mV or more: the governor's floor is the highest row at or below it
// (1428 MHz on this chip), so the skipper never engages.
TEST(SwitchGovernor, ColdFloorIndex)
{
    const Table t = cpuTable(1659);
    EXPECT_EQ(t.khz[gov_index_at_mv(t.mv.data(), (unsigned)t.mv.size(), kCpuColdFloorMv)], 1428000u);
}

// The boot window (the kernel forces every window busy) runs the ceiling the limits allow, never above: the DFLL's
// thermal cap or thermalmonitord's ceiling.
TEST(SwitchGovernor, BootFloorRunsTheCeilingUnderThermalLimits)
{
    const Table t = cpuTable(1450);  // a slow chip: the 84 C cap (1010 mV) takes the ceiling to 1683 MHz
    const unsigned capHi = gov_index_at_mv(t.mv.data(), (unsigned)t.mv.size(), 1010);
    REQUIRE(t.khz[capHi] == 1683000u);
    Governor g(t.khz, 0);
    gov_limit(g.g(), 0, capHi, 0);
    EXPECT_EQ(g.trace(std::vector<uint32_t>(20, 1000)), std::vector<unsigned>(20, capHi));
}

// The boot window ends at System Boot Complete; the fallback for one that never comes is later than r8a's slow boot
// (System Boot Complete at 74.3 s of uptime, after the boot band's own 60 s bound) and bounded.
static_assert(switchperf::kBootFallbackSeconds > 75, "the fallback outlasts r8a's slow boot");
static_assert(switchperf::kBootFallbackSeconds <= 180, "the fallback is bounded");

// ------------------------------------------------------------------------------------------------------------ traces

TEST(SwitchGovernor, SaturationReachesTheCeilingInOneWindow)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    EXPECT_EQ(g.trace({1000})[0], t.khz.size() - 1);
    EXPECT_EQ(gov_counter(g.g(), 2), 1u);  // one jump
}

// A burst that starts mid-window (half busy, then saturated) is at 1785 MHz after the second window: two CPU windows
// are under one 60 Hz frame.
TEST(SwitchGovernor, BurstFromIdleReachesTheTopWithinAFrame)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const std::vector<unsigned> out = g.trace({0, 0, 500, 1000});
    EXPECT_EQ(t.khz[out[3]], kNintendoCpuMaxKhz);
    EXPECT_LE(2 * tunables().window, 16667u);
}

TEST(SwitchGovernor, IdleDropsOnlyAfterTheHold)
{
    const Table t = cpuTable();
    const unsigned top = (unsigned)t.khz.size() - 1;
    Governor g(t.khz, top);
    const size_t windows = tunables().hold / tunables().window + 3;
    const std::vector<unsigned> out = g.trace(std::vector<uint32_t>(windows, 0));
    const auto firstLow = std::find_if(out.begin(), out.end(), [&](unsigned x) {
        return x != top;
    });
    REQUIRE(firstLow != out.end());
    // the targets first fall at window 0's end; the clock follows at the first window that ends a hold later
    const uint64_t windowsInHold = (tunables().hold + tunables().window - 1) / tunables().window;
    EXPECT_EQ((uint64_t)(firstLow - out.begin()), windowsInHold);
    EXPECT_EQ(out.back(), 0u);
    EXPECT_EQ(gov_counter(g.g(), 1), 1u);  // one step down, straight to the lowest point
}

// Busy and idle alternating faster than the hold: the clock never comes down.
TEST(SwitchGovernor, ShortDipsKeepTheClock)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const uint64_t dip = std::max<uint64_t>(1, tunables().hold / tunables().window - 1);
    std::vector<uint32_t> pattern(3, 1000);
    pattern.insert(pattern.end(), dip, 0);
    const std::vector<unsigned> out = g.trace(repeat(pattern, 20));
    for (size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(out[i], t.khz.size() - 1) << "window " << i;
    EXPECT_EQ(gov_counter(g.g(), 1), 0u);
}

TEST(SwitchGovernor, HoldDropsToTheHighestTargetItSaw)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const unsigned top = (unsigned)t.khz.size() - 1;
    g.trace({1000});
    // at 1785 MHz: 40 % busy wants 1020 MHz (952 / 0.75), 55 % wants 1326 MHz (1309 / 0.75), 10 % the floor
    const size_t windows = tunables().hold / tunables().window + 1;
    std::vector<uint32_t> loads = repeat({400, 550, 100}, windows);
    loads.resize(windows);
    const std::vector<unsigned> out = g.trace(loads, tunables().window);
    EXPECT_EQ(out.back(), indexOf(t.khz, 1326000)) << t.khz[out.back()] << " kHz";
    for (size_t i = 0; i + 1 < out.size(); ++i)
        EXPECT_EQ(out[i], top) << "window " << i;
}

// Half busy at 1785 MHz is 892 MHz of work: the lowest point that covers it at the target load.
TEST(SwitchGovernor, ProportionalPoint)
{
    const Table t = cpuTable();
    Governor g(t.khz, (unsigned)t.khz.size() - 1);
    const std::vector<unsigned> out = g.trace(std::vector<uint32_t>(tunables().hold / tunables().window + 2, 500));
    const double need = 1785000.0 * 500 / (double)tunables().target;
    uint32_t expect = 0;
    for (uint32_t k : t.khz)
        if (k >= need && (!expect || k < expect))
            expect = k;
    EXPECT_EQ(t.khz[out.back()], expect);
}

TEST(SwitchGovernor, TouchBoostIsImmediateAndExpires)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const unsigned top = (unsigned)t.khz.size() - 1;
    const uint64_t now = 10 * kMs, window = tunables().window, touch = tunables().touchHold;
    EXPECT_EQ(gov_boost(g.g(), top, now + touch, now), top);
    const size_t idle = (touch + tunables().hold) / window + 2;
    const std::vector<unsigned> out = g.trace(std::vector<uint32_t>(idle, 0), now);
    uint64_t lastTop = 0;
    for (size_t i = 0; i < idle; ++i) {
        const uint64_t at = now + (i + 1) * window;
        if (at < now + touch)
            EXPECT_EQ(out[i], top) << at;  // held for the whole boost, idle or not
        if (out[i] == top)
            lastTop = std::max(lastTop, at);
    }
    EXPECT_EQ(out.back(), 0u);
    EXPECT_GE(lastTop, now + touch);
}

TEST(SwitchGovernor, BoostsCombine)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    gov_boost(g.g(), 3, 100 * kMs, 0);
    gov_boost(g.g(), 6, 50 * kMs, 1);
    EXPECT_EQ(gov_cur(g.g()), 6u);
    const std::vector<unsigned> out = g.trace(std::vector<uint32_t>(30, 0));  // every window idle: the floor is 6 until
    for (size_t i = 0; i < out.size(); ++i)                                   // 100 ms, then the hold
        if ((i + 1) * tunables().window < 100 * kMs)
            EXPECT_GE(out[i], 6u) << "window " << i;
}

TEST(SwitchGovernor, ThermalCeilingClampsAtOnceAndLifts)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const unsigned top = (unsigned)t.khz.size() - 1;
    g.trace({1000});
    EXPECT_EQ(gov_limit(g.g(), 0, top - 1, 9 * kMs), top - 1);  // down at once, no hold
    EXPECT_EQ(g.trace(std::vector<uint32_t>(5, 1000), 9 * kMs), std::vector<unsigned>(5, top - 1));
    gov_limit(g.g(), 0, top, 60 * kMs);
    EXPECT_EQ(g.trace({1000}, 60 * kMs)[0], top);
    EXPECT_EQ(gov_counter(g.g(), 4), 1u);
}

TEST(SwitchGovernor, ColdFloorRaisesAtOnce)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const unsigned floor = indexOf(t.khz, 1428000);
    EXPECT_EQ(gov_limit(g.g(), floor, (unsigned)t.khz.size() - 1, 1), floor);
    for (unsigned x : g.trace(std::vector<uint32_t>(40, 0)))
        EXPECT_GE(x, floor);
}

// A load alternating every window between saturation and idle moves the clock at most once per hold.
TEST(SwitchGovernor, FlappingLoadIsRateLimited)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    const size_t windows = 2000;
    std::vector<uint32_t> busy;
    for (size_t i = 0; i < windows; ++i)
        busy.push_back(i % 2 == 0 ? 1000 : 0);
    g.trace(busy);
    const uint64_t elapsed = windows * tunables().window;
    EXPECT_LE(gov_counter(g.g(), 1), elapsed / tunables().hold + 1);
}

TEST(SwitchGovernor, ResidencyAddsUp)
{
    const Table t = cpuTable();
    Governor g(t.khz, 0);
    std::mt19937 random(5);
    std::uniform_int_distribution<uint32_t> load(0, 1000);
    const size_t n = 500;
    std::vector<uint32_t> busy;
    for (size_t i = 0; i < n; ++i)
        busy.push_back(load(random));
    g.trace(busy);
    uint64_t total = 0;
    for (unsigned i = 0; i < t.khz.size(); ++i)
        total += gov_residency(g.g(), i);
    EXPECT_EQ(total, n * tunables().window);
}

TEST(SwitchGovernor, FuzzLimitsAndResponsiveness)
{
    std::mt19937_64 random(1105);
    const auto below = [&](uint64_t n) {
        return std::uniform_int_distribution<uint64_t>(0, n - 1)(random);
    };
    const Table t = cpuTable();
    const unsigned count = (unsigned)t.khz.size();
    for (int trial = 0; trial < 60; ++trial) {
        Governor g(t.khz, (unsigned)below(count));
        uint64_t now = 0;
        for (int step = 0; step < 400; ++step) {
            now += tunables().window;
            const double roll = std::uniform_real_distribution<double>(0, 1)(random);
            if (roll < 0.03) {
                const unsigned hi = (unsigned)below(count);
                const unsigned lo = (unsigned)below(hi + 1);
                gov_limit(g.g(), lo, hi, now);
            } else if (roll < 0.08) {
                gov_boost(g.g(), (unsigned)below(count), now + below(2000) * kMs, now);
            }
            const uint32_t choices[] = {0, 0, 100, 300, 600, 899, 900, 1000, (uint32_t)below(1001)};
            const uint32_t busy = choices[below(9)];
            const unsigned cur = gov_step(g.g(), now, busy);
            ASSERT_GE(cur, gov_lo(g.g())) << "trial " << trial << " step " << step;
            ASSERT_LE(cur, gov_hi(g.g())) << "trial " << trial << " step " << step;
            ASSERT_LE(gov_khz(g.g(), cur), kNintendoCpuMaxKhz) << "trial " << trial << " step " << step;
            if (busy >= tunables().up)
                ASSERT_EQ(cur, gov_hi(g.g())) << "trial " << trial << " step " << step;
        }
    }
}

TEST(SwitchGovernor, TunablesAreSane)
{
    const Tunables &t = tunables();
    EXPECT_LT(t.window * 2, 16667u);  // up within a frame
    EXPECT_GE(t.hold, 4 * t.window);
    EXPECT_LT(t.target, t.up);
    EXPECT_GT(t.touchHold, t.hold);
}

}  // namespace
