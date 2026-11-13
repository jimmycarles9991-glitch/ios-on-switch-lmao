// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <memory>
#include <thread>
#include <vector>
#include "switch_vic_backing_stubs.h"
#include "../SwitchVICSurfaceBacking.cpp"

namespace {
unsigned registrations = 0, preparations = 0, completions = 0, destroyed = 0;
std::atomic<unsigned> adoptions{0};
std::atomic<int> adoptionError{0};
std::atomic<void *> adoptedDescriptor{nullptr};
}

extern "C" int switch_gpu_surface_adopt(void *descriptor, switch_gpu_backing *backing)
{
    EXPECT_NE(backing, nullptr);
    adoptedDescriptor = descriptor;
    ++adoptions;
    return adoptionError;
}

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *, uint32_t, uint32_t, void *owner,
                                                           void (*release)(void *))
{
    ++registrations;
    auto *backing = new switch_gpu_backing;
    backing->owner = owner;
    backing->release = release;
    return backing;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    if (--backing->refs == 0) {
        backing->release(backing->owner);
        ++destroyed;
        delete backing;
    }
}

extern "C" IOReturn tegra_vic_surface_prepare(TegraVICSurface *surface, uint64_t length, uint64_t *pages,
                                              uint32_t capacity, uint32_t *count)
{
    ++preparations;
    surface->prepared = true;
    *count = static_cast<uint32_t>((length + 4095) / 4096);
    EXPECT_LE(*count, capacity);
    for (uint32_t i = 0; i < *count; ++i)
        pages[i] = surface->memory->address + surface->baseOffset + i * 4096;
    return kIOReturnSuccess;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *surface)
{
    if (surface->prepared)
        ++completions;
    surface->surface->release();
    *surface = {};
}

namespace {
struct Surface {
    OSObject object;
    IOMemoryDescriptor memory;
    TegraVICSurface value = {};

    Surface()
    {
        value.surface = &object;
        value.memory = &memory;
        value.id = 17;
        value.desc.length = 4096;
    }
};

class SwitchVICBacking : public testing::Test {
    void SetUp() override
    {
        registrations = preparations = completions = destroyed = 0;
        adoptions = 0;
        adoptionError = 0;
        adoptedDescriptor = nullptr;
    }

    void TearDown() override
    {
        for (auto &entry : gSurfaces)
            evict(entry);
        EXPECT_EQ(gBytes, 0u);
    }
};

TEST_F(SwitchVICBacking, WarmSurfaceReusesItsPinnedBackingWithoutPageWalks)
{
    auto surface = std::make_unique<Surface>();
    auto *first = switch_vic_surface_backing(&surface->value);
    ASSERT_NE(first, nullptr);
    for (unsigned i = 0; i < 100; ++i) {
        auto *again = switch_vic_surface_backing(&surface->value);
        ASSERT_EQ(again, first);
        switch_gpu_backing_release(again);
    }
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(preparations, 1u);
    EXPECT_EQ(completions, 0u);
    switch_gpu_backing_release(first);
    for (auto &entry : gSurfaces)
        evict(entry);
    EXPECT_EQ(surface->object.refs, 1u);
    EXPECT_EQ(completions, 1u);
}

TEST_F(SwitchVICBacking, ReusedSurfaceIdCannotSelectAnotherObjectsCachedPages)
{
    Surface first, second;
    second.memory.address = 0x200000;
    auto *a = switch_vic_surface_backing(&first.value);
    auto *b = switch_vic_surface_backing(&second.value);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(registrations, 2u);
    EXPECT_EQ(preparations, 2u);
    switch_gpu_backing_release(a);
    switch_gpu_backing_release(b);
    for (auto &entry : gSurfaces)
        evict(entry);
}

TEST_F(SwitchVICBacking, EvictionCannotReleaseAnInFlightCallersPin)
{
    std::array<Surface, kSurfaceCacheCount + 1> surfaces;
    auto *held = switch_vic_surface_backing(&surfaces[0].value);
    ASSERT_NE(held, nullptr);
    for (size_t i = 1; i < surfaces.size(); ++i) {
        auto *backing = switch_vic_surface_backing(&surfaces[i].value);
        ASSERT_NE(backing, nullptr);
        switch_gpu_backing_release(backing);
    }
    EXPECT_EQ(destroyed, 0u);
    EXPECT_EQ(held->refs, 1u);
    EXPECT_EQ(surfaces[0].object.refs, 2u);
    switch_gpu_backing_release(held);
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(surfaces[0].object.refs, 1u);
    for (auto &entry : gSurfaces)
        evict(entry);
}

TEST_F(SwitchVICBacking, ConcurrentWarmUsersShareOnePreparedAllocation)
{
    Surface surface;
    auto *first = switch_vic_surface_backing(&surface.value);
    ASSERT_NE(first, nullptr);
    std::vector<std::thread> users;
    for (unsigned i = 0; i < 8; ++i)
        users.emplace_back([&] {
            for (unsigned j = 0; j < 100; ++j) {
                auto *backing = switch_vic_surface_backing(&surface.value);
                EXPECT_EQ(backing, first);
                switch_gpu_backing_release(backing);
            }
        });
    for (auto &user : users)
        user.join();
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(preparations, 1u);
    switch_gpu_backing_release(first);
    for (auto &entry : gSurfaces)
        evict(entry);
}

TEST_F(SwitchVICBacking, NewRegistrationAdoptsExistingDescriptorCpuLocks)
{
    Surface surface;
    auto *backing = switch_vic_surface_backing(&surface.value);
    ASSERT_NE(backing, nullptr);
    EXPECT_EQ(adoptions, 1u);
    EXPECT_EQ(adoptedDescriptor, &surface.memory);
    switch_gpu_backing_release(backing);
    for (auto &entry : gSurfaces)
        evict(entry);
}

TEST_F(SwitchVICBacking, CacheHitRefusesConflictingCpuReservationWithoutLeakingCallerPin)
{
    Surface surface;
    auto *backing = switch_vic_surface_backing(&surface.value);
    ASSERT_NE(backing, nullptr);
    switch_gpu_backing_release(backing);
    adoptionError = -16;
    EXPECT_EQ(switch_vic_surface_backing(&surface.value), nullptr);
    EXPECT_EQ(adoptions, 2u);
    EXPECT_EQ(backing->refs, 1u);
    adoptionError = 0;
    auto *retry = switch_vic_surface_backing(&surface.value);
    EXPECT_EQ(retry, backing);
    EXPECT_EQ(registrations, 1u);
    switch_gpu_backing_release(retry);
    for (auto &entry : gSurfaces)
        evict(entry);
}

TEST_F(SwitchVICBacking, FailedColdAdoptionLeavesOnlyBoundedCacheOwnership)
{
    Surface surface;
    adoptionError = -95;
    EXPECT_EQ(switch_vic_surface_backing(&surface.value), nullptr);
    EXPECT_EQ(adoptions, 1u);
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(surface.object.refs, 3u);
    for (auto &entry : gSurfaces)
        evict(entry);
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(surface.object.refs, 1u);
}
}  // namespace
