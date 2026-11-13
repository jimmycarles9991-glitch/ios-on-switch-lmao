// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "switch_native_surface_stubs.h"
#include "../SwitchVICSurfaceAccess.cpp"
#include "../../../kernel/switch/iokit/memory_attachment.cpp"

struct switch_gpu_backing {
    uint32_t refs = 1;
};

struct switch_gpu_cpu_lease {
    bool live = false;
};

namespace {
switch_gpu_backing backing;
switch_gpu_cpu_lease lease;
bool attached, backingAvailable;
int prepareError;
uint32_t prepareOptions, finishes, registrations, leaseReleases;
}

extern "C" int switch_native_surface_prepare_gpu(OSObject *, uint32_t, void *, uint32_t options,
                                                 switch_gpu_backing **out, switch_gpu_cpu_lease **owned)
{
    prepareOptions = options;
    *out = nullptr;
    *owned = nullptr;
    if (prepareError)
        return prepareError;
    if (attached) {
        ++backing.refs;
        *out = &backing;
        lease.live = true;
        *owned = &lease;
    }
    return 0;
}

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *)
{
    ++registrations;
    if (!backingAvailable)
        return nullptr;
    ++backing.refs;
    return &backing;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *value)
{
    --value->refs;
}

extern "C" void switch_gpu_admission_changed(void)
{}

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
    EXPECT_EQ(*owned, &lease);
    EXPECT_TRUE(lease.live);
    EXPECT_EQ(wrote, 0);
    lease.live = false;
    *owned = nullptr;
    ++leaseReleases;
    return 0;
}

extern "C" void switch_gpu_surface_finish(void *descriptor)
{
    ++finishes;
    SwitchMemoryAttachment::leaveCpu(static_cast<IOMemoryDescriptor *>(descriptor));
}

namespace {
class SwitchVICAccessTest : public testing::Test {
protected:
    OSObject *object;
    IOMemoryDescriptor *memory;
    TegraVICSurface surface;
    SwitchVICSurfaceAccess access;

    void SetUp() override
    {
        attached = false;
        backingAvailable = true;
        prepareError = 0;
        prepareOptions = finishes = registrations = leaseReleases = 0;
        backing = {};
        lease = {};
        object = new OSObject;
        memory = new IOMemoryDescriptor;
        surface = {object, memory, {}, 4096, 7, false};
        access = {};
    }

    void TearDown() override
    {
        switch_vic_surface_release_access(&access);
        EXPECT_EQ(backing.refs, 1u);
        EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 0u);
        EXPECT_FALSE(lease.live);
        object->release();
        memory->release();
    }
};

TEST_F(SwitchVICAccessTest, OrdinaryBackingStartsAtSurfaceBaseAndKeepsDescriptorGuard)
{
    ASSERT_EQ(switch_vic_surface_access(&surface, false, &access), 0);
    EXPECT_EQ(access.offset, 0u);
    EXPECT_EQ(access.backing, &backing);
    EXPECT_EQ(access.owned, nullptr);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 1u);
    EXPECT_EQ(access.guard.generation, SwitchMemoryAttachment::generation(memory));
    EXPECT_EQ(access.guard.expected, 1u);
    EXPECT_EQ(prepareOptions, 1u);
    switch_vic_surface_unreserve(&access);
    EXPECT_EQ(finishes, 1u);
    EXPECT_EQ(backing.refs, 2u);
}

TEST_F(SwitchVICAccessTest, MaterializedAliasKeepsCanonicalOffsetAndOwnershipUntilPublication)
{
    attached = true;
    ASSERT_EQ(switch_vic_surface_access(&surface, true, &access), 0);
    EXPECT_EQ(access.offset, 4096u);
    EXPECT_EQ(access.owned, &lease);
    EXPECT_EQ(registrations, 0u);
    EXPECT_EQ(prepareOptions, 0u);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 1u);
    switch_vic_surface_unreserve(&access);
    EXPECT_EQ(leaseReleases, 1u);
    EXPECT_EQ(finishes, 1u);
    EXPECT_EQ(backing.refs, 2u);
}

TEST_F(SwitchVICAccessTest, FailedMaterializationDropsDescriptorReservation)
{
    prepareError = -5;
    EXPECT_EQ(switch_vic_surface_access(&surface, true, &access), -5);
    EXPECT_EQ(finishes, 1u);
    EXPECT_EQ(access.descriptor, nullptr);
    EXPECT_EQ(access.backing, nullptr);
}

TEST_F(SwitchVICAccessTest, FailedRegistrationDropsDescriptorReservation)
{
    backingAvailable = false;
    EXPECT_EQ(switch_vic_surface_access(&surface, false, &access), -12);
    EXPECT_EQ(finishes, 1u);
    EXPECT_EQ(access.descriptor, nullptr);
}

TEST_F(SwitchVICAccessTest, AvoidSyncNeverWaitsForExclusivePublisher)
{
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(memory, false), 0);
    EXPECT_EQ(switch_vic_surface_access(&surface, false, &access, true), -16);
    EXPECT_EQ(registrations, 0u);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 0u);
    SwitchMemoryAttachment::leaveExclusive(memory);
    ASSERT_EQ(switch_vic_surface_access(&surface, false, &access, true), 0);
    EXPECT_EQ(prepareOptions, 3u);
}
}  // namespace
