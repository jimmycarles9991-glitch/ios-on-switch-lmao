// SwitchALSMath.h and SwitchBacklightMath.h, compiled for the Mac, against the references they cite.
//
// 1. Lux and gain against a port of CTCaer/switch-l4t-kernel-4.9 drivers/iio/light/bh1730.c (bh1730_get_lux,
//    bh1730_adjust_gain) with the OLED's calibration (tegra210-nx-sensor.dtsi: integration cycle 38, lux multiplier
//    3600; tegra210b01-fric.dts: opt-win-coeff, gain-coeff), on a grid of both channels at every gain: lux never below
//    Linux's and at most 4 above (Linux truncates three times, we once), never a u64 wrap inside a row; the gain step
//    equal to Linux's except where Linux would step up into a reading over the next gain's fl, and no step up that the
//    next sample undoes. Row 0's ci is hekate's Aula 30300, not the DT's 7502: every row boundary of Nintendo's Icosa
//    table (hekate's default) and of the Aula table with 30300 is continuous, and 7502 is not. The scale against the
//    datasheet's bare sensor (window loss) at a reading logged on the console, for ours and for the Icosa table.
// 5. Brightness-nits (nits * 65536) onto the panel linearly in luminance, 2047 at lMaxPanel and never below the floor,
//    which is lMinPanel; the legacy brightness mapping unchanged; the brightness the ramp keeps lands on the same panel
//    level; a logged auto-brightness ramp's targets on their nearest levels.
// 6. Why a logged auto-brightness held from 61 to 135 lux: CoreBrightness's curve (backboardd's ALSUserPreference,
//    E1 0 / L1 63.81 to E2 2000 / L2 287.62 nits above E0b 30 lux) gives the logged target at 61 lux exactly, and the
//    step to 135 lux is 0.03 in its perceptual scale (BULuminanceToPerceptual: 1 + ln(L / Lmax) / ln(Lmax / Lmin),
//    over this panel's 12.5..400 nits), a third of the brighten threshold 0.2 * 0.5 (AABC e70 * aggressivity 0x2a4).
// 7. A finger-then-flashlight scene through SwitchALS::sample's rule (a clipped reading above 1x only steps the gain
//    down, the sample after a gain change is skipped): 0 lux, then 50000, then 0 again, starting at 64x. Readings come
//    back valid within a few samples, no clipped reading is dispatched, and nothing in between under-reads.
// 8. The backlight-marketing-table read as DisplayCreateWithID reads it (int32 / 65536 into float): the calibration's
//    product range, no digital-dimming rescale (0x5e4/0x5e5 stay clear), and through
//    __DisplaySliderToLogicalBrightnessInternal the slider's ends on the panel's floor and full level.
// (2-4 and the rest of 7-8 check driver sources: als_source_test.)
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../SwitchALSMath.h"
#include "../SwitchBacklightMath.h"

namespace {
constexpr uint64_t kRefCycles = 38, kRefMultiplier = 3600, kRefMaxLux = 100000;
constexpr uint64_t kRefItimeUs = 2700 * kRefCycles;
constexpr uint64_t kRefGains[] = {1, 2, 64, 128};

struct Row {
    double rc, cv, ci;
};

const std::vector<Row> kWindowRows = {{231, 9697, 30300}, {993, 3333, 2778}, {1478, 1621, 1053}, {7500, 81, 10}};
const std::vector<Row> kDtWindow = {{231, 9697, 7502}, {993, 3333, 2778}, {1478, 1621, 1053}, {7500, 81, 10}};
const std::vector<Row> kIcosaWindow = {
    {500, 5002, 7502}, {754, 2250, 2000}, {1029, 1999, 1667}, {1373, 884, 583}, {1879, 309, 165}};  // hekate

struct Coeff {
    uint64_t cl, fl;
};

constexpr Coeff kGainCoeff[] = {{2500, 0}, {1600, 7800}, {10, 50000}, {0, 1000}};  // (cl, fl), tegra210b01-fric.dts

// bh1730_get_lux: u64 arithmetic, three truncating steps.
uint64_t linuxLux(uint64_t visible, uint64_t ir, unsigned gain)
{
    if (visible == 0)
        return 0;
    uint64_t lux = 0;
    for (const Row &w : kWindowRows) {
        if (1000 * ir / visible < (uint64_t)w.rc) {
            EXPECT_GE((uint64_t)w.cv * visible, (uint64_t)w.ci * ir)
                << "Linux would wrap in u64 here: " << visible << " " << ir;
            lux = (uint64_t)w.cv * visible - (uint64_t)w.ci * ir;
            break;
        }
    }
    lux *= 100;
    lux /= kRefGains[gain] * kRefItimeUs;
    lux = lux * kRefMultiplier / 1000;
    return std::min(lux, kRefMaxLux);
}

// bh1730_adjust_gain.
unsigned linuxGain(unsigned gain, uint64_t visible, uint64_t ir)
{
    const uint64_t highest = std::max(visible, ir);
    const Coeff c = kGainCoeff[gain];
    if (highest > c.fl && gain != 0)
        return gain - 1;
    if (highest < c.cl && gain != 3)
        return gain + 1;
    return gain;
}

// The grid: 0..63, 1.07^k for k in 64..164, and the values a logged reading and the datasheet name.
std::vector<unsigned> gridValues()
{
    std::set<unsigned> values;
    for (unsigned v = 0; v < 64; ++v)
        values.insert(v);
    for (int k = 64; k < 165; ++k)
        values.insert((unsigned)std::pow(1.07, k));
    for (unsigned v : {100u, 132u, 372u, 1000u, 65534u, 65535u})
        values.insert(v);
    return {values.begin(), values.end()};
}

// Each row's line cv - ci * r meets the next row's at the boundary r = rc (per mille), within 0.2% of its cv (the
// published tables are rounded to integers: the worst join is 1.6 in 1621).
std::vector<bool> joins(const std::vector<Row> &table)
{
    std::vector<bool> out;
    for (size_t i = 0; i + 1 < table.size(); ++i) {
        const Row &a = table[i], &b = table[i + 1];
        out.push_back(std::fabs((a.cv - a.ci * a.rc / 1000) - (b.cv - b.ci * a.rc / 1000)) <= 0.002 * a.cv);
    }
    return out;
}

double windowLux(const std::vector<Row> &table, double d0, double d1, unsigned gain = 2)
{
    for (const Row &w : table)
        if (1000 * d1 / d0 < w.rc)
            return (w.cv * d0 - w.ci * d1) * 100 * kRefMultiplier / ((double)kRefGains[gain] * kRefItimeUs * 1000);
    return 0;
}
}  // namespace

static_assert(256 - switch_als::kCycles == 0xda, "TIMING default DAh: the 38 cycles");
static_assert(switch_als::kIntegrationUS == kRefItimeUs && switch_als::kLuxMultiplier == kRefMultiplier);

// A wake: SwitchALS starts at some phase of a frame (the first frame end comes 0..1 frame later) and sets the sensor up,
// and CoreBrightness turns the display on at some point after that (the DC's restart, while the panel takes about half
// a second to wake) and gives up on a first sample 250 ms later. Whatever the phase and that point, every result is a
// whole conversion of this set-up and the next one comes in time.
TEST(SwitchALSMath, FirstSampleAfterDisplayOnInTime)
{
    using namespace switch_als;
    EXPECT_GE(kSampleUS, kConversionUS) << "a tick reads the conversion the last one started";
    for (unsigned phaseUS = 1; phaseUS <= kFrameUS; phaseUS += 500) {
        const unsigned first = phaseUS + (kFramesToFirstSample - 1) * kFrameUS;
        EXPECT_GE(first, kSetUpUS + kConversionUS) << "the first tick at phase " << phaseUS << " us";
        for (unsigned displayOnUS = 0; displayOnUS <= 600000; displayOnUS += 1000) {
            const unsigned next =
                displayOnUS < first ? first : first + ((displayOnUS - first) / kSampleUS + 1) * kSampleUS;
            EXPECT_LT(next - displayOnUS, kFirstSampleTimeoutUS) << "display-on " << displayOnUS << " us";
        }
    }
    // Seven frame ends for the first tick too would read the pre-sleep registers at an early phase.
    EXPECT_LT(1 + (kFramesPerSample - 1) * kFrameUS, kSetUpUS + kConversionUS);
    EXPECT_GE(kFirstSampleUS, kSetUpUS + kConversionUS)
        << "the timer's first tick reads a conversion the set-up started";
    EXPECT_LT(kSampleUS, kFirstSampleTimeoutUS) << "a display-on between ticks still sees one in time";
    // 11-06 (the old order): the sensor was set up at the panel's wake end, 69 ms after CoreBrightness's display-on, and
    // its first result came a set-up tick, a skipped tick and a sample tick later: 424 ms, past the timeout.
    EXPECT_GT(69000 + 3 * kSampleUS, kFirstSampleTimeoutUS);
}

TEST(SwitchALSMath, LuxAndGainAgainstBh1730)
{
    using namespace switch_als;
    const std::vector<unsigned> values = gridValues();
    size_t rows = 0, saturatedCases = 0;
    uint64_t worst = 0;
    for (unsigned g = 0; g < switch_als::kGains; ++g)
        for (unsigned v : values)
            for (unsigned i : values) {
                const unsigned next = nextGain(g, v, i);
                // A step up must hold at the next sample: the same light, read at the higher gain.
                if (next > g) {
                    const unsigned long scale = kGainFactor[next] / kGainFactor[g];
                    const unsigned v2 = v * scale > 65535 ? 65535 : (unsigned)(v * scale);
                    const unsigned i2 = i * scale > 65535 ? 65535 : (unsigned)(i * scale);
                    EXPECT_GE(nextGain(next, v2, i2), next)
                        << "a step up the next sample undoes: " << g << " " << v << " " << i;
                }
                const uint32_t ours = lux(v, i, g);
                ++rows;
                if (v >= 65535 || i >= 65535) {
                    // Clipped: the ceiling at 1x, no reading (the gain steps down) above it.
                    EXPECT_EQ(ours, g == 0 ? kRefMaxLux : 0) << g << " " << v << " " << i;
                    EXPECT_TRUE(g == 0 || next == g - 1) << g << " " << v << " " << i << " -> " << next;
                    ++saturatedCases;
                    continue;
                }
                const uint64_t reference = linuxLux(v, i, g);
                EXPECT_TRUE(reference <= ours && ours <= reference + 4)
                    << g << " " << v << " " << i << ": " << ours << " vs Linux " << reference;
                if (ours >= reference)
                    worst = std::max<uint64_t>(worst, ours - reference);
                const unsigned stepped = linuxGain(g, v, i);
                if (stepped > g) {
                    const bool held = std::max(v, i) * kRefGains[stepped] / kRefGains[g] < kGainCoeff[stepped].fl;
                    EXPECT_EQ(next, held ? stepped : g) << g << " " << v << " " << i << " (Linux " << stepped << ")";
                } else {
                    EXPECT_EQ(next, stepped) << g << " " << v << " " << i;
                }
            }
    const size_t unclipped = std::count_if(values.begin(), values.end(), [](unsigned v) {
        return v < 65535;
    });
    EXPECT_EQ(rows, 4 * values.size() * values.size());
    EXPECT_EQ(saturatedCases, 4 * (values.size() * values.size() - unclipped * unclipped));
    std::printf("SwitchALS: %zu lux/gain cases match bh1730.c (worst +%llu lux)\n", rows, (unsigned long long)worst);
    // Spot value: 64x, DATA0 1000, DATA1 100 (ratio 100 < 231, row 0): (9697000 - 3030000) * 100 * 3600 / (64 *
    // 102600 * 1000) = 365.5; Linux's 6667000 * 100 // 6566400 = 101, * 3600 // 1000 = 363.
    EXPECT_EQ(linuxLux(1000, 100, 2), 363u);
    EXPECT_EQ(lux(1000, 100, 2), 365u);
}

TEST(SwitchALSMath, WindowTablesAndScale)
{
    for (bool join : joins(kIcosaWindow))
        EXPECT_TRUE(join) << "Icosa";
    for (bool join : joins(kWindowRows))
        EXPECT_TRUE(join) << "Aula with ci0 30300";
    EXPECT_EQ(joins(kDtWindow), (std::vector<bool>{false, true, true})) << "7964 against 2691 at 231";
    // Scale: a logged uncovered reading, 64x DATA0 372 DATA1 132 (ratio 0.355), against the datasheet's own formula for
    // the bare part (ratio < 0.55: (0.795 DATA0 - 0.859 DATA1) / gain * 102.6 / ITIME ms): the optical window's factor.
    const double bare = (0.795 * 372 - 0.859 * 132) / 64;
    const double ours = windowLux(kWindowRows, 372, 132) / bare, icosa = windowLux(kIcosaWindow, 372, 132) / bare;
    EXPECT_TRUE(16.6 < ours && ours < 17.0) << ours;
    EXPECT_TRUE(16.6 < icosa && icosa < 17.0) << icosa;
    EXPECT_EQ(switch_als::lux(372, 132, 2), 47u) << "r7r logged lux 47 for ch0 372 ch1 132 at 64x";
}

namespace {
constexpr int64_t kPanel = 2047, kFloor = 64, kNitsMaxFixed = 400 << 16;
constexpr double kR7s[] = {37.7215, 47.7215, 51.0549, 84.3882, 137.9125, 103.5238};  // logged L_target values

// The panel level nearest to `nits` (16.16), as nitsToRaw must give it.
int64_t nearestLevel(int64_t nits)
{
    if (nits <= 0)
        return kFloor;
    return std::min(kPanel, std::max(kFloor, (nits * kPanel + kNitsMaxFixed / 2) / kNitsMaxFixed));
}
}  // namespace

TEST(SwitchBacklightMath, PanelModel)
{
    using namespace switch_backlight;
    EXPECT_EQ(kNitsMax, 400u << 16);
    EXPECT_EQ(kNitsMid, 140u << 16);
    EXPECT_EQ(kNitsMin, (uint32_t)std::nearbyint((double)(400 << 16) * kFloor / kPanel)) << "lMinPanel = the floor";
    EXPECT_LT(std::fabs(kNitsMin / 65536.0 - 12.506), 0.01);
    EXPECT_EQ(toRaw(0), (unsigned)kFloor) << "the legacy brightness mapping, as before";
    EXPECT_EQ(toRaw(kMax), (unsigned)kPanel);
    for (unsigned raw = 0; raw <= kPanelMax + 2; ++raw)
        if (raw >= kFloorRaw && raw <= kPanelMax)
            EXPECT_EQ(toRaw(toBrightness(raw)), raw) << "round trip";

    std::vector<int64_t> probe;
    for (double n : kR7s)
        probe.push_back((int64_t)(n * 65536));
    for (int64_t n = -65536; n < (420 << 16); n += 4099)
        probe.push_back(n);
    for (int64_t n : {int64_t{0}, kNitsMaxFixed, kNitsMaxFixed + 1, int64_t{0x7fffffff}})
        probe.push_back(n);
    std::map<int32_t, unsigned> shown;
    bool havePrevious = false;
    int32_t previousNits = 0;
    unsigned previousRaw = 0;
    for (int64_t n : probe) {
        const int32_t v = (int32_t)n;
        const unsigned raw = nitsToRaw(v), ramp = toRaw(nitsToBrightness(v)), back = rawToNits(raw);
        EXPECT_EQ(raw, nearestLevel(v)) << v;
        EXPECT_EQ(ramp, raw) << v << ": the ramp's brightness shows that level";
        EXPECT_LE(std::fabs(back - (double)raw * kNitsMaxFixed / kPanel), 1.0) << v << ": that level's nits";
        if (havePrevious && v >= previousNits)
            EXPECT_GE(raw, previousRaw) << v << ": monotonic";
        havePrevious = true;
        previousNits = v;
        previousRaw = raw;
        shown[v] = raw;
    }
    std::vector<unsigned> targets;
    for (double n : kR7s)
        targets.push_back(shown[(int32_t)(n * 65536)]);
    // 38..138 nits: panel 193..706.
    EXPECT_EQ(targets, (std::vector<unsigned>{193, 244, 261, 432, 706, 530}));
    EXPECT_EQ(shown[0], (unsigned)kFloor);
    EXPECT_EQ(shown[(int32_t)kNitsMaxFixed], (unsigned)kPanel);
    EXPECT_EQ(shown[0x7fffffff], (unsigned)kPanel);
}

namespace {
constexpr double kE1 = 0, kL1 = 63.80889892578125, kE2 = 2000, kL2 = 287.6190490722656, kE0b = 30;
constexpr double kL0a = 5.000001907348633, kL0b = 67.16605377197266;

double aabCurve(double lux)
{
    if (lux < kE0b)
        return kL0a + (kL0b - kL0a) * lux / kE0b;
    return kL1 + (kL2 - kL1) * (lux - kE1) / (kE2 - kE1);
}

double perceptual(double nits)
{
    const double low = switch_backlight::kNitsMin / 65536.0, high = 400.0;
    return 1 + std::log(std::max(nits, low) / high) / std::log(high / low);
}
}  // namespace

TEST(SwitchBacklightMath, R7vAutoBrightnessHolds)
{
    EXPECT_LT(std::fabs(aabCurve(61) - 70.6351), 5e-5) << "r7v: Begin ramp L_target=70.6351 _Esensor_trusted=61";
    const double delta = perceptual(aabCurve(135)) - perceptual(aabCurve(61));
    EXPECT_TRUE(0.025 < delta && delta < 0.04) << delta << ": well under 0.1, no ramp, as iOS does";
    const double threshold = 0.2 * 0.5;
    int brighten = -1, dim = -1;
    for (int lux = 61; lux <= 2000 && brighten < 0; ++lux)
        if (perceptual(aabCurve(lux)) - perceptual(aabCurve(61)) > threshold)
            brighten = lux;
    for (int lux = 61; lux >= 0 && dim < 0; --lux)
        if (perceptual(aabCurve(61)) - perceptual(aabCurve(lux)) > threshold)
            dim = lux;
    EXPECT_TRUE(250 < brighten && brighten < 400) << brighten;
    EXPECT_TRUE(5 < dim && dim < 25) << dim;
    std::printf("r7v at 61 lux holds from %d to %d lux (perceptual step to 135 lux %.3f)\n", dim, brighten, delta);
}

namespace {
struct Event {
    unsigned phase, tick;
    std::string kind;  // "lux", "clipped", "skip"
    unsigned value, gain;
};

// Counts the part gives for `lux` at a DATA1/DATA0 ratio of 0.37 (row 1), clipped at the ADC's top.
void counts(double lux, unsigned gain, unsigned &visible, unsigned &ir)
{
    using namespace switch_als;
    const double v =
        lux * kGainFactor[gain] * kIntegrationUS * 1000.0 / ((3333 - 2778 * 0.37) * 100.0 * kLuxMultiplier);
    visible = v > 65535 ? 65535 : (unsigned)v;
    const double i = v * 0.37;
    ir = i > 65535 ? 65535 : (unsigned)i;
}
}  // namespace

TEST(SwitchALSMath, StepsThroughSaturation)
{
    using namespace switch_als;
    const double scene[] = {0, 50000, 0};
    std::vector<Event> trace;
    unsigned gain = 2, skip = 0;  // 64x, as SwitchALS starts
    for (unsigned phase = 0; phase < 3; ++phase)
        for (unsigned tick = 0; tick < 12; ++tick) {
            if (skip) {
                --skip;
                trace.push_back({phase, tick, "skip", 0, gain});
                continue;
            }
            unsigned v, i;
            counts(scene[phase], gain, v, i);
            if (!saturated(v, i) || gain == 0)
                trace.push_back({phase, tick, "lux", lux(v, i, gain), gain});
            else
                trace.push_back({phase, tick, "clipped", 0, gain});
            const unsigned next = nextGain(gain, v, i);
            if (next != gain) {
                gain = next;
                skip = 1;
            }
        }
    std::map<unsigned, unsigned> settled;
    bool clippedInLight = false;
    for (unsigned phase = 0; phase < 3; ++phase) {
        const unsigned want = phase == 1 ? 50000 : 0;
        std::vector<Event> events;
        for (const Event &e : trace)
            if (e.phase == phase)
                events.push_back(e);
        const auto first = std::find_if(events.begin(), events.end(), [](const Event &e) {
            return e.kind == "lux";
        });
        ASSERT_NE(first, events.end()) << "phase " << phase;
        EXPECT_LE(first->tick, 4u) << "phase " << phase << ": a valid reading within four samples";
        for (const Event &e : events) {
            if (e.kind == "clipped")
                clippedInLight |= phase == 1;
            if (e.kind != "lux")
                continue;
            EXPECT_LE(std::abs((int)e.value - (int)want), (int)std::max(2u, want / 50))
                << "phase " << phase << " tick " << e.tick << ": never an under-read";
            settled[phase] = e.gain;
        }
        EXPECT_EQ(events.back().kind, "lux") << "phase " << phase << ": and it stays valid";
    }
    EXPECT_TRUE(clippedInLight) << "50000 lux clips 64x: dropped, not dispatched";
    EXPECT_EQ(settled, (std::map<unsigned, unsigned>{{0, 3}, {1, 0}, {2, 3}})) << "128x in the dark, 1x at 50000 lux";
}

namespace {
float f32(double x)
{
    return (float)x;
}

float interpolate(double x0, double y0, double x1, double y1, double x)
{
    return f32(y0 + (x - x0) * (y1 - y0) / (x1 - x0));
}
}  // namespace

TEST(SwitchBacklightMath, MilliNitsAsThermalmonitordReadsThem)
{
    using namespace switch_backlight;
    EXPECT_EQ(kMilliNitsMax, 400000u);
    EXPECT_EQ(rawToMilliNits(kPanelMax), kMilliNitsMax);
    EXPECT_EQ(rawToMilliNits(0), 0u);
    EXPECT_NEAR(rawToMilliNits(kFloorRaw), 12506.0, 1.0);
    unsigned previous = 0;
    for (unsigned raw = 0; raw <= kPanelMax; ++raw) {
        const uint32_t milli = rawToMilliNits(raw);
        EXPECT_LE(std::fabs(milli - rawToNits(raw) * 1000.0 / 65536), 0.5) << raw;
        EXPECT_GE(milli, previous) << raw;
        EXPECT_LE(milli, (uint32_t)INT32_MAX) << "a CFNumber SInt32";
        // thermalmonitord's percentage: (value * 100 + max / 2) / max
        EXPECT_LE((milli * 100 + kMilliNitsMax / 2) / kMilliNitsMax, 100u);
        previous = milli;
    }
}

TEST(SwitchBacklightMath, MarketingTableAsDisplayCreateWithIDReadsIt)
{
    using namespace switch_backlight;
    // CFDataGetLength / 4 entries, two or more, little-endian int32 (ldr w8 / scvtf).
    static_assert(sizeof(kMarketingTable) % 4 == 0 && sizeof(kMarketingTable) / 4 >= 2);
    const size_t entries = sizeof(kMarketingTable) / 4;
    std::vector<int32_t> table(entries);
    std::memcpy(table.data(), kMarketingTable, sizeof(kMarketingTable));
    EXPECT_EQ(table, (std::vector<int32_t>{(int32_t)kNitsMin, (int32_t)kNitsMid, (int32_t)kNitsMax}))
        << "the calibration's product range: min, mid, max";
    std::vector<float> marketing;  // scvtf s0, w8; fdiv by 65536.0 (0x1b62ae128..0x1b62ae138)
    for (int32_t v : table)
        marketing.push_back(f32((double)f32(v) / 65536));
    for (size_t i = 0; i + 1 < marketing.size(); ++i)
        EXPECT_LT(marketing[i], marketing[i + 1]);
    // display+0x50c/0x510/0x514 = lMax/Mid/MinProduct, 0x518/0x51c/0x520 = lMax/Mid/MinPanel (0x1b62ad334..0x1b62ad460).
    const float product[] = {f32(kNitsMax / 65536.0), f32(kNitsMid / 65536.0), f32(kNitsMin / 65536.0)};
    const float *panel = product;  // calibration(): the same values
    // digital-dimming-min-nits / user-accessible-max-nits absent: their defaults are the panel's min and max, through
    // fcvtzu(x * 65536) / 65536 (0x1b62adf64..0x1b62ae00c); the rescale loops run only if they come out lower.
    for (float limit : {panel[2], panel[0]})
        EXPECT_EQ(f32((double)(int64_t)f32((double)limit * 65536.0) / 65536.0), limit) << "no rescale";
    // __DisplaySliderToLogicalBrightnessInternal, 0x1b62b29a0..0x1b62b2b74.
    const auto sliderToNits = [&](double slider) {
        const float position = f32(slider * (double)(marketing.size() - 1));
        const size_t index = (size_t)position;
        const float m = index < marketing.size() - 1 ? interpolate((double)index, marketing[index], (double)index + 1,
                                                                   marketing[index + 1], position)
                                                     : marketing.back();
        if (m < product[1])
            return interpolate(product[2], panel[2], product[1], panel[1], m);
        return interpolate(product[1], panel[1], product[0], panel[0], m);
    };
    std::vector<float> curve;
    for (int i = 0; i <= 100; ++i)
        curve.push_back(sliderToNits(i / 100.0));
    for (size_t i = 0; i + 1 < curve.size(); ++i)
        EXPECT_LE(curve[i], curve[i + 1]) << "monotonic at " << i;
    EXPECT_EQ(curve[0], panel[2]);
    EXPECT_EQ(curve[50], panel[1]);
    EXPECT_EQ(curve[100], panel[0]);
    // nitsToRaw (PanelModel): the slider's bottom is the panel's floor, its top full.
    EXPECT_EQ(nearestLevel((int64_t)((double)curve[0] * 65536)), kFloor);
    EXPECT_EQ(nearestLevel((int64_t)((double)curve[100] * 65536)), kPanel);
    EXPECT_LT(std::fabs(curve[0] - 12.506), 0.01);
    EXPECT_EQ(curve[100], 400.0f);
}
