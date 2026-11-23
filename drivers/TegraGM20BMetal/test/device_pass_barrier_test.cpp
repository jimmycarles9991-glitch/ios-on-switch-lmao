// SPDX-License-Identifier: MIT
// Rule barrier.pass_pixel on gm20b::Device (pass_hazard.h): a pass whose recording says it samples a small
// block-linear target the pass before rendered opens with PIXEL_SHADER_BARRIER and no drain; one sampling a 1280x720
// target, and one the encode never described, open with today's; a memoryless colour's backing is no dependency.
#include "device_harness.h"

namespace {

constexpr uint32_t kBgra8 = 80;
constexpr uint32_t kPixelShaderBarrier = 0x0de0, kWaitForIdle = 0x0110, kShaderCaches = 0x021c,
                   kTextureDataNoWfi = 0x1288, kHeaderCache = 0x1428, kSamplerCache = 0x1424,
                   kShaderCachesNoWfi = 0x0da4;

// The 3D subchannel's methods of a job's words (IMMD, INC, NON_INC and ONE_INC), in order
std::vector<uint32_t> methods(const std::vector<uint32_t> &words)
{
    std::vector<uint32_t> out;
    for (size_t at = 0; at < words.size();) {
        const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (!h)
            continue;
        if (form == 4) {
            if (subc == 0)
                out.push_back(method);
            continue;
        }
        for (uint32_t i = 0; i < count && at < words.size(); i++, at++)
            if (subc == 0)
                out.push_back(form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0));
    }
    return out;
}

class DevicePassBarrier : public harness::DeviceTest {
protected:
    gm20b::Texture small, atlas, big, half;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, small, false, true));
        ASSERT_TRUE(small.blockLinear);
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, atlas));
        ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, big));
        ASSERT_TRUE(dev.createTexture(640, 360, kBgra8, half));
    }

    static gm20b::PassDesc cleared(gm20b::Texture &t)
    {
        gm20b::PassDesc pass;
        pass.color[0].texture = &t;
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.count = 1;
        return pass;
    }

    // Pass A clears `first`; pass B clears `second`, its recording sampling `sampled` where `described`: the PIXEL_SHADER_
    // BARRIERs and WAIT_FOR_IDLEs of the job
    std::pair<size_t, size_t> twoPasses(gm20b::Texture &first, gm20b::Texture &second, const gm20b::Texture *sampled,
                                        bool described)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        gm20b::RenderEncoder *a = cb->render(cleared(first));
        gm20b::PassUses uses;
        uses.known = true;
        a->plan(uses);
        a->end();
        gm20b::RenderEncoder *b = cb->render(cleared(second));
        if (sampled)
            uses.sample(gm20b::hazardImage(*sampled), false);
        if (described)
            b->plan(uses);
        b->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
        return {size_t(std::count(m.begin(), m.end(), kPixelShaderBarrier)),
                size_t(std::count(m.begin(), m.end(), kWaitForIdle))};
    }
};

TEST_F(DevicePassBarrier, ASmallTargetSampledByTheNextPassTakesThePixelBarrier)
{
    const uint64_t counted = dev.stats.passPixelOpenings.load();
    const auto drained = twoPasses(small, atlas, &small, false);
    EXPECT_EQ(dev.stats.passPixelOpenings.load(), counted) << "a drained opening was counted";
    const auto relaxed = twoPasses(small, atlas, &small, true);
    EXPECT_EQ(drained.first, 0u);
    EXPECT_EQ(relaxed.first, 1u);
    EXPECT_EQ(relaxed.second + 1, drained.second) << "the drain between the passes is gone";
    EXPECT_EQ(dev.stats.passPixelOpenings.load(), counted + 1);
}

// switchMetalCounters' pass_pixel_openings over a frame whose passes share one memoryless colour (each pass's scratch):
// every pass but the first opens with PIXEL_SHADER_BARRIER, and the counter moves once for each
TEST_F(DevicePassBarrier, PassesSharingAMemorylessAttachmentAreCountedAsPixelOpenings)
{
    constexpr uint32_t kPasses = 6, kRgba16f = 115;
    std::vector<gm20b::Texture> targets(kPasses);
    gm20b::Texture scratch;
    for (gm20b::Texture &t : targets)
        ASSERT_TRUE(dev.createTexture(722, 1282, kBgra8, t, false, true));
    ASSERT_TRUE(dev.createMemoryless(722, 1282, kRgba16f, scratch));
    const uint64_t counted = dev.stats.passPixelOpenings.load();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    for (uint32_t p = 0; p < kPasses; p++) {
        gm20b::PassDesc pass;
        pass.color[0].texture = &targets[p];
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.color[1].texture = &scratch;
        pass.color[1].load = gm20b::ColorAttachment::Clear;
        pass.color[1].store = false;
        pass.count = 2;
        gm20b::RenderEncoder *e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        gm20b::PassUses uses;
        uses.known = true;
        e->plan(uses);
        e->end();
    }
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), long(kPasses - 1));
    EXPECT_EQ(dev.stats.passPixelOpenings.load() - counted, uint64_t(kPasses - 1));
}

// Rule barrier.entry_no_drain: a job opens with the planner's invalidates, its first pass with nothing, no drain at all
TEST_F(DevicePassBarrier, AJobOpensWithThePlannersInvalidatesAndNoDrain)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->render(cleared(atlas))->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kWaitForIdle), 0);
    EXPECT_EQ(std::count(m.begin(), m.end(), kShaderCaches), 0) << "the waiting shader-cache flush";
    EXPECT_GE(std::count(m.begin(), m.end(), kTextureDataNoWfi), 1);
}

// gpu.x1.job_boundary_idle's arms on the job they run: one pass after the entry. entry_none's job opens with no invalidate
// at all (only the INSTRUCTION word commit may set), the planner's (the rule's) with the planner's invalidates; neither
// drains, and the first pass adds nothing to either
TEST_F(DevicePassBarrier, EachEntryArmsJobOpensWithExactlyItsEntry)
{
    struct Arm {
        gm20b::EntryForm form;
        std::vector<uint32_t> want;
    };

    const Arm arms[] = {
        {gm20b::EntryForm::None, {kShaderCachesNoWfi}},
        {gm20b::EntryForm::Rule, {kSamplerCache, kHeaderCache, kShaderCachesNoWfi, kTextureDataNoWfi}},
        {gm20b::EntryForm::Planner, {kSamplerCache, kHeaderCache, kShaderCachesNoWfi, kTextureDataNoWfi}},
    };
    const std::vector<uint32_t> barriers = {kWaitForIdle, kShaderCaches, kShaderCachesNoWfi,  kTextureDataNoWfi,
                                            kHeaderCache, kSamplerCache, kPixelShaderBarrier, 0x1338};
    for (const Arm &arm : arms) {
        dev.nextForms = {};
        dev.nextForms.entry = arm.form;
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        cb->render(cleared(atlas))->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> got;
        for (uint32_t m : methods(kernel_model::model.jobs.back().words))
            if (std::find(barriers.begin(), barriers.end(), m) != barriers.end())
                got.push_back(m);
        EXPECT_EQ(got, arm.want) << int(arm.form);
    }
}

// The header cell's rewrite: texture a's header slot holds b's header words, then its own again
TEST_F(DevicePassBarrier, ARetargetedHeaderDescribesTheOtherImageUntilRestored)
{
    std::vector<uint32_t> own(dev.headerWords(small.tic), dev.headerWords(small.tic) + 8);
    const std::vector<uint32_t> other(dev.headerWords(big.tic), dev.headerWords(big.tic) + 8);
    ASSERT_NE(own, other);
    ASSERT_TRUE(dev.probeRetarget(small, &big));
    EXPECT_EQ(std::vector<uint32_t>(dev.headerWords(small.tic), dev.headerWords(small.tic) + 8), other);
    ASSERT_TRUE(dev.probeRetarget(small, nullptr));
    EXPECT_EQ(std::vector<uint32_t>(dev.headerWords(small.tic), dev.headerWords(small.tic) + 8), own);
    gm20b::Texture none;
    EXPECT_FALSE(dev.probeRetarget(none, &big));
}

// A blit encoder that sends nothing changes no engine: the pass after it is ordered after the pass before it as if it were
// not there, never by the NO_WFI invalidates of a change from the copy engine that did not happen
TEST_F(DevicePassBarrier, AnEmptyBlitEncoderChangesNoEngine)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassUses uses;
    uses.known = true;
    gm20b::RenderEncoder *a = cb->render(cleared(small));
    a->plan(uses);
    a->end();
    cb->blit()->end();
    gm20b::RenderEncoder *b = cb->render(cleared(atlas));
    uses.sample(gm20b::hazardImage(small), false);
    b->plan(uses);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), 1) << "pass B's opening, the rule's held cell";
}

// A storage form reads its target through the SMs' data cache, which the pixel barrier leaves: its pass keeps the drain,
// and so does the pass after it
TEST_F(DevicePassBarrier, APassOfStorageFormsKeepsTheDrain)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassUses uses;
    uses.known = true;
    gm20b::RenderEncoder *a = cb->render(cleared(small));
    a->plan(uses);
    a->end();
    gm20b::PassDesc storage = cleared(atlas);
    storage.storageMask = 1;
    gm20b::RenderEncoder *b = cb->render(storage);
    b->plan(uses);
    b->end();
    gm20b::RenderEncoder *c = cb->render(cleared(half));
    c->plan(uses);
    c->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), 0);
}

// A Core Animation scale frame (boot 21: 15 passes, each a BGRA8 target of its own and a memoryless RGBA16F c1, four of
// 55 textures sampled a pass): the passes open with PIXEL_SHADER_BARRIER but where one samples the full-size target a pass
// since the last drain rendered (a glass backdrop), which no held cell covers and keeps the drain
TEST_F(DevicePassBarrier, ACompositorFrameDrainsOnlyWhereAPassReadsAnEarlierOnesTarget)
{
    constexpr uint32_t kPasses = 15, kTextures = 55;
    constexpr uint32_t kRgba16f = 115;
    std::vector<gm20b::Texture> targets(kPasses), textures(kTextures), scratch(kPasses);
    for (gm20b::Texture &t : targets)
        ASSERT_TRUE(dev.createTexture(722, 1282, kBgra8, t, false, true));
    for (gm20b::Texture &t : textures)
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, t, false, true));
    for (gm20b::Texture &t : scratch)
        ASSERT_TRUE(dev.createMemoryless(722, 1282, kRgba16f, t));
    const auto backdrop = [](uint32_t p) {
        return p == 5 || p == 9 || p == 13 ? int(p) - 2 : -1;
    };
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    for (uint32_t p = 0; p < kPasses; p++) {
        gm20b::PassDesc pass;
        pass.color[0].texture = &targets[p];
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.color[1].texture = &scratch[p];
        pass.color[1].load = gm20b::ColorAttachment::Clear;
        pass.color[1].store = false;
        pass.count = 2;
        gm20b::RenderEncoder *e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        gm20b::PassUses uses;
        uses.known = true;
        for (uint32_t t = 0; t < 4; t++)
            uses.sample(gm20b::hazardImage(textures[(p * 4 + t) % kTextures]), false);
        if (backdrop(p) >= 0)
            uses.sample(gm20b::hazardImage(targets[size_t(backdrop(p))]), false);
        e->plan(uses);
        e->end();
    }
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), long(kPasses - 1 - 3));
    EXPECT_EQ(std::count(m.begin(), m.end(), kWaitForIdle), 3) << "the three passes that sample a backdrop";
}

TEST_F(DevicePassBarrier, AFullSizeTargetKeepsTheDrain)
{
    const auto read = twoPasses(big, half, &big, true);
    EXPECT_EQ(read.first, 0u);
    const auto unrelated = twoPasses(big, half, nullptr, true);
    EXPECT_EQ(unrelated.first, 1u) << "nothing of the pass before is read or written";
}

}  // namespace
