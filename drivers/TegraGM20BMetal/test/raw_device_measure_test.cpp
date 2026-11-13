// SPDX-License-Identifier: MIT
// raw_band_rates.h's measured draws on raw::Device over the kernel model, with the tiled cache on: a measured draw adds
// its two report-semaphore stamps and no tiled-cache flush, only a draw alone in its pass is measured, a state no
// completion teaches is measured no more than its warm picks and one in kRemeasureEvery, and any completer's
// learnCompleted teaches what the GPU wrote.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_stamps.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kSide = 512;
constexpr uint16_t kTiledCacheFlush = 0x0f80;

raw::RuntimeRecord plainRecord(uint64_t key)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    return r;
}

// A submission's words: its tiled-cache flushes, and the draw stamps it releases (their addresses and payloads)
struct Words {
    uint32_t flushes = 0;
    std::vector<std::pair<uint64_t, uint32_t>> stamps;
    std::vector<long> stampAt, flushAt;  // their places among the methods
};

Words read(const std::vector<uint32_t> &words)
{
    Words w;
    std::vector<switch_gm20b_method> m(words.size());
    const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
    EXPECT_GE(n, 0);
    for (long i = 0; i < n; i++) {
        if (m[i].method == kTiledCacheFlush) {
            w.flushes++;
            w.flushAt.push_back(i);
        }
        if (m[i].method == NVB197_SET_REPORT_SEMAPHORE_D && m[i].value == raw::kDrawStampRelease && i >= 3) {
            w.stamps.push_back({uint64_t(m[i - 3].value) << 32 | m[i - 2].value, m[i - 1].value});
            w.stampAt.push_back(i);
        }
    }
    return w;
}

class DeviceMeasure : public testing::Test {
protected:
    raw::Device dev;
    raw::Texture target;
    raw::Pipeline *pl = nullptr;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = uint64_t(1) << 40;
        kernel_model::model.infoFlags = TegraGM20BInfoTiledCache;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kRGBA8, target));
        std::string why;
        pl = dev.addRecord(plainRecord(0x3e01), false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    // One command buffer of one pass of `draws` full-target draws of `pl`: its submitted words, read
    Words pass(uint32_t draws)
    {
        const size_t before = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        raw::PassDesc desc;
        desc.color[0].texture = &target;
        desc.color[0].load = raw::ColorAttachment::Load;
        desc.count = 1;
        raw::RenderEncoder *e = cb->render(desc);
        EXPECT_NE(e, nullptr);
        e->setPipeline(pl);
        for (uint32_t i = 0; i < draws; i++) {
            const char *why = nullptr;
            EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        }
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        return read(words);
    }
};

TEST_F(DeviceMeasure, AMeasuredDrawAddsItsStampsAndNoTiledCacheFlush)
{
    const Words measured = pass(1);
    ASSERT_EQ(measured.stamps.size(), 2u);
    for (uint32_t i = 1; i < raw::kWarmSamples; i++)
        EXPECT_EQ(pass(1).stamps.size(), 2u) << "warm pick " << i;
    const Words unmeasured = pass(1);
    EXPECT_TRUE(unmeasured.stamps.empty());
    EXPECT_EQ(measured.flushes, unmeasured.flushes);
    EXPECT_GE(unmeasured.flushes, 1u);
    // the end stamp follows the pass's own drain of the draw, the start stamp precedes it
    const auto drain = std::find_if(measured.flushAt.begin(), measured.flushAt.end(), [&](long at) {
        return at > measured.stampAt[0] && at < measured.stampAt[1];
    });
    EXPECT_NE(drain, measured.flushAt.end());
    EXPECT_EQ(measured.stamps[0].second, measured.stamps[1].second);
    EXPECT_EQ(measured.stamps[1].first - measured.stamps[0].first, offsetof(raw::StampSlot, end));
}

TEST_F(DeviceMeasure, OnlyADrawAloneInItsPassIsMeasured)
{
    EXPECT_TRUE(pass(2).stamps.empty());
    EXPECT_TRUE(pass(5).stamps.empty());
    EXPECT_EQ(pl->bandRate.picks.load(), 0u);
    EXPECT_EQ(pass(1).stamps.size(), 2u);
}

// The bench's case: nothing ever teaches the state, which is measured at its warm picks and then one in kRemeasureEvery
TEST_F(DeviceMeasure, AStateNothingTeachesIsMeasuredNoMoreThanTheCap)
{
    constexpr uint32_t kPasses = 100;
    uint32_t measured = 0;
    for (uint32_t i = 0; i < kPasses; i++)
        measured += !pass(1).stamps.empty();
    EXPECT_EQ(measured, raw::kWarmSamples + (kPasses - raw::kWarmSamples) / raw::kRemeasureEvery);
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
        const raw::StampRelease r{w.stamps[i].second, 0, ticks[i]};
        memcpy(at, &r, sizeof r);
    }
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
    EXPECT_EQ(pl->bandRate.psPerUnit.load(), 1'000'000'000ull / (uint64_t(kSide) * kSide));
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
}

TEST_F(DeviceMeasure, AMeasuredDrawTheGpuHasNotRunWaits)
{
    const Words w = pass(1);
    ASSERT_EQ(w.stamps.size(), 2u);
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 0u);
    for (size_t i = 0; i < 2; i++) {
        const raw::StampRelease r{w.stamps[i].second, 0, 5000 + 6144 * i};
        memcpy(kernel_model::model.host(w.stamps[i].first), &r, sizeof r);
    }
    dev.learnCompleted();
    EXPECT_EQ(pl->bandRate.samples.load(), 1u);
}

}  // namespace
