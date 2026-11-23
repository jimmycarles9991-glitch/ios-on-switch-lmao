// SPDX-License-Identifier: MIT
// Draws Metal's validation layer would stop, drawn on gm20b::Device as Apple's driver draws them (client_errors.h). What
// Apple did, measured on an M-series Mac with API validation off, each command buffer completed with no error:
//  - RGBA8Unorm state, BGRA8Unorm pass: texel 99 66 33 cc for (0.2, 0.4, 0.6, 0.8), the right colour
//  - RGBA16Float state, BGRA8Unorm pass: texel 66 32 66 36, the outputs' half-float bits: no colour to match, skipped
//  - one-sample state, four-sample pass: resolved 26 1a 0d 33, sample 0 alone; four-sample state, one-sample pass: the
//    whole colour
//  - drawPrimitives:indirectBuffer: 4 bytes into a 16-byte buffer: drawn from the three words inside and a zero after
#include "device_harness.h"
#include "../record.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kBGRA8 = 80, kRGBA16F = 115, kTriangle = 3;

gm20b::RuntimeRecord plainRecord(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0:1:0:0:1:0:0 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceClientErrors : public harness::DeviceTest {
protected:
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        std::string why;
        pl = dev.addRecord(plainRecord(0xc11e), false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    struct Drawn {
        uint32_t draws = 0, skips = 0;
        gm20b::Refusal skipped;
        std::vector<uint32_t> sampleMasks;  // the SET_SAMPLE_MASK_X0_Y0..X1_Y1 values written, in order
        std::vector<TegraGM20BAbiPush> pushes;
    };

    // One command buffer of one pass into `target` drawing `pl` once, through `draw`
    template <class Draw> Drawn drawn(gm20b::Texture &target, Draw draw)
    {
        const size_t before = kernel_model::model.jobs.size();
        Drawn out;
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return out;
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = gm20b::ColorAttachment::Load;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        EXPECT_NE(e, nullptr);
        e->setPipeline(pl);
        const char *why = nullptr;
        EXPECT_TRUE(draw(*e, &why)) << (why ? why : "");
        e->end();
        out.draws = cb->draws;
        out.skips = cb->skips;
        out.skipped = cb->skipped;
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
            const kernel_model::Job &job = kernel_model::model.jobs[j];
            words.insert(words.end(), job.words.begin(), job.words.end());
            out.pushes.insert(out.pushes.end(), job.pushes.begin(), job.pushes.end());
        }
        std::vector<switch_gm20b_method> m(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        for (long i = 0; i < n; i++)
            if (m[i].method >= NVB197_SET_SAMPLE_MASK_X0_Y0 && m[i].method <= NVB197_SET_SAMPLE_MASK_X1_Y1)
                out.sampleMasks.push_back(m[i].value);
        return out;
    }

    Drawn triangle(gm20b::Texture &target)
    {
        return drawn(target, [](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(kTriangle, 0, 3, 1, 0, why);
        });
    }
};

TEST_F(DeviceClientErrors, AStateOfTheOtherChannelOrderDrawsIntoThePasssFormat)
{
    gm20b::Texture bgra;
    ASSERT_TRUE(dev.createTexture(64, 64, kBGRA8, bgra));
    const Drawn d = triangle(bgra);
    EXPECT_EQ(d.draws, 1u);
    EXPECT_EQ(d.skips, 0u);
}

TEST_F(DeviceClientErrors, AStateWhoseColourAppleWritesAsOtherBitsIsSkipped)
{
    gm20b::Texture half;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA16F, half));
    const Drawn d = triangle(half);
    EXPECT_EQ(d.draws, 0u);
    EXPECT_EQ(d.skips, 1u);
    EXPECT_EQ(d.skipped.id, gm20b::RefColorFormats);
    EXPECT_EQ(d.skipped.b, kRGBA16F);
}

TEST_F(DeviceClientErrors, AOneSampleStateInAFourSamplePassWritesSampleZeroAlone)
{
    gm20b::Texture ms, one;
    ASSERT_TRUE(dev.createStoredMultisample(64, 64, kRGBA8, 4, ms));
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, one));
    const Drawn four = triangle(ms);
    EXPECT_EQ(four.draws, 1u);
    EXPECT_EQ(four.sampleMasks, (std::vector<uint32_t>{1, 1, 1, 1}));
    const Drawn back = triangle(one);
    EXPECT_EQ(back.draws, 1u);
    EXPECT_EQ(back.sampleMasks, (std::vector<uint32_t>{0xffff, 0xffff, 0xffff, 0xffff}));
    EXPECT_TRUE(triangle(one).sampleMasks.empty());
}

TEST_F(DeviceClientErrors, IndirectArgumentsPastTheirBufferReadZerosNotTheMemoryAfterIt)
{
    gm20b::Texture target;
    gm20b::Buffer arguments;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    ASSERT_TRUE(dev.createBuffer(16, arguments));
    const Drawn d = drawn(target, [&](gm20b::RenderEncoder &e, const char **why) {
        return e.drawIndirect(kTriangle, &arguments, 4, why);
    });
    EXPECT_EQ(d.draws, 1u);
    bool inside = false;
    for (const TegraGM20BAbiPush &p : d.pushes) {
        const bool fromArguments = p.va >= arguments.va && p.va < arguments.va + arguments.length;
        if (fromArguments) {
            inside = true;
            EXPECT_EQ(p.va, arguments.va + 4);
            EXPECT_EQ(p.dwords, 3u);
        }
        EXPECT_FALSE(p.va >= arguments.va + arguments.length && p.va < arguments.va + 256) << std::hex << p.va;
    }
    EXPECT_TRUE(inside);
}

TEST_F(DeviceClientErrors, IndirectArgumentsWhollyPastTheirBufferAreAllZeros)
{
    gm20b::Texture target;
    gm20b::Buffer arguments;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    ASSERT_TRUE(dev.createBuffer(16, arguments));
    const Drawn d = drawn(target, [&](gm20b::RenderEncoder &e, const char **why) {
        return e.drawIndirect(kTriangle, &arguments, 32, why);
    });
    EXPECT_EQ(d.draws, 1u);
    for (const TegraGM20BAbiPush &p : d.pushes)
        EXPECT_FALSE(p.va >= arguments.va && p.va < arguments.va + 256) << std::hex << p.va;
}

TEST(ClientErrors, ACopyMovesTheBytesInsideBothBuffers)
{
    EXPECT_EQ(gm20b::clampedCopy(4096, 0, 8192, 0, 8192), 4096u);
    EXPECT_EQ(gm20b::clampedCopy(4096, 2048, 8192, 0, 4096), 2048u);
    EXPECT_EQ(gm20b::clampedCopy(4096, 0, 8192, 6144, 4096), 2048u);
    EXPECT_EQ(gm20b::clampedCopy(4096, 5000, 8192, 0, 100), 0u);
    EXPECT_EQ(gm20b::clampedCopy(4096, 0, 4096, 1 << 20, 16), 0u);
    EXPECT_EQ(gm20b::clampedCopy(4096, 0, 4096, 0, 1 << 20), 4096u);
}

TEST(ClientErrors, SamplesDrawnAsAppleDrawsThem)
{
    EXPECT_TRUE(gm20b::samplesDrawn(1, 4, 1));
    EXPECT_TRUE(gm20b::samplesDrawn(4, 1, 1));
    EXPECT_FALSE(gm20b::samplesDrawn(4, 1, 4));
    EXPECT_FALSE(gm20b::samplesDrawn(2, 4, 1));
    EXPECT_EQ(gm20b::sampleMaskFor(1, 4), gm20b::kSampleMaskFirst);
    EXPECT_EQ(gm20b::sampleMaskFor(4, 1), gm20b::kSampleMaskAll);
    EXPECT_TRUE(gm20b::targetOrdersChannels(70, 80));
    EXPECT_TRUE(gm20b::targetOrdersChannels(81, 71));
    EXPECT_FALSE(gm20b::targetOrdersChannels(115, 80));
    EXPECT_FALSE(gm20b::targetOrdersChannels(70, 81));
}

}  // namespace
