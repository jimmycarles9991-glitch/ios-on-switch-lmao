// SPDX-License-Identifier: MIT
// A zcull region carried from one job of a Metal command buffer to its next range's (zcull_planes.h ZcullCarry), on
// gm20b::Device over the kernel model with the X1's zcull layout: the storing job's STORE_ZCULL is a store only where a
// pass loads the region, the next job loads it from the same slot, and a job submitted in between leaves it nothing.
#include "device_forms.h"
#include "TegraGM20BZcullABI.h"

namespace {

constexpr uint32_t kLoadZcullMethod = 0x1500, kStoreZcullMethod = 0x1464, kStorageA = 0x07e8;
constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

class DeviceZcullCarry : public DevicePassForms {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, zeta));
    }

    gm20b::PassDesc depthPass(bool clears)
    {
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Load);
        p.count = 1;
        p.depthStencil = {&zeta, clears, false, 1.0f, 0};
        return p;
    }

    // One job of the passes `clears` names in order (true: clears the depth, false: loads it), resuming what was parked
    // for (`resumeTo`, kRange) and keeping regions `kept` for (`carryTo`, kRange): its methods, empty where refused
    std::vector<Method> job(std::initializer_list<bool> clears, const void *resumeTo = nullptr,
                            const void *carryTo = nullptr, uint32_t kept = 0)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        const size_t first = kernel_model::model.jobs.size();
        if (resumeTo)
            cb->resumeZcull(resumeTo, kRange);
        for (const bool clear : clears) {
            const char *rule = nullptr;
            gm20b::RenderEncoder *e = cb->render(depthPass(clear), &rule);
            if (!e) {
                cb->abandon();
                return {};
            }
            e->end();
        }
        cb->carryZcull(carryTo, kRange, kept);
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return {};
        }
        return submitted(first);
    }

    static constexpr uint64_t kRange = 40;
    gm20b::Texture colour, zeta;
    const int commandBuffer = 0;
};

size_t unstoredWords(size_t first)
{
    size_t n = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++)
        n += size_t(std::count(kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end(),
                               gm20b::kZcullUnstoredWord));
    return n;
}

// A region no pass loads is never stored: its pass's STORE_ZCULL word stays a NO_OPERATION
TEST_F(DeviceZcullCarry, ARegionNoPassLoadsIsNotStored)
{
    const size_t first = kernel_model::model.jobs.size();
    const auto ms = job({true});
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(values(ms, kStoreZcullMethod).size(), 0u);
    EXPECT_EQ(unstoredWords(first), 1u);
    EXPECT_EQ(dev.stats.zcullUnstored.load(), 1u);
    EXPECT_EQ(dev.stats.zcullStores.load(), 0u);
}

// A later pass of the same job that loads the depth makes the earlier pass's word a store, and loads from its slot
TEST_F(DeviceZcullCarry, APassOfTheSameJobThatLoadsTheRegionMakesItAStore)
{
    const auto ms = job({true, false});
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(values(ms, kStoreZcullMethod).size(), 1u);
    EXPECT_EQ(values(ms, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(dev.stats.zcullLoads.load(), 1u);
    EXPECT_EQ(dev.stats.zcullCarried.load(), 0u);
}

// The region the storing job keeps for its command buffer's next range is stored, and that range's job loads it from
// the same storage
TEST_F(DeviceZcullCarry, TheNextRangesJobLoadsTheRegionTheJobBeforeItKept)
{
    const auto stored = job({true}, nullptr, &commandBuffer, 1);
    ASSERT_FALSE(stored.empty());
    EXPECT_EQ(values(stored, kStoreZcullMethod).size(), 1u);
    const uint64_t storage = addressOf(stored, kStorageA);
    ASSERT_NE(storage, 0u);
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(addressOf(loaded, kStorageA), storage);
    EXPECT_EQ(dev.stats.zcullCarried.load(), 1u);
}

// A job submitted between the storing range and the next leaves the next range no region
TEST_F(DeviceZcullCarry, AJobSubmittedInBetweenLeavesTheNextRangeNoRegion)
{
    ASSERT_FALSE(job({true}, nullptr, &commandBuffer, 1).empty());
    const int other = 0;
    ASSERT_FALSE(job({true}, nullptr, &other, 0).empty());
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 0u);
    EXPECT_EQ(dev.stats.zcullCarriesLost.load(), 1u);
}

// A job encoded and abandoned in between submits nothing: the next range still loads the region, from a slot the
// abandoned job could not take
TEST_F(DeviceZcullCarry, AnAbandonedJobInBetweenTakesNotTheCarriedSlot)
{
    const auto stored = job({true}, nullptr, &commandBuffer, 1);
    ASSERT_FALSE(stored.empty());
    const uint64_t storage = addressOf(stored, kStorageA);
    gm20b::CommandBuffer *between = dev.begin();
    ASSERT_NE(between, nullptr);
    const char *rule = nullptr;
    gm20b::RenderEncoder *e = between->render(depthPass(true), &rule);
    ASSERT_NE(e, nullptr) << (rule ? rule : "");
    e->end();
    between->abandon();
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(addressOf(loaded, kStorageA), storage);
}

// A range that resumed the carry and is then abandoned for good (its command buffer failed) ends the carry: the loss
// is counted once, and nothing resumes it after
TEST_F(DeviceZcullCarry, AnAbandonedResumeCountsTheCarryLostAndEndsIt)
{
    ASSERT_FALSE(job({true}, nullptr, &commandBuffer, 1).empty());
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->resumeZcull(&commandBuffer, kRange);
    cb->abandon();
    EXPECT_EQ(dev.stats.zcullCarriesLost.load(), 1u);
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 0u);
    EXPECT_EQ(dev.stats.zcullCarriesLost.load(), 1u);
}

// A range whose commit the kernel refuses for a CPU-locked surface (admissionBlocked) is encoded again once the lock
// goes: that encode resumes the carry and loads the region from the same storage. An encode abandoned in between may
// take the carry's slot, which the retry holds again: its pass over another depth stores elsewhere.
TEST_F(DeviceZcullCarry, AnAdmissionBlockedCommitKeepsTheCarryForItsRetry)
{
    const auto stored = job({true}, nullptr, &commandBuffer, 1);
    ASSERT_FALSE(stored.empty());
    const uint64_t storage = addressOf(stored, kStorageA);
    ASSERT_NE(storage, 0u);
    static uint8_t pages[0x4000] __attribute__((aligned(0x4000)));
    gm20b::Import surface;
    ASSERT_TRUE(gm20b::DeviceHarness::kernel(dev).import(pages, sizeof pages, surface.bo));
    ASSERT_EQ(kernel_model::model.cpuLock(surface.bo.handle), KERN_SUCCESS);
    gm20b::CommandBuffer *blocked = dev.begin();
    ASSERT_NE(blocked, nullptr);
    blocked->resumeZcull(&commandBuffer, kRange);
    blocked->use(&surface, false);
    const char *rule = nullptr;
    gm20b::RenderEncoder *e = blocked->render(depthPass(false), &rule);
    ASSERT_NE(e, nullptr) << (rule ? rule : "");
    e->end();
    blocked->carryZcull(nullptr, kRange, 0);
    uint32_t seqno = 0;
    ASSERT_FALSE(blocked->commit(seqno));
    ASSERT_TRUE(blocked->admissionBlocked);
    blocked->abandonForRetry();
    kernel_model::model.cpuLocked.clear();
    gm20b::CommandBuffer *between = dev.begin();
    ASSERT_NE(between, nullptr);
    e = between->render(depthPass(true), &rule);
    ASSERT_NE(e, nullptr) << (rule ? rule : "");
    e->end();
    between->abandon();
    gm20b::Texture other;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, other));
    gm20b::CommandBuffer *retry = dev.begin();
    ASSERT_NE(retry, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    retry->resumeZcull(&commandBuffer, kRange);
    gm20b::PassDesc otherPass = depthPass(true);
    otherPass.depthStencil.texture = &other;
    for (const gm20b::PassDesc &p : {otherPass, depthPass(false)}) {
        e = retry->render(p, &rule);
        ASSERT_NE(e, nullptr) << (rule ? rule : "");
        e->end();
    }
    retry->carryZcull(nullptr, kRange, 0);
    ASSERT_TRUE(retry->commit(seqno));
    const auto loaded = submitted(first);
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(addressOf(loaded, kStorageA), storage);
    const std::vector<uint32_t> upper = values(loaded, kStorageA), lower = values(loaded, kStorageA + 4);
    ASSERT_EQ(upper.size(), 2u);
    EXPECT_NE(uint64_t(upper[0]) << 32 | lower[0], storage) << "the other depth's region took the carried slot";
    EXPECT_EQ(dev.stats.zcullCarriesLost.load(), 0u);
}

// A job the parked carry leaves no slot to (it holds the only one) finds one made at its begin(), not by its first
// pass mid-encode
TEST_F(DeviceZcullCarry, TheSlotACarryLeavesNoneOfIsMadeAtTheNextBegin)
{
    ASSERT_FALSE(job({true}, nullptr, &commandBuffer, 1).empty());
    EXPECT_EQ(gm20b::DeviceHarness::zcullSlotsMade(dev), 1u);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(gm20b::DeviceHarness::zcullSlotsMade(dev), 2u) << "made between the jobs";
    cb->abandon();
}

// A pass whose draws invalidate its region (a depth write under Always) records none and gives its slot back: after five
// such depths, a sixth still stores its region and the pass that loads it loads it
TEST_F(DeviceZcullCarry, APassThatRecordsNoRegionGivesItsSlotBack)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=252/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs-zcull";
    r.programs[1].id = "fs-zcull";
    r.pipeline.keyHash = 0x6b07;
    r.blob = {0x1a00, 0x1111};
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    gm20b::DepthStencilDesc always;
    always.depthCompare = 7;  // MTLCompareFunctionAlways
    always.depthWrite = true;
    const gm20b::DepthStencilState invalidating(always);
    gm20b::Texture zetas[6];
    for (gm20b::Texture &z : zetas)
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, z));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const auto pass = [&](gm20b::Texture &z, bool clears, bool draws) {
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Load);
        p.count = 1;
        p.depthStencil = {&z, clears, false, 1.0f, 0};
        const char *rule = nullptr;
        gm20b::RenderEncoder *e = cb->render(p, &rule);
        ASSERT_NE(e, nullptr) << (rule ? rule : "");
        if (draws) {
            e->setPipeline(pl);
            e->setDepthStencil(&invalidating);
            ASSERT_TRUE(e->draw(3, 0, 3, 1, 0, &rule)) << (rule ? rule : "");
        }
        e->end();
    };
    for (int k = 0; k < 5; k++)
        pass(zetas[k], true, true);
    pass(zetas[5], true, false);
    pass(zetas[5], false, false);
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const auto ms = submitted(first);
    EXPECT_EQ(values(ms, kLoadZcullMethod).size(), 1u) << "the sixth depth found a slot";
    EXPECT_EQ(values(ms, kStoreZcullMethod).size(), 1u);
}

}  // namespace
