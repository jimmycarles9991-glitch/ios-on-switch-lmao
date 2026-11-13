// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchVICEncodeInput.cpp: the job VIC runs for an encoder's input picture, against stubs of the surface lookup,
// ownership, mappings, copy engine, bounce pool and queue. A 1920x1080 NV12 surface with 2048-byte rows into NVENC's BL16 input buffer of the
// coded 1920x1088 picture: the output surface is block linear with sixteen-GOB blocks at the buffer's luma and chroma
// offsets, the region lands at the origin 1:1, the source is a checked read and the buffer a recorded write, and the
// job waits for the encoder's last job first; the coded rows past the picture get its last two rows stretched over
// them in the same batch, the columns past a narrower one its last two columns, both before the picture itself.
#include <gtest/gtest.h>
#include <vector>
#include "switch_vic_backing_stubs.h"

inline void IOLog(const char *, ...)
{}

#include "../SwitchVICEncodeInput.cpp"

struct SwitchVICMapping {
    uint64_t address;
};

namespace {
OSObject surfaceObject;
IOMemoryDescriptor surfaceMemory;
switch_gpu_backing sourcePitch, buffer;
volatile uint64_t sourceGeneration = 1;
bool found = true, vicUp = true, sourcePitchLayout = true;
int mappings = 0, accesses = 0, lookups = 0;
void (*pending)(void *, int);
void *pendingContext;
TegraVICJob submittedJob;
std::vector<TegraVICJob> submittedJobs;
TegraVICAcquire submittedAfter;
bool hadAfter;
std::vector<switch_gpu_backing_use> declaredUses;
uint32_t sourceRow = 2048;
SwitchVICBounceSlot bounceSlot;
switch_gpu_backing scratchBacking;
IOMemoryDescriptor scratchMemory;
SwitchVICMapping *scratchMapping;
int bounces = 0;

struct EngineCopy {
    tegra_gm20b_ce_plane src, dst;
    uint32_t rowBytes, rows;
    switch_gpu_backing *from, *to;
    uint64_t fromOffset, toOffset;
    tegra_gm20b_ce_done done;
};

std::vector<EngineCopy> engineCopies;
}  // namespace

int switch_vic_bounce_acquire(const TegraVICSurfaceDesc *source, const TegraVICSurfaceDesc *destination,
                              SwitchVICBounceSlot **out)
{
    EXPECT_EQ(destination, nullptr);
    ++bounces;
    bounceSlot = {};
    tegra_vic_bounce_desc(source, 256, &bounceSlot.buffers[0].desc);
    bounceSlot.buffers[0].memory = &scratchMemory;
    bounceSlot.buffers[0].backing = &scratchBacking;
    bounceSlot.buffers[0].mapping = scratchMapping;
    *out = &bounceSlot;
    return 0;
}

void switch_vic_bounce_release(SwitchVICBounceSlot *slot)
{
    if (slot)
        --bounces;
}

extern "C" int tegra_gm20b_ce_copy_plane_checked(const tegra_gm20b_ce_plane *src, const tegra_gm20b_ce_plane *dst,
                                                 uint32_t rowBytes, uint32_t rows, const tegra_gm20b_ce_wait *,
                                                 const tegra_gm20b_ce_done *done, switch_gpu_backing *from,
                                                 uint64_t fromOffset, switch_gpu_backing *to, uint64_t toOffset,
                                                 const switch_gpu_copy_ownership *ownership,
                                                 const switch_gpu_event_submission *)
{
    EXPECT_EQ(ownership->guard_count, 1u);
    engineCopies.push_back({*src, *dst, rowBytes, rows, from, to, fromOffset, toOffset, *done});
    return 0;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return &kTegraVICTraits;
}

extern "C" bool tegra_vic_ready()
{
    return vicUp;
}

extern "C" uint64_t tegra_vic_config_address()
{
    return 0x80001000;
}

extern "C" uint64_t tegra_vic_filter_address()
{
    return 0x80002000;
}

bool switch_vic_pitch_backing(switch_gpu_backing *, uint64_t)
{
    return sourcePitchLayout;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    static uint64_t pages[1] = {0x100000};
    *info = {};
    info->pages = pages;
    info->page_count = backing == &buffer ? (6u << 20) >> 12 : 1024;
}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t id, task_t, TegraVICSurface *out)
{
    *out = {};
    if (!found || id != 77)
        return -1;
    ++lookups;
    out->surface = &surfaceObject;
    out->memory = &surfaceMemory;
    out->id = id;
    TegraVICSurfaceDesc &d = out->desc;
    d.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    d.width = 1920;
    d.height = 1080;
    d.planes = 2;
    d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = sourceRow;
    d.planeHeight[0] = 1080;
    d.planeHeight[1] = 540;
    d.planeOffset[1] = uint64_t(sourceRow) * 1080;
    d.length = uint64_t(sourceRow) * 1080 * 3 / 2;
    d.colorMatrix = SWITCH_VIC_MATRIX_709;
    return 0;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *s)
{
    if (s->surface)
        --lookups;
    *s = {};
}

int switch_native_surface_acquire(const TegraVICSurface *, SwitchNativeSurfaceView *, bool, bool)
{
    return -2;
}

void switch_native_surface_release(SwitchNativeSurfaceView *view)
{
    *view = {};
}

int switch_vic_surface_access(const TegraVICSurface *surface, bool write, SwitchVICSurfaceAccess *access, bool)
{
    EXPECT_FALSE(write);
    ++accesses;
    access->descriptor = surface->memory;
    access->backing = &sourcePitch;
    access->guard = {&sourcePitch, &sourceGeneration, 1};
    return 0;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *access)
{
    access->descriptor = nullptr;
}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access)
{
    if (access->backing)
        --accesses;
    *access = {};
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool write)
{
    EXPECT_EQ(write, backing == &buffer);
    ++mappings;
    return new SwitchVICMapping{write ? 0x50000000u : 0x40000000u};
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (mapping) {
        --mappings;
        delete mapping;
    }
}

uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping)
{
    return mapping->address;
}

const switch_gpu_backing_token kWritten = {0x7000010e1ULL, 3, SWITCH_GPU_ENGINE_VIC, 161};

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *after,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events, switch_gpu_backing_token *written)
{
    EXPECT_EQ(events, nullptr);
    submittedJob = jobs[count - 1];
    submittedJobs.assign(jobs, jobs + count);
    hadAfter = after != nullptr;
    if (after)
        submittedAfter = *after;
    declaredUses.assign(ownership.uses, ownership.uses + ownership.useCount);
    EXPECT_EQ(ownership.guardCount, 1u);
    pending = completion;
    pendingContext = context;
    *fence = 4321;
    *written = kWritten;
    return 0;
}

namespace {
constexpr uint32_t kPitch = 1920, kLumaRows = 1152, kChromaRows = 640;
constexpr uint64_t kChroma = uint64_t(kPitch) * kLumaRows;

class SwitchVICEncodeInputTest : public testing::Test {
protected:
    TegraVICAcquire last = {9, 1234};
    SwitchVICEncodeInput input = {};

    void SetUp() override
    {
        found = vicUp = sourcePitchLayout = true;
        mappings = accesses = lookups = 0;
        pending = nullptr;
        hadAfter = false;
        declaredUses.clear();
        submittedJobs.clear();
        engineCopies.clear();
        sourceRow = 2048;
        scratchMapping = new SwitchVICMapping{0x60000000u};
        input.destination = &buffer;
        input.lumaOffset = 0;
        input.chromaOffset = kChroma;
        input.pitch = kPitch;
        input.lumaRows = kLumaRows;
        input.chromaRows = kChromaRows;
        input.blockLinear = 1;
        input.blockHeightLog2 = 4;
        input.width = 1920;
        input.height = 1088;
        input.w = 1920;
        input.h = 1080;
        input.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
        input.after = &last;
    }

    void TearDown() override
    {
        for (const auto &copy : engineCopies)
            copy.done.fn(copy.done.arg, 0);
        if (pending)
            pending(pendingContext, 0);
        EXPECT_EQ(bounces, 0);
        delete scratchMapping;
        EXPECT_EQ(mappings, 0);
        EXPECT_EQ(accesses, 0);
        EXPECT_EQ(lookups, 0);
    }

    uint64_t output(uint32_t base, uint32_t bit, uint32_t width) const
    {
        return tegra_vic_get(submittedJob.config, base, bit, width);
    }

    uint32_t method(uint32_t m) const
    {
        for (uint32_t i = 0; i < submittedJob.count; ++i) {
            if (submittedJob.method[i] == m)
                return submittedJob.value[i];
        }
        return 0xffffffffu;
    }
};

TEST_F(SwitchVICEncodeInputTest, WritesTheRegionBlockLinearAtTheBufferOriginAfterTheEncodersLastJob)
{
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), 0) << why;
    EXPECT_EQ(fence, 4321u);
    EXPECT_EQ(written.value, kWritten.value) << "the encoder learns the job's failure by it";
    EXPECT_EQ(written.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    ASSERT_TRUE(hadAfter);
    EXPECT_EQ(submittedAfter.syncpt, 9u);
    EXPECT_EQ(submittedAfter.threshold, 1234u);
    ASSERT_EQ(declaredUses.size(), 2u);
    EXPECT_EQ(declaredUses[0].backing, &buffer);
    EXPECT_EQ(declaredUses[0].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    EXPECT_EQ(declaredUses[1].backing, &sourcePitch);
    EXPECT_EQ(declaredUses[1].access, uint32_t(SWITCH_GPU_BACKING_READ));

    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND),
              uint64_t(TEGRA_VIC_BLK_16Bx2));
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_HEIGHT), 4u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH), kPitch - 1);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT), kLumaRows - 1);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT), kChromaRows - 1);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT), 1919u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM), 1079u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_BOTTOM), uint64_t(1079) << 16);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_FILTER_X), uint64_t(TEGRA_VIC_FILTER_1TAP));
    EXPECT_EQ(method(TEGRA_VIC_M_OUTPUT_LUMA), 0x50000000u >> 8);
    EXPECT_EQ(method(TEGRA_VIC_M_OUTPUT_CHROMA_U), uint32_t((0x50000000u + kChroma) >> 8));
    EXPECT_EQ(method(TEGRA_VIC_M_SURFACE0_SLOT0_LUMA), 0x40000000u >> 8);
    EXPECT_EQ(mappings, 2);

    ASSERT_EQ(submittedJobs.size(), 2u);
    const uint8_t *below = submittedJobs[0].config;
    EXPECT_EQ(tegra_vic_get(below, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP_BIT, SLOT_SOURCE_TOP_WIDTH), uint64_t(1078) << 16);
    EXPECT_EQ(tegra_vic_get(below, TEGRA_VIC_SLOTS, SLOT_SOURCE_BOTTOM_BIT, SLOT_SOURCE_BOTTOM_WIDTH), uint64_t(1079)
                                                                                                           << 16);
    EXPECT_EQ(tegra_vic_get(below, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP_BIT, OUT_TARGET_TOP_WIDTH), 1080u);
    EXPECT_EQ(tegra_vic_get(below, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM_BIT, OUT_TARGET_BOTTOM_WIDTH), 1087u);
    EXPECT_EQ(tegra_vic_get(below, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT_BIT, OUT_TARGET_RIGHT_WIDTH), 1919u);
}

TEST_F(SwitchVICEncodeInputTest, ANarrowerPictureGetsItsLastColumnsStretchedRight)
{
    input.w = 1900;
    input.h = 1088;
    input.height = 1088;
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why),
              -95);  // 1088 rows of a 1080-row surface
    input.h = 1080;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), 0) << why;
    ASSERT_EQ(submittedJobs.size(), 3u);
    const uint8_t *right = submittedJobs[1].config;
    EXPECT_EQ(tegra_vic_get(right, TEGRA_VIC_SLOTS, SLOT_SOURCE_LEFT_BIT, SLOT_SOURCE_LEFT_WIDTH), uint64_t(1898)
                                                                                                       << 16);
    EXPECT_EQ(tegra_vic_get(right, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT_BIT, OUT_TARGET_LEFT_WIDTH), 1900u);
    EXPECT_EQ(tegra_vic_get(right, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT_BIT, OUT_TARGET_RIGHT_WIDTH), 1919u);
    EXPECT_EQ(tegra_vic_get(right, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM_BIT, OUT_TARGET_BOTTOM_WIDTH), 1079u);
}

TEST_F(SwitchVICEncodeInputTest, ATileRegionIsReadFromItsOriginInTheSurface)
{
    input.width = input.height = input.w = input.h = 512;
    input.pitch = 512;
    input.lumaRows = 512;
    input.chromaRows = 256;
    input.chromaOffset = 512 * 512;
    input.x = 1024;
    input.y = 512;
    input.after = nullptr;
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), 0) << why;
    EXPECT_FALSE(hadAfter);
    EXPECT_EQ(submittedJobs.size(), 1u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_LEFT), uint64_t(1024) << 16);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP), uint64_t(512) << 16);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_DEST_LEFT), 0u);
}

// CoreVideo's 1920-byte rows are off VIC's 256-byte grid: the copy engine copies the region's rows of both planes into
// the scratch at 2048 bytes a row, VIC reads the scratch, and the source stays listed under its guard.
TEST_F(SwitchVICEncodeInputTest, RowsOffVICsGridAreBouncedThroughTheCopyEngine)
{
    sourceRow = 1920;
    input.y = 0;
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), 0) << why;
    EXPECT_TRUE(bounced);
    ASSERT_EQ(engineCopies.size(), 2u);
    EXPECT_EQ(engineCopies[0].rows, 1080u);
    EXPECT_EQ(engineCopies[0].rowBytes, 1920u);
    EXPECT_EQ(engineCopies[0].src.pitch, 1920u);
    EXPECT_EQ(engineCopies[0].dst.pitch, 2048u);
    EXPECT_EQ(engineCopies[0].from, &sourcePitch);
    EXPECT_EQ(engineCopies[0].to, &scratchBacking);
    EXPECT_EQ(engineCopies[1].rows, 540u);
    EXPECT_EQ(engineCopies[1].src.offset, uint64_t(1920) * 1080);
    EXPECT_EQ(engineCopies[1].dst.offset, bounceSlot.buffers[0].desc.planeOffset[1]);
    ASSERT_EQ(declaredUses.size(), 3u);
    EXPECT_EQ(declaredUses[1].backing, &scratchBacking);
    EXPECT_EQ(declaredUses[2].backing, &sourcePitch);
    EXPECT_EQ(method(TEGRA_VIC_M_SURFACE0_SLOT0_LUMA), 0x60000000u >> 8);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, SURF_LUMA_WIDTH), 2047u);
    EXPECT_EQ(mappings, 1);  // the destination's; the scratch's mapping is the pool's
}

// A source the GPU keeps block linear is bounced too, whole planes (the copy engine's only form for such a side), even
// with its rows on VIC's grid, and only the region is then read from the scratch.
TEST_F(SwitchVICEncodeInputTest, AGpuTiledSourceIsBouncedWholePlanes)
{
    sourcePitchLayout = false;
    input.y = 8;
    input.h = 1072;
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), 0) << why;
    EXPECT_TRUE(bounced);
    ASSERT_EQ(engineCopies.size(), 2u);
    EXPECT_EQ(engineCopies[0].src.offset, 0u);
    EXPECT_EQ(engineCopies[0].rows, 1080u);
    EXPECT_EQ(engineCopies[1].src.offset, uint64_t(2048) * 1080);
    EXPECT_EQ(engineCopies[1].rows, 540u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP), uint64_t(8) << 16);
}

TEST_F(SwitchVICEncodeInputTest, RefusesWithoutQueueingAndReleasesEverything)
{
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const char *why = nullptr;
    found = false;
    EXPECT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), -22);
    found = true;
    input.chromaRows = 600;  // not whole sixteen-GOB blocks
    EXPECT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), -95);
    EXPECT_STREQ(why, "block-linear plane of partial blocks");
    input.chromaRows = kChromaRows;
    vicUp = false;
    EXPECT_EQ(switch_vic_encode_input(77, nullptr, &input, &fence, &written, &bounced, &why), -19);
    EXPECT_EQ(pending, nullptr);
}
}  // namespace
