// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDECCancel.cpp on threads: the falcon's reboot and proof run without the power lock NVJPG's gate shares, under
// a hold of NVDEC's gate, and parking an engine that did not come back takes the lock again.
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <future>
#include "drivers/TegraNVDEC/TegraNVDECCancel.cpp"

namespace {
using namespace std::chrono_literals;

IOLock gPower;
std::promise<void> gProving, gProven;
bool gBoots = true, gHeld, gReleased, gWedgedUnderLock;

bool heldElsewhere()
{
    return std::async(std::launch::async,
                      [] {
                          if (!gPower.mutex.try_lock())
                              return true;
                          gPower.mutex.unlock();
                          return false;
                      })
        .get();
}

// The cancellation on its thread call's thread, until its proof waits; then whether NVJPG's gate could take the lock.
int lockBlockedDuringProof()
{
    gProving = {};
    gProven = {};
    switch_nvdec_job_stalled(8, -5, 1000);
    auto cancel = std::async(std::launch::async, [] {
        switch_cancel_entered->fn(nullptr, nullptr);
    });
    gProving.get_future().wait();
    auto use = std::async(std::launch::async, [] {
        IOLockLock(&gPower);
        IOLockUnlock(&gPower);
    });
    const int blocked = use.wait_for(300ms) == std::future_status::timeout ? 1 : 0;
    gProven.set_value();
    cancel.get();
    use.get();
    return blocked;
}
}  // namespace

extern "C" IOLock *tegra_nvdec_power_lock(void)
{
    return &gPower;
}

bool switch_nvdec_engine_powered()
{
    return true;
}

void switch_nvdec_engine_describe(char *text, size_t size)
{
    snprintf(text, size, "falcon");
}

bool switch_nvdec_engine_falcon_failed(char *, size_t)
{
    return false;
}

bool switch_nvdec_engine_reset(bool *drained)
{
    *drained = true;
    return true;
}

bool switch_nvdec_engine_boot()
{
    gProving.set_value();
    gProven.get_future().wait();
    return gBoots;
}

const char *switch_nvdec_engine_wedge()
{
    gWedgedUnderLock = heldElsewhere();
    return "falcon";
}

uint32_t switch_nvdec_jobs_cancel(uint32_t reached)
{
    return reached;
}

void switch_nvdec_jobs_recovered(bool)
{}

void switch_nvdec_power_hold_locked()
{
    gHeld = heldElsewhere();
}

void switch_nvdec_power_release()
{
    gReleased = true;
}

extern "C" {
uint32_t tegra_host1x_syncpt_read(uint32_t)
{
    return 7;
}

int tegra_host1x_syncpt_release_failed(uint32_t, uint32_t, uint32_t)
{
    return 0;
}

void tegra_host1x_describe(char *buffer, size_t size)
{
    snprintf(buffer, size, "host1x");
}

void tegra_host1x_channel_teardown(uint32_t)
{}

void tegra_host1x_channel_restart(uint32_t)
{}
}

TEST(NVDECCancel, NVJPGsGateTakesTheSharedPowerLockWhileNVDECsFalconProves)
{
    ASSERT_TRUE(switch_nvdec_cancel_start(9));
    gBoots = true;
    // base 1: the proof's syncpoint waits held the power lock NVJPG's use() takes
    EXPECT_EQ(lockBlockedDuringProof(), 0);
    EXPECT_TRUE(gHeld) << "NVDEC's gate is held from under the lock";
    EXPECT_TRUE(gReleased);
}

TEST(NVDECCancel, AnEngineThatDidNotComeBackIsParkedUnderThePowerLock)
{
    ASSERT_TRUE(switch_nvdec_cancel_start(9));
    gBoots = false;
    gWedgedUnderLock = false;
    lockBlockedDuringProof();
    EXPECT_TRUE(gWedgedUnderLock);
}
