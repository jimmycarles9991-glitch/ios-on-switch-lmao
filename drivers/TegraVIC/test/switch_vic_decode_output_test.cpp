// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchVICDecodeOutput.cpp: the job VIC runs for NVJPG's planar 4:2:2 and 4:4:4 decodes, against stubs of the surface
// access, mappings and queue. The decoder's three planes, at 256-byte pitches in its own buffer, go into a client's
// '422f' or '444f' surface at the request's place: the source is VIC's planar format of the same subsampling, every
// plane at its buffer address, the destination its semi-planar format, no matrix on either side (the channels pass
// through), a 1-tap copy, and the job waits for the decoder's syncpoint on the hardware.
#include <gtest/gtest.h>
#include <vector>
#include "switch_vic_backing_stubs.h"

inline void IOLog(const char *, ...)
{}

#include "../SwitchVICDecodeOutput.cpp"
#include "vic_model.h"

struct SwitchVICMapping {
    uint64_t address;
};

namespace {
OSObject surfaceObject;
IOMemoryDescriptor surfaceMemory;
switch_gpu_backing destinationBacking, decoded;
volatile uint64_t destinationGeneration = 1;
bool vicUp = true, rows = true;
int mappings = 0, accesses = 0, completions = 0, lastStatus = 1;
void (*pending)(void *, int);
void *pendingContext;
TegraVICJob submittedJob;
TegraVICAcquire submittedAfter;
bool hadAfter;
std::vector<switch_gpu_backing_use> declaredUses;

void heard(void *context, int status)
{
    EXPECT_EQ(context, &completions);
    ++completions;
    lastStatus = status;
}
}  // namespace

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
    return rows;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    static uint64_t pages[1] = {0x100000};
    *info = {};
    info->pages = pages;
    info->page_count = backing == &decoded ? (4u << 20) >> 12 : 1024;
}

int switch_vic_surface_access(const TegraVICSurface *surface, bool write, SwitchVICSurfaceAccess *access, bool,
                              switch_gpu_backing *)
{
    EXPECT_TRUE(write);
    ++accesses;
    access->descriptor = surface->memory;
    access->backing = &destinationBacking;
    access->guard = {&destinationBacking, &destinationGeneration, 1};
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
    EXPECT_EQ(write, backing == &destinationBacking);
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

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *after,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events)
{
    EXPECT_EQ(events, nullptr);
    EXPECT_EQ(count, 1u);
    submittedJob = jobs[0];
    hadAfter = after != nullptr;
    if (after)
        submittedAfter = *after;
    declaredUses.assign(ownership.uses, ownership.uses + ownership.useCount);
    EXPECT_EQ(ownership.guardCount, 1u);
    pending = completion;
    pendingContext = context;
    *fence = 4321;
    return 0;
}

namespace {
constexpr uint32_t kWidth = 300, kHeight = 200;

// A CoreVideo-shaped '422f' or '444f' surface 320 pixels wide: 64-byte rows, the CbCr plane after the luma.
TegraVICSurface destination(bool f444)
{
    TegraVICSurface s = {};
    s.surface = &surfaceObject;
    s.memory = &surfaceMemory;
    s.id = 77;
    TegraVICSurfaceDesc &d = s.desc;
    d.fourcc = f444 ? TEGRA_VIC_FOURCC('4', '4', '4', 'f') : TEGRA_VIC_FOURCC('4', '2', '2', 'f');
    d.width = 320;
    d.height = 240;
    d.planes = 2;
    d.planeBytesPerRow[0] = 320;
    d.planeBytesPerRow[1] = f444 ? 640 : 320;
    d.planeHeight[0] = d.planeHeight[1] = 240;
    d.planeOffset[1] = 320 * 240;
    d.length = d.planeOffset[1] + uint64_t(d.planeBytesPerRow[1]) * 240;
    return s;
}

class SwitchVICDecodeOutputTest : public testing::Test {
protected:
    TegraVICAcquire decoder = {14, 777};
    SwitchVICDecodeOutput output = {};

    void SetUp() override
    {
        vicUp = rows = true;
        mappings = accesses = completions = 0;
        lastStatus = 1;
        pending = nullptr;
        hadAfter = false;
        declaredUses.clear();
    }

    void TearDown() override
    {
        if (pending)
            pending(pendingContext, 0);
        EXPECT_EQ(mappings, 0);
        EXPECT_EQ(accesses, 0);
    }

    // NVJPG's planar picture of 304x208 (whole MCUs) at 256-byte pitches, chroma planes after the luma.
    void planar(bool f444)
    {
        const uint32_t chroma = f444 ? 512 : 256;
        output.source = &decoded;
        output.planeOffset[0] = 0;
        output.planeOffset[1] = 512ull * 208;
        output.planeOffset[2] = output.planeOffset[1] + uint64_t(chroma) * 208;
        output.pitch[0] = 512;
        output.pitch[1] = output.pitch[2] = chroma;
        output.rows[0] = output.rows[1] = output.rows[2] = 208;
        output.chroma444 = f444 ? 1 : 0;
        output.width = kWidth;
        output.height = kHeight;
        output.x = 16;
        output.y = 8;
        output.after = &decoder;
    }

    uint32_t method(uint32_t m) const
    {
        for (uint32_t i = 0; i < submittedJob.count; ++i) {
            if (submittedJob.method[i] == m)
                return submittedJob.value[i];
        }
        return 0xffffffffu;
    }

    void expectRepack(bool f444)
    {
        const uint32_t slot = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, slot, SURF_FORMAT),
                  uint64_t(f444 ? TEGRA_VIC_T_Y8___U8___V8_N444 : TEGRA_VIC_T_Y8___U8___V8_N422));
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT),
                  uint64_t(f444 ? TEGRA_VIC_T_Y8___U8V8_N444 : TEGRA_VIC_T_Y8___U8V8_N422));
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND),
                  uint64_t(TEGRA_VIC_BLK_PITCH));
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, slot, SURF_CHROMA_LOC_HORIZ),
                  TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ))
            << "the same siting both sides: chroma samples land where they were";
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_FILTER_X), uint64_t(TEGRA_VIC_FILTER_1TAP));
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_SOURCE_RIGHT), uint64_t(kWidth - 1) << 16);
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_DEST_LEFT), 16u);
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_DEST_TOP), 8u);
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_DEST_RIGHT), 16u + kWidth - 1);
        EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS, SLOT_DEST_BOTTOM), 8u + kHeight - 1);
        for (const uint32_t matrix :
             {uint32_t(TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX), uint32_t(TEGRA_VIC_OUT_MATRIX)}) {
            EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, matrix, MATRIX_C00), 0u)
                << "no matrix: the channels pass through";
            EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, matrix, MATRIX_C11), 0u);
            EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, matrix, MATRIX_C22), 0u);
        }
        EXPECT_EQ(method(TEGRA_VIC_M_SURFACE0_SLOT0_LUMA), 0x40000000u >> 8);
        EXPECT_EQ(method(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U), uint32_t((0x40000000u + output.planeOffset[2]) >> 8))
            << "VIC's U is Cr on this unit";
        EXPECT_EQ(method(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_V), uint32_t((0x40000000u + output.planeOffset[1]) >> 8));
        EXPECT_EQ(method(TEGRA_VIC_M_OUTPUT_LUMA), 0x50000000u >> 8);
        EXPECT_EQ(method(TEGRA_VIC_M_OUTPUT_CHROMA_U), uint32_t((0x50000000u + 320 * 240) >> 8));
        ASSERT_TRUE(hadAfter);
        EXPECT_EQ(submittedAfter.syncpt, 14u);
        EXPECT_EQ(submittedAfter.threshold, 777u);
        ASSERT_EQ(declaredUses.size(), 2u);
        EXPECT_EQ(declaredUses[0].backing, &destinationBacking);
        EXPECT_EQ(declaredUses[0].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
        EXPECT_EQ(declaredUses[1].backing, &decoded);
        EXPECT_EQ(declaredUses[1].access, uint32_t(SWITCH_GPU_BACKING_READ));
    }

    // The submitted job on vic_model.h: every destination pair holds the decoder's Cb then its Cr, as '422f' and '444f'
    // keep them, and the luma lands unchanged at the request's place.
    void expectSamplesInPlace(bool f444)
    {
        const uint32_t chromaPitch = output.pitch[1];
        std::vector<uint8_t> source(output.planeOffset[2] + uint64_t(chromaPitch) * output.rows[2]);
        for (uint32_t y = 0; y < output.rows[0]; ++y)
            for (uint32_t x = 0; x < output.pitch[0]; ++x)
                source[y * output.pitch[0] + x] = uint8_t(16 + (x * 3 + y) % 220);
        for (uint32_t y = 0; y < output.rows[1]; ++y)
            for (uint32_t x = 0; x < chromaPitch; ++x) {
                source[output.planeOffset[1] + y * chromaPitch + x] = uint8_t(0x20 + (x + y) % 64);
                source[output.planeOffset[2] + y * chromaPitch + x] = uint8_t(0xa0 + (x + 2 * y) % 64);
            }
        const TegraVICSurface d = destination(f444);
        std::vector<uint8_t> target(d.desc.length);
        VicModel model = {};
        model.region[0] = {0x40000000, source.data(), source.size()};
        model.region[1] = {0x50000000, target.data(), target.size()};
        model.regions = 2;
        vic_model_run(&model, &submittedJob);
        EXPECT_EQ(model.faults, 0u);
        const uint32_t chromaRow = d.desc.planeBytesPerRow[1];
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x)
                ASSERT_EQ(target[(8 + y) * 320 + 16 + x], source[y * output.pitch[0] + x]) << x << "," << y;
            for (uint32_t c = 0; c < (f444 ? kWidth : kWidth / 2); ++c) {
                const uint8_t *pair = &target[d.desc.planeOffset[1] + (8 + y) * chromaRow + ((f444 ? 16 : 8) + c) * 2];
                ASSERT_EQ(pair[0], source[output.planeOffset[1] + y * chromaPitch + c]) << "Cb first, pair " << c;
                ASSERT_EQ(pair[1], source[output.planeOffset[2] + y * chromaPitch + c]) << "Cr second, pair " << c;
            }
        }
    }
};

TEST_F(SwitchVICDecodeOutputTest, RepacksAPlanar422DecodeInto422fAfterTheDecoder)
{
    planar(false);
    const TegraVICSurface d = destination(false);
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), 0) << why;
    expectRepack(false);
    pending(pendingContext, 0);
    pending = nullptr;
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(lastStatus, 0);
}

TEST_F(SwitchVICDecodeOutputTest, RepacksAPlanar444DecodeInto444fAfterTheDecoder)
{
    planar(true);
    const TegraVICSurface d = destination(true);
    const char *why = nullptr;
    ASSERT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), 0) << why;
    expectRepack(true);
    pending(pendingContext, -5);
    pending = nullptr;
    EXPECT_EQ(lastStatus, -5) << "the job's failure is the decode's";
}

TEST_F(SwitchVICDecodeOutputTest, PlacesCbAndCrWhereThe422fAnd444fPairsHoldThem)
{
    for (const bool f444 : {false, true}) {
        SCOPED_TRACE(f444 ? "444f" : "422f");
        planar(f444);
        const TegraVICSurface d = destination(f444);
        const char *why = nullptr;
        ASSERT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), 0) << why;
        expectSamplesInPlace(f444);
        pending(pendingContext, 0);
        pending = nullptr;
    }
}

TEST_F(SwitchVICDecodeOutputTest, RefusesASamplingTheDestinationDoesNotHoldAndALayoutNotOfRows)
{
    planar(true);
    TegraVICSurface d = destination(false);
    const char *why = nullptr;
    EXPECT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), -95);
    EXPECT_STREQ(why, "destination is not the decode's full-range semi-planar sampling");
    planar(false);
    rows = false;
    EXPECT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), -95);
    EXPECT_STREQ(why, "a destination not laid out as rows");
    rows = true;
    output.planeOffset[2] = (4u << 20) - 100;
    EXPECT_EQ(switch_vic_decode_output(&d, &output, heard, &completions, &why), -95);
    EXPECT_STREQ(why, "decoded plane outside its buffer");
    EXPECT_EQ(pending, nullptr);
    EXPECT_EQ(completions, 0);
}
}  // namespace
