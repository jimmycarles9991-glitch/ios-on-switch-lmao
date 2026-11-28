// SPDX-License-Identifier: MIT
// Rule barrier.pass_pixel on gm20b::Device (pass_hazard.h): a pass whose recording says it samples a small
// block-linear target the pass before rendered opens with PIXEL_SHADER_BARRIER and no drain; one sampling a 1280x720
// target, and one the encode never described, open with today's; a memoryless colour's backing is no dependency.
#include "device_harness.h"
#include "../record.h"

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
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), long(kPasses - 1));
    EXPECT_EQ(std::count(m.begin(), m.end(), kWaitForIdle), 0);
}

TEST_F(DevicePassBarrier, AFullSizeTargetTakesPixelBarrierForFsAndDrainsForVs)
{
    const auto read = twoPasses(big, half, &big, true);
    EXPECT_EQ(read.first, 1u);
    EXPECT_EQ(read.second, 0u);
    const auto unrelated = twoPasses(big, half, nullptr, true);
    EXPECT_EQ(unrelated.first, 0u);
    EXPECT_EQ(unrelated.second, 0u);

    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassUses aUses;
    aUses.known = true;
    gm20b::RenderEncoder *a = cb->render(cleared(big));
    a->plan(aUses);
    a->end();
    gm20b::PassUses bUses;
    bUses.known = true;
    bUses.sample(gm20b::hazardImage(big), true);
    gm20b::RenderEncoder *b = cb->render(cleared(half));
    b->plan(bUses);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), 0);
    EXPECT_EQ(std::count(m.begin(), m.end(), kWaitForIdle), 1);
}

TEST_F(DevicePassBarrier, EntryGatesHeaderAndSamplerInvalidatesOnDirtyPool)
{
    const auto entryInvalidates = [&]() -> std::pair<size_t, size_t> {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        cb->render(cleared(atlas))->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
        return {size_t(std::count(m.begin(), m.end(), kHeaderCache)),
                size_t(std::count(m.begin(), m.end(), kSamplerCache))};
    };
    const auto first = entryInvalidates();
    EXPECT_EQ(first.first, 1u);
    EXPECT_EQ(first.second, 1u);
    const auto clean = entryInvalidates();
    EXPECT_EQ(clean.first, 0u);
    EXPECT_EQ(clean.second, 0u);
    gm20b::Texture extra;
    ASSERT_TRUE(dev.createTexture(32, 32, kBgra8, extra));
    const auto afterTic = entryInvalidates();
    EXPECT_EQ(afterTic.first, 1u);
    EXPECT_EQ(afterTic.second, 0u);
    gm20b::Sampler sampler;
    ASSERT_TRUE(dev.createSampler({}, sampler));
    const auto afterTsc = entryInvalidates();
    EXPECT_EQ(afterTsc.first, 0u);
    EXPECT_EQ(afterTsc.second, 1u);
    dev.destroySampler(sampler);
    dev.destroyTexture(extra);
}

TEST_F(DevicePassBarrier, VolatilePitchBufferTextureTargetKeepsTheDrain)
{
    constexpr uint32_t kWidth = 320, kHeight = 180, kPitch = kWidth * 4;
    gm20b::Buffer volBuf, cachedBuf;
    gm20b::Texture volTex, cachedTex, pyramid;
    ASSERT_TRUE(dev.createBuffer(uint64_t(kPitch) * kHeight, volBuf, true, true, true));
    ASSERT_TRUE(dev.createBuffer(uint64_t(kPitch) * kHeight, cachedBuf, true, true, false));
    ASSERT_TRUE(dev.createBufferTexture(volBuf, 0, kWidth, kHeight, kPitch, kBgra8, volTex));
    ASSERT_TRUE(dev.createBufferTexture(cachedBuf, 0, kWidth, kHeight, kPitch, kBgra8, cachedTex));
    ASSERT_TRUE(dev.createTexture(160, 90, kBgra8, pyramid, false, true));
    ASSERT_TRUE(volTex.renderable && !volTex.shadowTarget && volTex.storageRange().gpuVolatile);
    ASSERT_TRUE(cachedTex.renderable && !cachedTex.shadowTarget && !cachedTex.storageRange().gpuVolatile);

    const auto cachedResult = twoPasses(cachedTex, pyramid, &cachedTex, true);
    EXPECT_EQ(cachedResult.first, 1u);
    EXPECT_EQ(cachedResult.second, 0u);

    const auto volResult = twoPasses(volTex, pyramid, &volTex, true);
    EXPECT_EQ(volResult.first, 0u);
    EXPECT_EQ(volResult.second, 1u);

    dev.destroyTexture(pyramid);
    dev.destroyTexture(cachedTex);
    dev.destroyTexture(volTex);
    dev.destroyBuffer(cachedBuf);
    dev.destroyBuffer(volBuf);
}

TEST_F(DevicePassBarrier, FallbackDrainAvoidsDoubleWfiAndDropsConstantsOnlyAfterBufferWrite)
{
    const auto shaderInvalValues = [](const std::vector<uint32_t> &words, uint32_t targetMethod) {
        std::vector<uint32_t> values;
        for (size_t at = 0; at < words.size();) {
            const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (!h)
                continue;
            if (form == 4) {
                if (subc == 0 && method == targetMethod)
                    values.push_back(count);
                continue;
            }
            for (uint32_t i = 0; i < count && at < words.size(); i++, at++) {
                const uint32_t m = form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0);
                if (subc == 0 && m == targetMethod)
                    values.push_back(words[at]);
            }
        }
        return values;
    };

    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassUses aUses;
        aUses.known = true;
        gm20b::RenderEncoder *a = cb->render(cleared(big));
        a->plan(aUses);
        a->end();
        gm20b::PassUses bUses;
        bUses.known = true;
        bUses.sample(gm20b::hazardImage(big), true);
        gm20b::RenderEncoder *b = cb->render(cleared(half));
        b->plan(bUses);
        b->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
    const std::vector<uint32_t> &ropWords = kernel_model::model.jobs.back().words;
    const std::vector<uint32_t> ropMethods = methods(ropWords);
    EXPECT_EQ(std::count(ropMethods.begin(), ropMethods.end(), kPixelShaderBarrier), 0);
    EXPECT_EQ(std::count(ropMethods.begin(), ropMethods.end(), kWaitForIdle), 1);
    EXPECT_EQ(std::count(ropMethods.begin(), ropMethods.end(), kShaderCaches), 0);
    const std::vector<uint32_t> ropNoWfi = shaderInvalValues(ropWords, kShaderCachesNoWfi);
    ASSERT_GE(ropNoWfi.size(), 2u);
    EXPECT_EQ(ropNoWfi.back(), uint32_t(NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE)));

    gm20b::Buffer ssbo;
    ASSERT_TRUE(dev.createBuffer(4096, ssbo, true));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassUses uses;
    uses.known = true;
    gm20b::RenderEncoder *a = cb->render(cleared(big));
    cb->use(ssbo, true, true);
    a->plan(uses);
    a->end();
    gm20b::RenderEncoder *b = cb->render(cleared(half));
    b->plan(uses);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> &bufWords = kernel_model::model.jobs.back().words;
    const std::vector<uint32_t> bufMethods = methods(bufWords);
    EXPECT_EQ(std::count(bufMethods.begin(), bufMethods.end(), kWaitForIdle), 0);
    EXPECT_EQ(std::count(bufMethods.begin(), bufMethods.end(), kShaderCaches), 1);
    const std::vector<uint32_t> bufNoWfi = shaderInvalValues(bufWords, kShaderCachesNoWfi);
    ASSERT_GE(bufNoWfi.size(), 2u);
    EXPECT_EQ(bufNoWfi.back(), uint32_t(NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE) |
                                        NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE)));
    dev.destroyBuffer(ssbo);
}

TEST_F(DevicePassBarrier, ShaderWritesFlushDataAndInvalidateNextPassConstants)
{
    const auto methodValues = [](const std::vector<uint32_t> &words, uint32_t targetMethod) {
        std::vector<uint32_t> values;
        for (size_t at = 0; at < words.size();) {
            const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (!h)
                continue;
            if (form == 4) {
                if (subc == 0 && method == targetMethod)
                    values.push_back(count);
                continue;
            }
            for (uint32_t i = 0; i < count && at < words.size(); i++, at++) {
                const uint32_t m = form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0);
                if (subc == 0 && m == targetMethod)
                    values.push_back(words[at]);
            }
        }
        return values;
    };
    const uint32_t flushAndData = uint32_t(NVDEF(NVB197, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                                           NVDEF(NVB197, INVALIDATE_SHADER_CACHES, DATA, TRUE));
    const uint32_t dataAndConst = uint32_t(NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE) |
                                           NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE));

    {
        gm20b::Texture storageTex;
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, storageTex));
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=80 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        gm20b::PackPipeline &pp = r.pipeline;
        pp.keyHash = 0x4b4201;
        pp.flags = gm20b::PipelinePushMetadata;
        pp.setBytes = 16;
        gm20b::setEntryCount(pp, 1);
        pp.entry[0] = {0, gm20b::EntryTexture, 2, 1, 0, 0, 0};
        pp.writtenFormat[1] = kBgra8;
        r.blob = {0x1a00, 0x1111};
        std::string whyStr;
        gm20b::Pipeline *pl = dev.addRecord(r, false, whyStr);
        ASSERT_NE(pl, nullptr) << whyStr;
        ASSERT_NE(pl->writtenSlots, 0u);
        ASSERT_FALSE(pl->writesMemory[0]);
        ASSERT_FALSE(pl->writesMemory[1]);

        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassUses uses;
        uses.known = true;
        gm20b::RenderEncoder *a = cb->render(cleared(big));
        ASSERT_NE(a, nullptr);
        a->setPipeline(pl);
        a->setTexture(2, 1, &storageTex);
        const char *why = nullptr;
        ASSERT_TRUE(a->draw(3, 0, 3, 1, 0, &why)) << (why ? why : "");
        a->plan(uses);
        a->end();
        gm20b::RenderEncoder *b = cb->render(cleared(half));
        ASSERT_NE(b, nullptr);
        b->plan(uses);
        b->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        const auto &words = kernel_model::model.jobs.back().words;
        const auto flushes = methodValues(words, kShaderCaches);
        ASSERT_EQ(flushes.size(), 1u);
        EXPECT_EQ(flushes[0], flushAndData);
        const auto noWfi = methodValues(words, kShaderCachesNoWfi);
        ASSERT_GE(noWfi.size(), 2u);
        EXPECT_EQ(noWfi.back(), dataAndConst);
        dev.destroyTexture(storageTex);
    }

    {
        gm20b::Buffer addrBuf;
        ASSERT_TRUE(dev.createBuffer(4096, addrBuf, true));
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassUses uses;
        uses.known = true;
        gm20b::RenderEncoder *a = cb->render(cleared(big));
        ASSERT_NE(a, nullptr);
        cb->useAddress(addrBuf.va, 256, true);
        a->plan(uses);
        a->end();
        gm20b::RenderEncoder *b = cb->render(cleared(half));
        ASSERT_NE(b, nullptr);
        b->plan(uses);
        b->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        const auto &words = kernel_model::model.jobs.back().words;
        const auto flushes = methodValues(words, kShaderCaches);
        ASSERT_EQ(flushes.size(), 1u);
        EXPECT_EQ(flushes[0], flushAndData);
        const auto noWfi = methodValues(words, kShaderCachesNoWfi);
        ASSERT_GE(noWfi.size(), 2u);
        EXPECT_EQ(noWfi.back(), dataAndConst);
        dev.destroyBuffer(addrBuf);
    }

    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassUses planned;
        planned.known = false;
        planned.shaderWrites = true;
        planned.bufferWrites = true;
        gm20b::RenderEncoder *a = cb->render(cleared(big));
        ASSERT_NE(a, nullptr);
        a->plan(planned);
        a->end();
        gm20b::PassUses clean;
        clean.known = true;
        gm20b::RenderEncoder *b = cb->render(cleared(half));
        ASSERT_NE(b, nullptr);
        b->plan(clean);
        b->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        const auto &words = kernel_model::model.jobs.back().words;
        const auto flushes = methodValues(words, kShaderCaches);
        ASSERT_EQ(flushes.size(), 1u);
        EXPECT_EQ(flushes[0], flushAndData);
        const auto noWfi = methodValues(words, kShaderCachesNoWfi);
        ASSERT_GE(noWfi.size(), 2u);
        EXPECT_EQ(noWfi.back(), dataAndConst);
    }
}

TEST(DeviceTiledCacheSize, DynamicTileSizeTracksPassFootprintAndShadowCaches)
{
    constexpr uint32_t kRgba16f = 115, kDepth32Stencil8 = 260, kTileSize = 0x0f64;
    kernel_model::model = {};
    kernel_model::model.infoFlags = TegraGM20BInfoTiledCache;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Texture bgra8, rgba16f, depth, ms4Color, ms4Depth;
    ASSERT_TRUE(dev.createTexture(128, 128, kBgra8, bgra8, false, true));
    ASSERT_TRUE(dev.createTexture(128, 128, kRgba16f, rgba16f, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(128, 128, kDepth32Stencil8, 1, false, depth));
    ASSERT_TRUE(dev.createStoredMultisample(128, 128, kRgba16f, 4, ms4Color));
    ASSERT_TRUE(dev.createOwnedZeta(128, 128, kDepth32Stencil8, 4, false, ms4Depth));

    const auto runPass = [&](gm20b::Texture &color, gm20b::Texture *zs) {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        gm20b::PassDesc pass;
        pass.color[0].texture = &color;
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.count = 1;
        if (zs) {
            pass.depthStencil.texture = zs;
            pass.depthStencil.clearDepth = true;
            pass.depthStencil.clearStencil = true;
        }
        gm20b::RenderEncoder *e = cb->render(pass);
        EXPECT_NE(e, nullptr);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> sizes;
        const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
        for (size_t at = 0; at < words.size();) {
            const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (!h)
                continue;
            if (form == 4) {
                if (subc == 0 && method == kTileSize)
                    sizes.push_back(count);
                continue;
            }
            for (uint32_t i = 0; i < count && at < words.size(); i++, at++) {
                const uint32_t m = form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0);
                if (subc == 0 && m == kTileSize)
                    sizes.push_back(words[at]);
            }
        }
        return sizes;
    };

    EXPECT_TRUE(runPass(bgra8, nullptr).empty());
    EXPECT_EQ(runPass(rgba16f, nullptr), (std::vector<uint32_t>{64u | (128u << 16)}));
    EXPECT_TRUE(runPass(rgba16f, nullptr).empty());
    EXPECT_EQ(runPass(rgba16f, &depth), (std::vector<uint32_t>{64u | (64u << 16)}));
    EXPECT_EQ(runPass(ms4Color, nullptr), (std::vector<uint32_t>{64u | (32u << 16)}));
    EXPECT_EQ(runPass(ms4Color, &ms4Depth), (std::vector<uint32_t>{32u | (32u << 16)}));
    EXPECT_EQ(runPass(bgra8, nullptr), (std::vector<uint32_t>{128u | (128u << 16)}));

    EXPECT_EQ(runPass(ms4Color, &ms4Depth), (std::vector<uint32_t>{32u | (32u << 16)}));
    // A clear-only pass of a 64x64 area over a stored 128x128 BGRA8 and a 128x128 depth: its colour's clear by a pass of
    // its own (the whole attachment) at the colour's tile size, then a depth kept by the pass cleared over the area at the
    // pass's, or a stored one cleared whole by a pass of its own at the zeta's and the pass's targets then set at the
    // pass's. The tile size and CLEAR_SURFACE's words in order.
    const auto clearOnly = [&](bool stored) {
        std::vector<std::pair<uint32_t, uint32_t>> events;
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return events;
        gm20b::PassDesc pass;
        pass.color[0].texture = &bgra8;
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.color[0].store = true;
        pass.count = 1;
        pass.depthStencil.texture = &depth;
        pass.depthStencil.clearDepth = true;
        pass.depthStencil.stored = stored;
        pass.width = 64;
        pass.height = 64;
        gm20b::RenderEncoder *e = cb->render(pass);
        EXPECT_NE(e, nullptr);
        if (!e)
            return events;
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
        for (size_t at = 0; at < words.size();) {
            const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (!h)
                continue;
            if (form == 4) {
                if (subc == 0 && (method == kTileSize || method == NVB197_CLEAR_SURFACE))
                    events.emplace_back(method, count);
                continue;
            }
            for (uint32_t i = 0; i < count && at < words.size(); i++, at++) {
                const uint32_t m = form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0);
                if (subc == 0 && (m == kTileSize || m == NVB197_CLEAR_SURFACE))
                    events.emplace_back(m, words[at]);
            }
        }
        return events;
    };
    using Event = std::pair<uint32_t, uint32_t>;
    constexpr uint32_t kClear = NVB197_CLEAR_SURFACE, kColourClear = 0x3c, kDepthClear = 1;
    EXPECT_EQ(clearOnly(false), (std::vector<Event>{{kTileSize, 128u | (128u << 16)},
                                                    {kClear, kColourClear},
                                                    {kTileSize, 64u | (64u << 16)},
                                                    {kClear, kDepthClear}}));
    EXPECT_EQ(clearOnly(true), (std::vector<Event>{{kTileSize, 128u | (128u << 16)},
                                                   {kClear, kColourClear},
                                                   {kTileSize, 64u | (128u << 16)},
                                                   {kClear, kDepthClear},
                                                   {kTileSize, 64u | (64u << 16)}}));

    dev.destroyTexture(ms4Depth);
    dev.destroyTexture(ms4Color);
    dev.destroyTexture(depth);
    dev.destroyTexture(rgba16f);
    dev.destroyTexture(bgra8);
}

TEST(DeviceTiledCacheTail, TailTiledCacheFlushRunsOnlyWhenDirty)
{
    constexpr uint32_t kTiledCacheFlush = SWITCH_NVB197_TILED_CACHE_FLUSH;
    kernel_model::model = {};
    kernel_model::model.infoFlags = TegraGM20BInfoTiledCache;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer buf;
    gm20b::Texture color;
    ASSERT_TRUE(dev.createBuffer(4096, buf, true));
    ASSERT_TRUE(dev.createTexture(128, 128, kBgra8, color, false, true));

    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::BlitEncoder *b = cb->blit();
    b->fillBytes(buf.va, 256, 0x11);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kTiledCacheFlush), 0);

    cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc pass;
    pass.color[0].texture = &color;
    pass.color[0].load = gm20b::ColorAttachment::Clear;
    pass.count = 1;
    cb->render(pass)->end();
    ASSERT_TRUE(cb->commit(seqno));
    m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kTiledCacheFlush), 1);

    {
        constexpr uint32_t kDepth32Float = 252;
        gm20b::Texture fourColor, twoZeta;
        ASSERT_TRUE(dev.createArray(64, 64, 4, kBgra8, gm20b::fx::kTexTwoDArray, fourColor));
        ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, twoZeta, 2));
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc shadowPass;
        shadowPass.color[0].texture = &fourColor;
        shadowPass.color[0].load = gm20b::ColorAttachment::Load;
        shadowPass.color[0].store = true;
        shadowPass.count = 1;
        shadowPass.layers = 4;
        shadowPass.depthStencil = {&twoZeta, false, false, 0.75f, 0};
        shadowPass.depthStencil.loaded = true;
        cb->render(shadowPass)->end();
        ASSERT_TRUE(cb->commit(seqno));
        m = methods(kernel_model::model.jobs.back().words);
        EXPECT_EQ(std::count(m.begin(), m.end(), kTiledCacheFlush), 1);
        dev.destroyTexture(twoZeta);
        dev.destroyTexture(fourColor);
    }

    {
        constexpr uint32_t kDepth32Float = 252, kStencil8 = 253;
        gm20b::Texture sepDepth, sepStencil;
        ASSERT_TRUE(dev.createOwnedZeta(128, 128, kDepth32Float, 1, false, sepDepth));
        ASSERT_TRUE(dev.createOwnedZeta(128, 128, kStencil8, 1, false, sepStencil));
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc sepPass;
        sepPass.color[0].texture = &color;
        sepPass.color[0].load = gm20b::ColorAttachment::Clear;
        sepPass.count = 1;
        sepPass.depthStencil.texture = &sepDepth;
        sepPass.depthStencil.clearDepth = true;
        sepPass.depthStencil.clearStencil = true;
        sepPass.separate = {&sepDepth, &sepStencil, false, false, true, true};
        cb->render(sepPass)->end();
        ASSERT_TRUE(cb->commit(seqno));
        m = methods(kernel_model::model.jobs.back().words);
        EXPECT_EQ(std::count(m.begin(), m.end(), kTiledCacheFlush), 1);
        dev.destroyTexture(sepStencil);
        dev.destroyTexture(sepDepth);
    }

    {
        gm20b::Texture second;
        ASSERT_TRUE(dev.createTexture(128, 128, kBgra8, second, false, true));
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc firstPass;
        firstPass.color[0].texture = &color;
        firstPass.color[0].load = gm20b::ColorAttachment::Clear;
        firstPass.count = 1;
        cb->render(firstPass)->end();
        gm20b::PassDesc secondPass;
        secondPass.color[0].texture = &second;
        secondPass.color[0].load = gm20b::ColorAttachment::Clear;
        secondPass.count = 1;
        cb->render(secondPass)->end();
        ASSERT_TRUE(cb->commit(seqno));
        m = methods(kernel_model::model.jobs.back().words);
        const auto firstFlush = std::find(m.begin(), m.end(), kTiledCacheFlush);
        const auto drainWfi = std::find(m.begin(), m.end(), kWaitForIdle);
        ASSERT_NE(firstFlush, m.end());
        ASSERT_NE(drainWfi, m.end());
        EXPECT_LT(firstFlush, drainWfi);
        EXPECT_EQ(std::count(m.begin(), m.end(), kTiledCacheFlush), 2);
        dev.destroyTexture(second);
    }

    dev.destroyTexture(color);
    dev.destroyBuffer(buf);
}

}  // namespace
