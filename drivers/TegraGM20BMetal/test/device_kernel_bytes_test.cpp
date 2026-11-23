// SPDX-License-Identifier: MIT
// The kernel's own memory a dispatch reads (its clock anchor table, in the connection's fence page) on gm20b::Device: bound
// with setKernelBytes, the job names no use of it and the kernel takes the job; named as a use (the old binding,
// setBytesAt with no allocation) SubmitJob refuses the job kIOReturnBadArgument, as the kernel finds no BO of the client's
// there (TegraGM20BUserClientJobSubmit.cpp). That refusal failed every blit resolveCounters (metal.native.counters'
// `enc.kernel a=0 b=0`, boots 21 and 22). And the facade binds the table through setKernelBytes at both its sites.
#include "device_harness.h"
#include "../compute.h"
#include "bazel/testing/source_text.h"

namespace {

constexpr uint64_t kAnchorsVa = 0xfff0000800ull;  // in no BO the connection holds or binds
constexpr uint32_t kAnchorsBytes = 1024;

gm20b::ComputeRecord readerOfSlot3()
{
    gm20b::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufType[0] = gm20b::CbufRoot;
    r.cbufType[1] = gm20b::CbufSet;
    r.cbufs = 2;
    r.kernel.regionWord = -1;
    r.kernel.entries = 1;
    r.kernel.entry[0] = {0, gm20b::KernelBuffer, 3, 0};
    r.kernel.setBytes = 16;
    return r;
}

const gm20b::ComputeRecord &reader()
{
    static const gm20b::ComputeRecord r = readerOfSlot3();
    return r;
}

bool namesAddress(const kernel_model::Job &job, uint64_t va)
{
    for (const TegraGM20BAbiSurfaceUse &use : job.uses)
        if ((use.access & TegraGM20BSurfaceGpuAddress) && use.handle == va)
            return true;
    return false;
}

class DeviceKernelBytes : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        DeviceTest::SetUp();
        kernel_model::model.unknownUsesRefused = true;
    }

    // One dispatch of the slot-3 reader with the table bound by `bind`, committed: whether the kernel took it
    template <typename Bind> bool dispatchCommitted(Bind bind)
    {
        const gm20b::ComputeState *k = gm20b::placeCompute(dev, reader());
        EXPECT_NE(k, nullptr);
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!k || !cb)
            return false;
        {
            gm20b::ComputeEncoder e(*cb, dev);
            e.setState(k);
            bind(e);
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *why = nullptr;
            EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
            e.end();
        }
        uint32_t seqno = 0;
        const bool committed = cb->commit(seqno);
        if (!committed)
            cb->abandon();
        return committed;
    }
};

TEST_F(DeviceKernelBytes, TheKernelsTableBoundAsKernelMemoryIsNoUseAndTheJobIsTaken)
{
    const size_t before = kernel_model::model.jobs.size();
    EXPECT_TRUE(dispatchCommitted([](gm20b::ComputeEncoder &e) {
        e.setKernelBytes(3, kAnchorsVa, kAnchorsBytes);
    }));
    ASSERT_GT(kernel_model::model.jobs.size(), before);
    for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
        EXPECT_FALSE(namesAddress(kernel_model::model.jobs[j], kAnchorsVa)) << "job " << j;
}

TEST_F(DeviceKernelBytes, TheTableNamedAsAUseIsRefusedAsTheKernelRefusesIt)
{
    EXPECT_FALSE(dispatchCommitted([](gm20b::ComputeEncoder &e) {
        e.setBytesAt(3, kAnchorsVa, kAnchorsBytes);
    }));
}

TEST_F(DeviceKernelBytes, TheFacadeBindsTheTableAsKernelMemoryAtBothSites)
{
    const std::string compute = switch_ios::test::SourceText("../TegraGM20BMetalCompute.mm"),
                      native = switch_ios::test::SourceText("../TegraGM20BMetalPlanNative.mm");
    ASSERT_FALSE(compute.empty());
    ASSERT_FALSE(native.empty());
    const size_t op = compute.find("case OpKernelClockAnchors:\n        e.");
    ASSERT_NE(op, std::string::npos);
    EXPECT_EQ(compute.compare(op + std::string("case OpKernelClockAnchors:\n        e.").size(), 15, "setKernelBytes("),
              0);
    const size_t plan = native.find("Source::ClockAnchors) {\n            compute->");
    ASSERT_NE(plan, std::string::npos);
    EXPECT_EQ(native.compare(plan + std::string("Source::ClockAnchors) {\n            compute->").size(), 15,
                             "setKernelBytes("),
              0);
}

}  // namespace
