// SPDX-License-Identifier: MIT
// draw_times.h's frame-weights capture on gm20b::Device over the kernel model, with the tiled cache on: an asked-for
// command buffer counts fragment invocations and marks its start, each pass's prologue twice and each draw (a banded one
// once), each mark behind a tiled-cache flush and a wait for idle; the command buffer after it stops the counter and marks
// nothing; one nobody asked for has neither; and a completer reads back what the GPU wrote as each mark's time and
// invocations.
#include "device_harness.h"
#include "../record.h"
#include "../stamps.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kSide = 512;
constexpr uint16_t kTiledCacheFlush = 0x0f80;
constexpr uint64_t kUnit = 384;  // PTIMER units: 625 ns

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

// A four-method semaphore or report at m[i]: its address and its D word
struct Report {
    uint64_t va = 0;
    uint32_t payload = 0, d = 0;
};

bool report(const std::vector<switch_gm20b_method> &m, size_t i, uint16_t first, Report &r)
{
    if (i + 3 >= m.size() || m[i].method != first || m[i + 3].method != first + 12)
        return false;
    r = {uint64_t(m[i].value & 0xff) << 32 | m[i + 1].value, m[i + 2].value, m[i + 3].value};
    return true;
}

class DeviceDrawTimes : public testing::Test {
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
        pass(3);  // the channel and its context made ahead of what a test reads
    }

    // One command buffer of one pass of `draws` full-target draws of `pl`: the methods it submitted
    std::vector<switch_gm20b_method> pass(uint32_t draws)
    {
        const size_t before = kernel_model::model.jobs.size();
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        gm20b::PassDesc desc;
        desc.color[0].texture = &target;
        desc.color[0].load = gm20b::ColorAttachment::Load;
        desc.count = 1;
        gm20b::RenderEncoder *e = cb->render(desc);
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
        std::vector<switch_gm20b_method> m(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        m.resize(size_t(std::max(n, 0L)));
        return m;
    }

    bool marks(uint64_t va) const
    {
        return dev.drawTimeMarks_.va && va >= dev.drawTimeMarks_.va &&
               va < dev.drawTimeMarks_.va + dev.drawTimeMarks_.bytes;
    }

    // The capture's releases (their methods' places) and invocations reports, and every SET_STATISTICS_COUNTER value
    struct Seen {
        std::vector<size_t> releases;
        size_t counters = 0;
        std::vector<uint32_t> statistics;
    };

    Seen seen(const std::vector<switch_gm20b_method> &m) const
    {
        Seen s;
        for (size_t i = 0; i < m.size(); i++) {
            if (m[i].method == NVB197_SET_STATISTICS_COUNTER)
                s.statistics.push_back(m[i].value);
            Report r;
            if (!report(m, i, NVB197_SET_REPORT_SEMAPHORE_A, r) || !marks(r.va))
                continue;
            if (r.d == gm20b::kInvocationsReport)
                s.counters++;
            else
                s.releases.push_back(i);
        }
        return s;
    }
};

TEST_F(DeviceDrawTimes, AnAskedCommandBufferMarksItsStartEachPrologueTwiceAndEachDrawBehindAFlushAndAWait)
{
    dev.askDrawTimes(1);
    const std::vector<switch_gm20b_method> m = pass(3);
    const Seen s = seen(m);
    EXPECT_EQ(s.statistics, (std::vector<uint32_t>{gm20b::kStatisticsPsInvocations}));
    ASSERT_EQ(s.releases.size(), 1u + 2u + 3u);
    EXPECT_EQ(s.counters, s.releases.size());
    for (size_t at : s.releases) {
        ASSERT_GE(at, 2u);
        EXPECT_EQ(m[at - 1].method, NVB197_WAIT_FOR_IDLE);
        EXPECT_EQ(m[at - 2].method, kTiledCacheFlush);
        EXPECT_EQ(m[at + 2].value, 1u);  // the capture's serial
    }
}

TEST_F(DeviceDrawTimes, TheCommandBufferAfterACaptureStopsTheCounterAndMarksNothing)
{
    dev.askDrawTimes(1);
    pass(3);
    const Seen after = seen(pass(3));
    EXPECT_EQ(after.statistics, (std::vector<uint32_t>{0}));
    EXPECT_TRUE(after.releases.empty());
    EXPECT_EQ(after.counters, 0u);
    EXPECT_TRUE(seen(pass(3)).statistics.empty());
}

TEST_F(DeviceDrawTimes, ACommandBufferNobodyAskedForHasNoMarksAndNoCounter)
{
    const Seen s = seen(pass(3));
    EXPECT_TRUE(s.statistics.empty());
    EXPECT_TRUE(s.releases.empty());
    EXPECT_EQ(dev.drawTimeMarks_.va, 0u);
}

TEST_F(DeviceDrawTimes, ABandedDrawIsOneMark)
{
    dev.bandBudgetNs_.store(1);
    const uint64_t banded = dev.stats.bandedDraws.load();
    dev.askDrawTimes(1);
    const Seen s = seen(pass(3));
    ASSERT_GT(dev.stats.bandedDraws.load(), banded) << "the draws were not banded: the scene tests nothing";
    EXPECT_EQ(s.releases.size(), 1u + 2u + 3u);
}

// A GPU that takes (k + 1) x kUnit to each capture mark k and counts 100 x k invocations ahead of it, and kUnit from the
// last mark to the end stamp: the completer reads back each step
TEST_F(DeviceDrawTimes, ACompleterReadsBackEachMarksTimeAndInvocations)
{
    dev.askDrawTimes(1);
    const std::vector<switch_gm20b_method> m = pass(3);
    uint64_t ptimer = 100000, counter = 0;
    uint32_t mark = 0;
    for (size_t i = 0; i < m.size(); i++) {
        Report r;
        if ((report(m, i, NVB06F_SEMAPHOREA, r) && r.d == gm20b::HOST_SEMAPHORE_RELEASE_16_WFI &&
             r.va != dev.channelProgressVa()) ||
            (report(m, i, NVB197_SET_REPORT_SEMAPHORE_A, r) && !marks(r.va))) {
            ptimer += mark == 6 ? kUnit : 0;
            const gm20b::StampRelease release{r.payload, 0, ptimer};
            memcpy(kernel_model::model.host(r.va, sizeof release), &release, sizeof release);
        } else if (report(m, i, NVB197_SET_REPORT_SEMAPHORE_A, r) && marks(r.va)) {
            if (r.d == gm20b::kInvocationsReport) {
                const uint64_t counted[2] = {counter, ptimer};
                memcpy(kernel_model::model.host(r.va, sizeof counted), counted, sizeof counted);
                continue;
            }
            mark++;
            ptimer += mark * kUnit;
            counter += 100 * (mark - 1);
            const gm20b::StampRelease release{r.payload, 0, ptimer};
            memcpy(kernel_model::model.host(r.va, sizeof release), &release, sizeof release);
        }
    }
    ASSERT_EQ(mark, 6u);
    dev.learnCompleted();
    const gm20b::DrawTimesTaken t = dev.drawTimesTaken();
    EXPECT_TRUE(t.ended);
    ASSERT_EQ(t.buffers.size(), 1u);
    EXPECT_EQ(t.buffers[0].gpuNs, 22u * 625u);
    ASSERT_EQ(t.rows.size(), 7u);
    std::string kinds;
    for (const gm20b::DrawTimesTaken::Row &r : t.rows)
        kinds += gm20b::timedKindLetter(r.row.label.kind);
    EXPECT_EQ(kinds, "sbpdddt");
    for (uint32_t k = 0; k < 6; k++)
        EXPECT_EQ(t.rows[k].row.ns, (k + 1) * 625u) << "mark " << k;
    EXPECT_EQ(t.rows[6].row.ns, 625u);
    EXPECT_EQ(t.rows[0].row.invocations, -1);
    EXPECT_EQ(t.rows[3].row.invocations, 300);
    EXPECT_EQ(t.rows[5].row.invocations, 500);
    EXPECT_EQ(t.rows[3].state.key, 0x3e01u);
    EXPECT_EQ(t.rows[3].pass.width, kSide);
}

}  // namespace
