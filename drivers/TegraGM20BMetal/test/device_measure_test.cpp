// SPDX-License-Identifier: MIT
// band_rates.h's measured draws on gm20b::Device over the kernel model, with the tiled cache on: a measured draw adds
// its two report-semaphore stamps and no tiled-cache flush, only a draw alone in its pass is measured, a state no
// completion teaches is measured no more than its warm picks and one in kRemeasureEvery, and learnCompleted, which the
// next command buffer's begin calls, teaches what the GPU wrote.
#include "device_harness.h"
#include "timed_gpu.h"
#include "../record.h"
#include "../stamps.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kSide = 512;
constexpr uint16_t kTiledCacheFlush = 0x0f80;

gm20b::RuntimeRecord plainRecord(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
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

// A submission's words: its tiled-cache flushes, draw stamps and band progress releases
struct Words {
    uint32_t flushes = 0;
    uint32_t progressReleases = 0;
    uint32_t jobs = 0;
    uint32_t internalFences = 0;
    std::vector<std::pair<uint64_t, uint32_t>> stamps;
    std::vector<long> stampAt, flushAt;  // their places among the methods
};

Words read(const std::vector<uint32_t> &words, uint64_t cbStampPage, uint64_t progressVa)
{
    Words w;
    std::vector<switch_gm20b_method> m(words.size());
    const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
    EXPECT_GE(n, 0);
    constexpr uint32_t kProgressRelease = NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) |
                                          NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN) |
                                          NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 16BYTE);
    for (long i = 0; i < n; i++) {
        if (m[i].method == kTiledCacheFlush) {
            w.flushes++;
            w.flushAt.push_back(i);
        }
        if (progressVa && m[i].method == NVB06F_SEMAPHORED && m[i].value == kProgressRelease && i >= 3) {
            const uint64_t va = uint64_t(m[i - 3].value & 0xff) << 32 | m[i - 2].value;
            if (va == progressVa)
                w.progressReleases++;
        }
        if (m[i].method == NVB197_SET_REPORT_SEMAPHORE_D && m[i].value == gm20b::kDrawStampRelease && i >= 3) {
            const uint64_t va = uint64_t(m[i - 3].value) << 32 | m[i - 2].value;
            if (va >= cbStampPage && va < cbStampPage + gm20b::kDrawStampBase)
                continue;
            w.stamps.push_back({va, m[i - 1].value});
            w.stampAt.push_back(i);
        }
    }
    return w;
}

class DeviceMeasure : public testing::Test {
protected:
    gm20b::Device dev;
    gm20b::Texture target;
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = uint64_t(1) << 40;
        kernel_model::model.infoFlags = TegraGM20BInfoTiledCache;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kRGBA8, target));
        std::string why;
        pl = dev.addRecord(plainRecord(0x3e01), false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    Words passOn(gm20b::Texture &into, uint32_t draws, gm20b::Pipeline *first = nullptr, uint32_t *outSeq = nullptr)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        const size_t before = kernel_model::model.jobs.size();
        gm20b::PassDesc desc;
        desc.color[0].texture = &into;
        desc.color[0].load = gm20b::ColorAttachment::Load;
        desc.count = 1;
        gm20b::RenderEncoder *e = cb->render(desc);
        EXPECT_NE(e, nullptr);
        for (uint32_t i = 0; i < draws; i++) {
            e->setPipeline(i == 0 && first ? first : pl);
            const char *why = nullptr;
            EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        }
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        if (outSeq)
            *outSeq = seqno;
        std::vector<uint32_t> words;
        uint32_t internalFences = 0;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
            for (const auto &push : kernel_model::model.jobs[j].pushes)
                if (push.flags & TegraGM20BPushFence)
                    internalFences++;
        }
        Words w = read(words, dev.loopRecordVa(1) - gm20b::kLoopWordsBase, dev.channelProgressVa());
        w.jobs = uint32_t(kernel_model::model.jobs.size() - before);
        w.internalFences = internalFences;
        return w;
    }

    // One command buffer of one pass of `draws` full-target draws of `pl`: its submitted words, read
    Words pass(uint32_t draws)
    {
        return passOn(target, draws);
    }
};

TEST_F(DeviceMeasure, AMeasuredWholeDrawAddsItsStampsAndNoTiledCacheFlush)
{
    // A state already learned to run under budget stays whole and measures via pass-level stamps without extra flushes.
    gm20b::learn(pl->bandRate, 100'000, uint64_t(kSide) * kSide);
    const Words measured = pass(1);
    ASSERT_EQ(measured.stamps.size(), 2u);
    for (uint32_t i = 1; i < gm20b::kWarmSamples; i++)
        EXPECT_EQ(pass(1).stamps.size(), 2u) << "warm pick " << i;
    const Words unmeasured = pass(1);
    EXPECT_TRUE(unmeasured.stamps.empty());
    EXPECT_EQ(measured.flushes, unmeasured.flushes);
    EXPECT_GE(unmeasured.flushes, 1u);
    const auto drain = std::find_if(measured.flushAt.begin(), measured.flushAt.end(), [&](long at) {
        return at > measured.stampAt[0] && at < measured.stampAt[1];
    });
    EXPECT_NE(drain, measured.flushAt.end());
    EXPECT_EQ(measured.stamps[0].second, measured.stamps[1].second);
    EXPECT_EQ(measured.stamps[1].first - measured.stamps[0].first, offsetof(gm20b::StampSlot, end));
}

TEST_F(DeviceMeasure, OnlyALearnedWholeDrawAloneInItsPassIsMeasured)
{
    gm20b::learn(pl->bandRate, 100'000, uint64_t(kSide) * kSide);
    EXPECT_TRUE(pass(2).stamps.empty());
    EXPECT_TRUE(pass(5).stamps.empty());
    EXPECT_EQ(pl->bandRate.picks.load(), 0u);
    EXPECT_EQ(pass(1).stamps.size(), 2u);
}

TEST_F(DeviceMeasure, SmallUnmeasuredDrawInSecondSlotStaysWholeWhileHeavyOneBandsAndTeaches)
{
    std::string why;
    gm20b::Pipeline *fast = dev.addRecord(plainRecord(0x3e02), false, why);
    ASSERT_NE(fast, nullptr) << why;
    gm20b::learn(fast->bandRate, 100'000, uint64_t(kSide) * kSide);

    const uint64_t smallBandsBefore = dev.stats.bands.load();
    const Words smallWords = passOn(target, 2, fast);
    EXPECT_EQ(dev.stats.bands.load(), smallBandsBefore);
    EXPECT_EQ(smallWords.progressReleases, 0u);
    EXPECT_TRUE(smallWords.stamps.empty());

    gm20b::RuntimeRecord heavyRec = plainRecord(0x3e04);
    heavyRec.programs[1].id = "fs_heavy";
    heavyRec.programs[1].image.assign(128 + 4096 * 8, 0);
    gm20b::Pipeline *heavy = dev.addRecord(heavyRec, false, why);
    ASSERT_NE(heavy, nullptr) << why;
    pl = heavy;

    const uint64_t bandsBefore = dev.stats.bands.load();
    const Words w = passOn(target, 2, fast);
    const uint64_t emittedBands = dev.stats.bands.load() - bandsBefore;
    EXPECT_EQ(emittedBands, 6u);
    EXPECT_EQ(w.progressReleases, emittedBands + 1);
    EXPECT_EQ(w.internalFences, 0u);
    EXPECT_EQ(w.jobs, 1u);
    ASSERT_EQ(w.stamps.size(), 2u);
    const uint64_t ticks[2] = {1000, 1000 + 20 * 614'400};
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
    EXPECT_EQ(pl->bandRate.psPerUnit.load(), 20'000'000'000ull / (uint64_t(kSide) * kSide));
}

TEST_F(DeviceMeasure, BackToBackHeavyUnmeasuredDrawsVaryBandCountAndSeparateOverhead)
{
    std::string why;
    gm20b::RuntimeRecord uberRec = plainRecord(0x3e05);
    uberRec.programs[1].id = "fs_uber";
    uberRec.programs[1].image.assign(128 + 15152 * 8, 0);
    gm20b::Pipeline *uber = dev.addRecord(uberRec, false, why);
    ASSERT_NE(uber, nullptr) << why;
    pl = uber;

    const uint64_t bandsBefore = dev.stats.bands.load();
    const Words w = passOn(target, 2);
    const uint64_t totalBands = dev.stats.bands.load() - bandsBefore;
    EXPECT_EQ(totalBands, 9u);
    ASSERT_EQ(w.stamps.size(), 4u);
    const uint32_t bandsPerDraw[2] = {6, 3};
    for (size_t d = 0; d < 2; d++) {
        const uint64_t msTicks = (1 + bandsPerDraw[d] * 2) * 614'400ull;  // 1 ms pixels + 2 ms/band
        const uint64_t ticks[2] = {1000 + d * 10'000'000, 1000 + d * 10'000'000 + msTicks};
        for (size_t i = 0; i < 2; i++) {
            const gm20b::StampRelease r{w.stamps[d * 2 + i].second, 0, ticks[i]};
            memcpy(kernel_model::model.host(w.stamps[d * 2 + i].first), &r, sizeof r);
        }
    }
    dev.learnCompleted();
    EXPECT_TRUE(pl->bandRate.separated.load());
    EXPECT_NEAR(double(pl->bandRate.nsPerBand.load()), 2'000'000.0, 200'000.0);
    EXPECT_NEAR(double(gm20b::estimatedNs(pl->bandRate, uint64_t(kSide) * kSide, 0)), 1'000'000.0, 150'000.0);
}

// The bench's case: nothing ever teaches the state, which is measured at its warm picks and then one in kRemeasureEvery
TEST_F(DeviceMeasure, AStateNothingTeachesIsMeasuredNoMoreThanTheCap)
{
    constexpr uint32_t kPasses = 100;
    uint32_t measured = 0;
    for (uint32_t i = 0; i < kPasses; i++)
        measured += !pass(1).stamps.empty();
    EXPECT_EQ(measured, gm20b::kWarmSamples + (kPasses - gm20b::kWarmSamples) / gm20b::kRemeasureEvery);
    EXPECT_EQ(pl->bandRate.samples.load(), 0u);
}

// What the GPU writes at the stamps' addresses, then any completer's call: the state learns the draw's rate
TEST_F(DeviceMeasure, AnyCompleterTeachesWhatTheGpuStamped)
{
    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    const uint64_t ticks[2] = {1000, 1000 + 614'400};  // one millisecond of PTIMER apart
    for (size_t i = 0; i < 2; i++) {
        uint8_t *at = kernel_model::model.host(w.stamps[i].first);
        ASSERT_NE(at, nullptr);
        const gm20b::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(at, &r, sizeof r);
    }
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
    EXPECT_EQ(pl->bandRate.psPerUnit.load(), 1'000'000'000ull / (uint64_t(kSide) * kSide));
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
}

// The next command buffer's begin teaches what the GPU stamped, on the thread that committed the measured draw
TEST_F(DeviceMeasure, TheNextCommandBufferTeachesWhatTheGpuStamped)
{
    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    const uint64_t ticks[2] = {1000, 1000 + 614'400};
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    EXPECT_EQ(pl->bandRate.samples.load(), 0u);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
    EXPECT_EQ(pl->bandRate.psPerUnit.load(), 1'000'000'000ull / (uint64_t(kSide) * kSide));
    cb->abandon();
}

// A large draw counts where its price came from: the static estimate until a measured draw of its state completed,
// then the learned rate, by which the next command buffer bands it (ten milliseconds in one-millisecond bands)
TEST_F(DeviceMeasure, TheNextCommandBufferPricesALargeDrawByItsLearnedRate)
{
    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    EXPECT_EQ(dev.stats.pricedStatic.load(), 1u);
    EXPECT_EQ(dev.stats.pricedLearned.load(), 0u);
    const uint64_t ticks[2] = {1000, 1000 + 10 * 614'400};
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    const uint64_t bands = dev.stats.bands.load();
    pass(1);
    EXPECT_EQ(dev.stats.pricedStatic.load(), 1u);
    EXPECT_EQ(dev.stats.pricedLearned.load(), 1u);
    EXPECT_EQ(dev.stats.bands.load() - bands, 10u);
}

TEST_F(DeviceMeasure, AMeasuredDrawTheGpuHasNotRunWaits)
{
    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 0u);
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{w.stamps[i].second, 0, 5000 + 6144 * i};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
}

TEST_F(DeviceMeasure, PriorityZeroNonCriticalAppIsNotCompositorClassAndIsBanded)
{
    kernel_model::model.priorityCritical = false;
    ASSERT_TRUE(dev.gpuPriority(0, 4));
    EXPECT_FALSE(dev.compositorClass());

    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    const uint64_t ticks[2] = {1000, 1000 + 10 * 614'400};
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    const uint64_t bandsBefore = dev.stats.bands.load();
    pass(1);
    EXPECT_EQ(dev.stats.bands.load() - bandsBefore, 10u);

    kernel_model::model.priorityCritical = true;
    ASSERT_TRUE(dev.gpuPriority(0, 4));
    EXPECT_TRUE(dev.compositorClass());
    const uint64_t bandsAfter = dev.stats.bands.load();
    pass(1);
    EXPECT_EQ(dev.stats.bands.load(), bandsAfter);
}

TEST_F(DeviceMeasure, TwoCommandBuffersOnTimedGpuLearnRateForTheSecond)
{
    timed_gpu::Gpu gpu;
    gpu.setPerJob(std::chrono::microseconds(500));
    uint32_t seq1 = 0;
    const Words first = passOn(target, 1, nullptr, &seq1);
    ASSERT_EQ(first.stamps.size(), 2u);
    const uint64_t ticks[2] = {2000, 2000 + 12 * 614'400};  // 12 ms
    for (size_t i = 0; i < 2; i++) {
        const gm20b::StampRelease r{first.stamps[i].second, 0, ticks[i]};
        memcpy(kernel_model::model.host(first.stamps[i].first), &r, sizeof r);
    }
    ASSERT_TRUE(dev.wait(seq1));
    const uint64_t bandsBefore = dev.stats.bands.load();
    uint32_t seq2 = 0;
    passOn(target, 1, nullptr, &seq2);
    ASSERT_TRUE(dev.wait(seq2));
    EXPECT_EQ(dev.stats.bands.load() - bandsBefore, 12u);
}

TEST_F(DeviceMeasure, Learned248MsDrawSplitsInto2DTilesInOneJobWithProgressWord)
{
    gm20b::Texture hd;
    ASSERT_TRUE(dev.createTexture(1280, 720, kRGBA8, hd));
    gm20b::learn(pl->bandRate, 248'000'000, 1280ull * 720);
    const uint64_t bandsBefore = dev.stats.bands.load();
    const Words w = passOn(hd, 1);
    const uint64_t bands = dev.stats.bands.load() - bandsBefore;
    EXPECT_GE(bands, 248u);
    EXPECT_EQ(w.jobs, 1u);
    EXPECT_EQ(w.internalFences, 0u);
    EXPECT_EQ(w.progressReleases, bands);
}

TEST(DeviceMeasureLegacy, LegacyKernelWithoutProgressWordCapsBandsAndUsesInternalFences)
{
    kernel_model::model = {};
    kernel_model::model.vaEnd = uint64_t(1) << 40;
    kernel_model::model.infoFlags = TegraGM20BInfoTiledCache | TegraGM20BInfoPushFences;
    kernel_model::model.channelProgress = false;
    gm20b::Device legacy;
    std::string err;
    ASSERT_TRUE(legacy.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Texture hd;
    ASSERT_TRUE(legacy.createTexture(1280, 720, kRGBA8, hd));
    std::string why;
    gm20b::Pipeline *lpl = legacy.addRecord(plainRecord(0x3e03), false, why);
    ASSERT_NE(lpl, nullptr) << why;
    gm20b::learn(lpl->bandRate, 248'000'000, 1280ull * 720);

    gm20b::CommandBuffer *cb = legacy.begin();
    ASSERT_NE(cb, nullptr);
    const size_t before = kernel_model::model.jobs.size();
    gm20b::PassDesc desc;
    desc.color[0].texture = &hd;
    desc.color[0].load = gm20b::ColorAttachment::Load;
    desc.count = 1;
    gm20b::RenderEncoder *e = cb->render(desc);
    ASSERT_NE(e, nullptr);
    e->setPipeline(lpl);
    const char *drawErr = nullptr;
    ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &drawErr)) << (drawErr ? drawErr : "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));

    uint32_t internalFences = 0;
    for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
        for (const auto &push : kernel_model::model.jobs[j].pushes)
            if (push.flags & TegraGM20BPushFence)
                internalFences++;
    EXPECT_EQ(legacy.channelProgressVa(), 0u);
    EXPECT_EQ(legacy.stats.bands.load(), gm20b::kMaxBands);
    EXPECT_GT(internalFences, 0u);
}

}  // namespace
