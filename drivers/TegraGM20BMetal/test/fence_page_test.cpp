// SPDX-License-Identifier: MIT
// The connection's fence page as gm20b::Kernel maps it, over the kernel model's page (kernel_model.h), laid out as the
// kernel's (TegraGM20BFencePageABI.h): the clock anchors counter samples resolve with stay as the kernel published them
// while the connection's writer promises are made, kept by their jobs and cancelled.
#include "kernel_model.h"
#include "TegraGM20BClockABI.h"
#include "TegraGM20BJobABI.h"
#include "../counter_kernel.h"
#include "../kernel.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using kernel_model::model;

constexpr TegraGM20BAbiClockAnchor kBoot = {20000000000ull, 15000000000ull}, kWake = {90000000000ull, 600000000000ull};

class GM20BFencePage : public testing::Test {
protected:
    gm20b::Kernel kernel;
    uint32_t channel = 0;
    const volatile uint32_t *fence = nullptr;
    const volatile TegraGM20BAbiClockAnchors *anchors = nullptr;
    uint64_t anchorsVa = 0;

    void SetUp() override
    {
        model = {};
        model.powerUp(kBoot);
        model.powerUp(kWake);
        std::string err;
        ASSERT_TRUE(kernel.open(err)) << err;
        ASSERT_TRUE(kernel.channel(TegraGM20BEngine3D | TegraGM20BEngineCopy, channel, fence));
        ASSERT_TRUE(kernel.clockAnchors(anchors, anchorsVa));
    }

    // A counter sample 384 PTIMER units into the wake's power-up, as resolveCounterRange: places it
    uint64_t resolvedSample() const
    {
        TegraGM20BAbiClockAnchors now;
        gm20b::counters::snapshot(*anchors, now);
        return gm20b::counters::resolved({gm20b::counters::kPayload, 0, kWake.units + 384}, now);
    }

    void keep(const uint32_t &promise)
    {
        gm20b::KernelJob job;
        job.channel = channel;
        job.promises = &promise;
        job.promiseCount = 1;
        uint32_t seqno = 0;
        ASSERT_TRUE(kernel.submitJob(job, seqno));
    }
};

// Every promise made, then kept by its job's end or cancelled, twice over
TEST_F(GM20BFencePage, PromisesKeptAndCancelledLeaveTheClockAnchors)
{
    ASSERT_EQ(resolvedSample(), kWake.ns + 625);
    for (uint32_t round = 0; round < 2; round++) {
        std::vector<uint32_t> made(TegraGM20BMaxPromises);
        for (uint32_t &promise : made)
            ASSERT_EQ(kernel.promise(channel, {7}, promise), KERN_SUCCESS);
        for (size_t i = 0; i < made.size(); i++) {
            if (i % 2)
                ASSERT_EQ(kernel.cancelPromise(made[i]), KERN_SUCCESS);
            else
                keep(made[i]);
            EXPECT_EQ(resolvedSample(), kWake.ns + 625) << "promise " << made[i];
        }
    }
    EXPECT_EQ(model.fences.stray(), kernel_model::kFencePageBytes);
}

}  // namespace
