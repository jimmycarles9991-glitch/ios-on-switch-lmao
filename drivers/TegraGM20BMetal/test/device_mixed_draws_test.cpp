// SPDX-License-Identifier: MIT
// How each state's draws keep Apple's colour rule in a mixed pass on gm20b::Device (mixed_samples.h MixedDraw): a
// four-sample state's sample 0 and samples 1-3 drawn apart where it writes colour and depth or stencil, on every draw
// path, samples 1-3 with the pixel stage off where its program shapes no coverage, never its fragment program's memory
// writes twice, and no draw skipped.
#include "device_forms.h"
#include "../depth_stencil_state.h"
#include "../icb.h"
#include "../mme.h"
#include <functional>
#include <tuple>

namespace {

constexpr uint32_t kTriangle = 3;

gm20b::DepthStencilDesc depthWrites()
{
    gm20b::DepthStencilDesc d;
    d.depthCompare = 1;  // Less
    d.depthWrite = true;
    return d;
}

// A record of `keyString` whose state writes `ctWrite` to colour 0, its fragment program storing to memory where `stores`
gm20b::RuntimeRecord record(uint64_t key, const char *keyString, uint32_t ctWrite, bool stores, bool kills = false)
{
    gm20b::RuntimeRecord r;
    r.keyString = keyString;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = std::string("fs") + keyString + (stores ? "stores" : "") + (kills ? "kills" : "");
    if (kills) {
        constexpr uint32_t kKillsPixels = 1u << 15;  // SPH word 0 KILLS_PIXELS
        uint32_t word0 = 0;
        memcpy(&word0, r.programs[1].image.data(), sizeof word0);
        word0 |= kKillsPixels;
        memcpy(r.programs[1].image.data(), &word0, sizeof word0);
    }
    if (stores) {
        constexpr uint64_t kStg = uint64_t(0xeed8) << 48;  // program_stores.h STG, past the group's scheduling word
        memcpy(r.programs[1].image.data() + 80 + 8, &kStg, sizeof kStg);
    }
    r.pipeline.keyHash = key;
    r.blob = {NVB197_SET_CT_WRITE(0), ctWrite};
    return r;
}

gm20b::Pipeline *recordState(gm20b::Device &dev, uint64_t key, const char *keyString, uint32_t ctWrite, bool stores,
                             bool kills = false)
{
    std::string why;
    gm20b::Pipeline *p = dev.addRecord(record(key, keyString, ctWrite, stores, kills), false, why);
    EXPECT_NE(p, nullptr) << why;
    return p;
}

// One pass of a one-sample colour beside a four-sample depth, both cleared, `body` encoding into it: what it submitted,
// empty with `why` where it was refused
struct MixedPass {
    gm20b::Device &dev;
    gm20b::Texture colour, zeta;
    gm20b::CommandBuffer *cb = nullptr;
    std::string why;

    explicit MixedPass(gm20b::Device &device) : dev(device)
    {
        EXPECT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    }

    template <class Body> std::vector<Method> run(Body body)
    {
        cb = dev.begin();
        if (!cb) {
            why = "no command buffer";
            return {};
        }
        const size_t first = kernel_model::model.jobs.size();
        gm20b::PassDesc p;
        p.color[0].texture = &colour;
        p.color[0].load = gm20b::ColorAttachment::Clear;
        p.color[0].store = true;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        const char *rule = nullptr;
        gm20b::RenderEncoder *e = cb->render(p, &rule);
        if (!e) {
            cb->abandon();
            why = rule ? rule : "no pass";
            return {};
        }
        const bool ok = body(*e, &rule);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            why = rule ? rule : "not committed";
            return {};
        }
        return submitted(first);
    }
};

struct Step {
    // 'M' a sample mask, 'C' colour target 0's CT_WRITE, 'P' the pixel stage's switch (1 on), 'D' a draw, 'F' a fetching
    // state's ordered draw
    char kind;
    uint32_t value;
};

// What a mixed pass sends between the mode's enable and its disable, in order
std::vector<Step> modeSteps(const std::vector<Method> &ms)
{
    using namespace gm20b::mme;
    std::vector<Step> out;
    bool on = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_TIR)
            on = x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M;
        if (!on)
            continue;
        if (x.method == NVB197_SET_SAMPLE_MASK_X0_Y0)
            out.push_back({'M', x.value});
        else if (x.method == NVB197_SET_CT_WRITE(0))
            out.push_back({'C', x.value});
        else if (x.method == gm20b::kPixelStageSwitch)
            out.push_back({'P', x.value & 1});
        else if (x.method >= kMacroCall && (x.method - kMacroCall) % 8 == 0) {
            const uint32_t slot = (x.method - kMacroCall) / 8;
            if (slot == kFetchDraw || slot == kFetchDrawIndexed)
                out.push_back({'F', 0});
            else if (countedAsDraw(slot) || slot == kIcbDraw || slot == kIcbDrawIndexed || slot == kAmplify ||
                     slot == kAmplifyIndexed)
                out.push_back({'D', 0});
        }
    }
    return out;
}

std::string stepText(const Step &s)
{
    if (s.kind != 'M' && s.kind != 'C' && s.kind != 'P')
        return std::string(1, s.kind);
    char text[16];
    snprintf(text, sizeof text, "%c%x", s.kind, s.value);
    return text;
}

// The sample masks, pixel stage switches and draws, as "M1111 D Meeee P0 D P1"
std::string masksAndDraws(const std::vector<Step> &steps)
{
    std::string out;
    for (const Step &s : steps)
        if (s.kind != 'C')
            out += (out.empty() ? "" : " ") + stepText(s);
    return out;
}

// The colour writes from samples 1-3's mask through the draw after it to the colour write after that, as "C0 D C1111"
std::string colourWritesAroundLaterSamples(const std::vector<Step> &steps)
{
    std::string out;
    auto at = std::find_if(steps.begin(), steps.end(), [](const Step &s) {
        return s.kind == 'M' && s.value == gm20b::kLaterSamplesMask;
    });
    bool drawn = false;
    for (at = at == steps.end() ? at : at + 1; at != steps.end() && at->kind != 'M'; ++at) {
        if (at->kind == 'P')
            continue;
        if (at->kind != 'C' && drawn)
            break;
        out += (out.empty() ? "" : " ") + stepText(*at);
        if (at->kind == 'C' && drawn)
            break;
        drawn |= at->kind != 'C';
    }
    return out;
}

// The mode is on with REDUCE_COVERAGE (a colour pixel where any raster sample passes) for every state: disabling it under
// RASTER_N_TARGET_M is INVALID_TIR_COMBINATION05
TEST_F(DevicePassForms, AMixedPassKeepsReduceCoverageOnForEveryState)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    MixedPass m(dev);
    const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
            return false;
        e.setPipeline(one);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << m.why;
    const std::vector<uint32_t> controls = values(ms, NVB197_SET_TIR_CONTROL);
    ASSERT_GE(controls.size(), 1u);
    for (const uint32_t c : controls)
        EXPECT_EQ(c, uint32_t(NVB197_SET_TIR_CONTROL_REDUCE_COVERAGE_ENABLE << 1));
}

// A four-sample state writing colour and depth keeps Apple's colour of sample 0 by two draws: sample 0's, then samples
// 1-3's with colour writes off (the colour targets' CT_WRITE back after). A one-sample state after it draws once over
// every sample.
TEST_F(DevicePassForms, AFourSampleStateWritingDepthDrawsSampleZeroThenTheRestWithoutColour)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass m(dev);
    const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        e.setDepthStencil(&writes);
        if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
            return false;
        e.setPipeline(one);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << m.why;
    const std::vector<Step> steps = modeSteps(ms);
    EXPECT_EQ(masksAndDraws(steps), "M1111 D Meeee P0 D P1 Mffff D");
    EXPECT_EQ(colourWritesAroundLaterSamples(steps), "C0 D C1111");
}

// A four-sample state that writes no depth or stencil leaves samples 1-3 as they were: sample 0's draw alone. One that
// writes no colour draws every sample at once.
TEST_F(DevicePassForms, AFourSampleStateWritingNoDepthOrNoColourDrawsOnce)
{
    gm20b::Pipeline *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(four, nullptr);
    MixedPass noDepth(dev);
    const auto ms = noDepth.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << noDepth.why;
    EXPECT_EQ(masksAndDraws(modeSteps(ms)), "M1111 D");

    gm20b::Pipeline *depthOnly = recordState(dev, 0xb5, "samples=4 c0=70 ds=252/0 ", 0, false);
    ASSERT_NE(depthOnly, nullptr);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass noColour(dev);
    const auto prepass = noColour.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(depthOnly);
        e.setDepthStencil(&writes);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(prepass.empty()) << noColour.why;
    const std::string drawn = masksAndDraws(modeSteps(prepass));
    EXPECT_TRUE(drawn == "D" || drawn == "Mffff D") << drawn;
}

// A four-sample state whose fragment program writes memory, and writes colour and depth: samples 1-3 draw with the pixel
// stage off, so its memory writes run once and nothing is skipped. Writing no depth, it is drawn once at sample 0.
TEST_F(DevicePassForms, AFourSampleStateThatStoresRunsItsProgramOnce)
{
    gm20b::Pipeline *stores = recordState(dev, 0xb6, "samples=4 c0=70 ds=252/0 ", 0x1111, true);
    ASSERT_NE(stores, nullptr);
    ASSERT_TRUE(stores->writesMemory[1]);
    ASSERT_FALSE(stores->shapesCoverage);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass split(dev);
    uint32_t skips = 0;
    const auto ms = split.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(stores);
        e.setDepthStencil(&writes);
        const bool done = e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        skips = split.cb->skips;
        return done;
    });
    ASSERT_FALSE(ms.empty()) << split.why;
    EXPECT_EQ(skips, 0u);
    EXPECT_EQ(masksAndDraws(modeSteps(ms)), "M1111 D Meeee P0 D P1");

    MixedPass once(dev);
    const auto drawn = once.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(stores);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(drawn.empty()) << once.why;
    EXPECT_EQ(masksAndDraws(modeSteps(drawn)), "M1111 D");
}

// A program that shapes coverage (here it discards) decides what samples 1-3 write, so they run it again with colour
// writes off; one that also writes memory cannot run twice and draws every sample at once instead of being skipped
TEST_F(DevicePassForms, AFourSampleStateShapingCoverageRunsItsProgramForSamplesOneToThreeOrDrawsOnce)
{
    gm20b::Pipeline *kills = recordState(dev, 0xb8, "samples=4 c0=70 ds=252/0 ", 0x1111, false, true);
    gm20b::Pipeline *killsAndStores = recordState(dev, 0xb9, "samples=4 c0=70 ds=252/0 ", 0x1111, true, true);
    ASSERT_NE(kills, nullptr);
    ASSERT_NE(killsAndStores, nullptr);
    ASSERT_TRUE(kills->shapesCoverage);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass shaded(dev);
    const auto ms = shaded.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(kills);
        e.setDepthStencil(&writes);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << shaded.why;
    const std::vector<Step> steps = modeSteps(ms);
    EXPECT_EQ(masksAndDraws(steps), "M1111 D Meeee D");
    EXPECT_EQ(colourWritesAroundLaterSamples(steps), "C0 D C1111");

    MixedPass covering(dev);
    uint32_t skips = 0;
    const auto once = covering.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(killsAndStores);
        e.setDepthStencil(&writes);
        const bool done = e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        skips = covering.cb->skips;
        return done;
    });
    ASSERT_FALSE(once.empty()) << covering.why;
    EXPECT_EQ(skips, 0u);
    // every sample at once: the mode starts with the whole mask, so no mask is written
    EXPECT_EQ(masksAndDraws(modeSteps(once)), "D");
}

// Every way a draw reaches the GPU splits the same: indexed, indirect, indexed indirect, an indirect command buffer's
// commands, and a fetching state's ordered draw, its samples 1-3 drawn whole
TEST_F(DevicePassForms, EveryDrawPathSplitsAFourSampleStateWritingDepth)
{
    gm20b::Pipeline *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(four, nullptr);
    gm20b::RuntimeRecord fetching = record(0xb7, "samples=4 c0=70 ds=252/0 ", 0x1111, false);
    gm20b::setEntryCount(fetching.pipeline, 1);
    fetching.pipeline.entry[0] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    fetching.pipeline.flags = gm20b::PipelineFetches;
    std::string added;
    gm20b::Pipeline *fetches = dev.addRecord(fetching, false, added);
    ASSERT_NE(fetches, nullptr) << added;
    gm20b::Buffer indices, arguments, slots;
    ASSERT_TRUE(dev.createBuffer(256, indices));
    ASSERT_TRUE(dev.createBuffer(256, arguments));
    ASSERT_TRUE(dev.createBuffer(gm20b::icb::kSlotWords * 4, slots));
    const uint16_t strip[4] = {0, 1, 2, 3};
    memcpy(indices.cpu, strip, sizeof strip);
    const uint32_t direct[5] = {4, 1, 0, 0, 0};
    memcpy(arguments.cpu, direct, sizeof direct);
    gm20b::icb::draw(reinterpret_cast<uint32_t *>(slots.cpu), 0, {kTriangleStrip, 4, 1, 0, 0});
    const gm20b::DepthStencilState writes(depthWrites());
    using Draw = std::function<bool(gm20b::RenderEncoder &, const char **)>;
    const std::vector<std::tuple<const char *, gm20b::Pipeline *, Draw, std::string>> paths = {
        {"indexed", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndexed(kTriangleStrip, 4, 2, &indices, 0, 1, 0, 0, why);
         },
         "M1111 D Meeee P0 D P1"},
        {"indirect", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndirect(kTriangleStrip, &arguments, 0, why);
         },
         "M1111 D Meeee P0 D P1"},
        {"indexed indirect", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndexedIndirect(kTriangleStrip, 2, &indices, 0, &arguments, 0, why);
         },
         "M1111 D Meeee P0 D P1"},
        {"commands", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.executeSlots(
                 {&slots, 0, 1, gm20b::icb::kSlotWords, nullptr, 0, kTriangleStrip, false, false, nullptr, 0}, why);
         },
         "M1111 D Meeee P0 D P1"},
        {"ordered fetch", fetches,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.draw(kTriangle, 0, 6, 1, 0, why);
         },
         "M1111 F Meeee P0 D P1"},
    };
    for (const auto &path : paths) {
        SCOPED_TRACE(std::get<0>(path));
        gm20b::Pipeline *pl = std::get<1>(path);
        const Draw &draw = std::get<2>(path);
        MixedPass m(dev);
        const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
            e.setPipeline(pl);
            e.setDepthStencil(&writes);
            return draw(e, refused);
        });
        ASSERT_FALSE(ms.empty()) << m.why;
        const std::vector<Step> steps = modeSteps(ms);
        EXPECT_EQ(masksAndDraws(steps), std::get<3>(path));
        EXPECT_EQ(colourWritesAroundLaterSamples(steps), "C0 D C1111");
    }
}

// HW Test gpu.gm20b.mixed_merge_probe's rows (Device::probeTir kTirProbeWhole): a state the driver would split is drawn
// once, under the whole mask prepare chose for it (written where the held mask was another), so the control row
// measures one draw
TEST_F(DevicePassForms, AProbeRowAskingWholeDrawsASplitStateOnceUnderTheWholeMask)
{
    if (!SWITCH_METAL_DIAGNOSTICS)
        GTEST_SKIP() << "the probe rows are debug builds'";
    gm20b::Pipeline *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    gm20b::Pipeline *kills = recordState(dev, 0xb8, "samples=4 c0=70 ds=252/0 ", 0x1111, false, true);
    ASSERT_NE(four, nullptr);
    ASSERT_NE(kills, nullptr);
    gm20b::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(256, arguments));
    const uint32_t direct[4] = {4, 1, 0, 0};
    memcpy(arguments.cpu, direct, sizeof direct);
    const gm20b::DepthStencilState writes(depthWrites());
    dev.probeTir.store(1u | gm20b::kTirProbeWhole);
    for (gm20b::Pipeline *pl : {four, kills}) {
        for (const bool indirect : {false, true}) {
            SCOPED_TRACE(std::string(pl == four ? "split" : "split shaded") + (indirect ? ", indirect" : ""));
            MixedPass m(dev);
            const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
                e.setPipeline(pl);
                e.setDepthStencil(&writes);
                return indirect ? e.drawIndirect(kTriangleStrip, &arguments, 0, refused)
                                : e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
            });
            ASSERT_FALSE(ms.empty()) << m.why;
            const std::string drawn = masksAndDraws(modeSteps(ms));
            EXPECT_TRUE(drawn == "D" || drawn == "Mffff D") << drawn;
        }
    }
    dev.probeTir.store(0);
}

}  // namespace
