// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decoder's pool of pictures materialised into its client's pool of surfaces (switch_vic_present_surface_owned, the
// CPU read of a native publication), through VIC's real map cache: each picture and each surface is mapped into VIC's
// SMMU space once for its allocation's life, and a pool that has gone round maps nothing. Boot 22 made 2029 mappings
// for 1470 such presents (TegraVIC `maps=` lines), each with an SMMU flush.
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>
#include "switch_vic_backing_stubs.h"
#undef IONewZero
#undef IODelete
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)

[[noreturn]] void panic(const char *)
{
    throw std::runtime_error("mapping panic");
}

#include "../TegraVICPresent.cpp"
#include "../SwitchVICMapping.cpp"

namespace {
struct Allocation {
    switch_gpu_backing backing;
    uint64_t id = 0;
    std::vector<uint64_t> pages;
};

std::map<const switch_gpu_backing *, Allocation *> gAllocations;
std::map<uint32_t, Allocation *> gSurfaceBackings;
uint64_t gNextId = 100, gNextPage = 0x100000000;
uint64_t gNextAddress = 0x40000000;
std::vector<uint64_t> gCreatedFirstPages;
std::vector<uint64_t> gDestroyed;
void (*gForget)(uint64_t);
OSObject gSurfaceObject;
IOMemoryDescriptor gSurfaceMemory;
}  // namespace

extern "C" bool tegra_vic_ready()
{
    return true;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return &kTegraVICTraits;
}

extern "C" uint64_t tegra_vic_config_address()
{
    return 0x80001000;
}

extern "C" uint64_t tegra_vic_filter_address()
{
    return 0x80002000;
}

extern "C" void switch_refusal(const char *, const char *, const char *, ...)
{}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t, task_t, TegraVICSurface *)
{
    return -22;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *surface)
{
    if (surface->surface)
        surface->surface->release();
    *surface = {};
}

extern "C" void tegra_vic_surface_written(const TegraVICSurface *)
{}

extern "C" void switch_vic_stats_present(bool)
{}

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *surface)
{
    auto *backing = &gSurfaceBackings.at(surface->id)->backing;
    ++backing->refs;
    return backing;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    --backing->refs;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    const Allocation *a = gAllocations.at(backing);
    *info = {};
    info->allocation_id = a->id;
    info->allocation_generation = 1;
    info->pages = a->pages.data();
    info->page_count = static_cast<uint32_t>(a->pages.size());
}

extern "C" int switch_gpu_backing_register_forget(uint32_t engine, void (*forget)(uint64_t))
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    gForget = forget;
    return 0;
}

extern "C" void switch_gpu_backing_note_mapped(switch_gpu_backing *, uint32_t)
{}

void switch_vic_totals_register()
{}

int switch_vic_surface_access(const TegraVICSurface *, bool, SwitchVICSurfaceAccess *, bool, switch_gpu_backing *)
{
    ADD_FAILURE() << "an owned materialisation takes no access of its own";
    return -22;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *)
{}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access)
{
    *access = {};
}

int switch_vic_submit_checked(const TegraVICJob *, uint32_t, const TegraVICAcquire *, const SwitchVICOwnership &,
                              void (*completion)(void *, int), void *context, uint32_t *fence, SwitchVICEvents *)
{
    *fence = 1;
    if (completion)
        completion(context, 0);
    return 0;
}

extern "C" int switch_vic_wait(uint32_t)
{
    return 0;
}

extern "C" uint64_t switch_vic_mapping_create(const uint64_t *pages, uint32_t, bool)
{
    gCreatedFirstPages.push_back(pages[0]);
    return gNextAddress += 0x1000000;
}

extern "C" void switch_vic_mapping_destroy(uint64_t address, uint32_t)
{
    gDestroyed.push_back(address);
}

namespace {
struct Geometry {
    uint32_t width, height, pitch, rows;
};

constexpr Geometry k720p = {1280, 720, 1536, 736}, k4k = {3840, 2160, 3840, 2176};
constexpr unsigned kPool = 16, kFrames = 30;

class VICPresentIdentity : public testing::Test {
protected:
    std::vector<Allocation *> pictures, surfaces;
    switch_gpu_cpu_lease *lease = reinterpret_cast<switch_gpu_cpu_lease *>(uintptr_t(0x1234));

    void SetUp() override
    {
        if (!gMappings.lock)
            ASSERT_TRUE(switch_vic_mapping_start());
        tegra_vic_present_start();
        gCreatedFirstPages.clear();
        gDestroyed.clear();
    }

    void TearDown() override
    {
        for (auto &mapping : gMappings.maps) {
            EXPECT_EQ(mapping.users, 0u);
            gMappings.evict(mapping);
        }
        for (Allocation *a : pictures)
            EXPECT_EQ(a->backing.refs, 1u);
        for (Allocation *a : surfaces)
            EXPECT_EQ(a->backing.refs, 1u);
        gAllocations.clear();
        gSurfaceBackings.clear();
        for (Allocation *a : pictures)
            delete a;
        for (Allocation *a : surfaces)
            delete a;
    }

    static Allocation *allocate(uint64_t bytes)
    {
        auto *a = new Allocation;
        a->id = ++gNextId;
        for (uint64_t at = 0; at < bytes; at += 4096)
            a->pages.push_back(gNextPage += 4096);
        gAllocations[&a->backing] = a;
        return a;
    }

    void pools(const Geometry &g)
    {
        for (unsigned i = 0; i < kPool; ++i) {
            pictures.push_back(allocate(uint64_t(g.pitch) * g.rows * 3 / 2));
            surfaces.push_back(allocate(uint64_t(g.pitch) * g.height * 3 / 2));
            gSurfaceBackings[200 + i] = surfaces.back();
        }
    }

    static TegraVICPicture pictureOf(const Geometry &g, Allocation *a, uint32_t surface)
    {
        TegraVICPicture p = {};
        p.backing = &a->backing;
        p.pages = a->pages.data();
        p.pageCount = static_cast<uint32_t>(a->pages.size());
        p.bytes = uint64_t(g.pitch) * g.rows * 3 / 2;
        p.pitch = g.pitch;
        p.rows = g.rows;
        p.chromaOffset = g.pitch * g.rows;
        p.blockHeightLog2 = 1;
        p.width = g.width;
        p.height = g.height;
        p.surfaceID = surface;
        p.sampleBytes = 1;
        return p;
    }

    static TegraVICSurface surfaceOf(const Geometry &g, uint32_t id)
    {
        TegraVICSurfaceDesc d = {};
        d.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
        d.width = g.width;
        d.height = g.height;
        d.planes = 2;
        d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = g.pitch;
        d.planeHeight[0] = g.height;
        d.planeHeight[1] = g.height / 2;
        d.planeOffset[1] = uint64_t(g.pitch) * g.height;
        d.length = uint64_t(g.pitch) * g.height * 3 / 2;
        gSurfaceObject.retain();
        return {&gSurfaceObject, &gSurfaceMemory, d, 0, id, false};
    }

    void present(const Geometry &g, unsigned frame)
    {
        const uint32_t id = 200 + frame % kPool;
        TegraVICPicture p = pictureOf(g, pictures[frame % kPool], id);
        TegraVICSurface s = surfaceOf(g, id);
        const char *why = nullptr;
        ASSERT_EQ(switch_vic_present_surface_owned(&p, &s, lease, &why), 0) << (why ? why : "");
        tegra_vic_surface_release(&s);
    }

    unsigned createdFor(const std::vector<Allocation *> &pool) const
    {
        unsigned n = 0;
        for (uint64_t first : gCreatedFirstPages)
            for (const Allocation *a : pool)
                n += first >= a->pages.front() && first <= a->pages.back();
        return n;
    }

    void playPools(const Geometry &g)
    {
        pools(g);
        size_t warm = 0;
        for (unsigned frame = 0; frame < kFrames; ++frame) {
            if (frame == kPool)
                warm = gCreatedFirstPages.size();
            present(g, frame);
        }
        EXPECT_LE(createdFor(pictures), kPool) << "the decoder's pool, at most one mapping a picture";
        EXPECT_LE(createdFor(surfaces), kPool) << "the client's pool, at most one mapping a surface";
        EXPECT_EQ(gCreatedFirstPages.size(), warm) << "mappings made after both pools went round";
        EXPECT_TRUE(gDestroyed.empty());
    }
};

TEST_F(VICPresentIdentity, A720pPoolGoingRoundMapsEachBufferOnce)
{
    playPools(k720p);
}

TEST_F(VICPresentIdentity, A4kPoolGoingRoundMapsEachBufferOnce)
{
    playPools(k4k);
}

// A freed pool buffer's mapping goes with it, not at the next eviction: the backing's destruction forgets the
// allocation. One a user still holds is unmapped at once and leaves the cache at its release, found by nobody.
TEST_F(VICPresentIdentity, AFreedPictureIsUnmappedWhenItsBackingGoes)
{
    pools(k720p);
    present(k720p, 0);
    ASSERT_NE(gForget, nullptr);
    const size_t made = gCreatedFirstPages.size();
    gForget(pictures[0]->id);
    EXPECT_EQ(gDestroyed.size(), 1u);
    present(k720p, 0);
    EXPECT_EQ(gCreatedFirstPages.size(), made + 1) << "a forgotten allocation is never found again";

    auto *held = switch_vic_mapping_acquire(&pictures[1]->backing, false);
    ASSERT_NE(held, nullptr);
    gForget(pictures[1]->id);
    EXPECT_EQ(gDestroyed.size(), 2u);
    EXPECT_EQ(switch_vic_mapping_address(held), 0u);
    auto *again = switch_vic_mapping_acquire(&pictures[1]->backing, false);
    EXPECT_NE(again, held);
    switch_vic_mapping_release(again);
    switch_vic_mapping_release(held);
    for (const auto &mapping : gMappings.maps)
        EXPECT_FALSE(mapping.orphan);
}
}  // namespace
