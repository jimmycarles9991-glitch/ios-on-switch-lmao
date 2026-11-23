// SPDX-License-Identifier: MIT
#include "../lock.h"
#include "../native_plane.h"
#include <gtest/gtest.h>
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace gm20b {
namespace fx {
constexpr unsigned kTexTwoD = 2;
}
struct NativeSurface;

struct Bo {
    uint64_t handle = 0, va = 0, bytes = 0;
};

struct Import {
    Bo bo;
    const void *owner = nullptr;
    NativeSurface *native = nullptr;
};

struct Texture {
    Import *import = nullptr;
    void *chain = nullptr;
    uint32_t nativePlane = ~0u, layers = 1, depth = 1, samples = 1, blockW = 1, blockH = 1;
    uint32_t type = fx::kTexTwoD, width = 1280, height = 720, format = 10, bpp = 1;
    uint64_t va = 0, layerBytes = 0, nativeGeneration = 0;
    uint32_t pitch = 0, tic = 0, blockHeight = 0;
    bool blockLinear = false, renderable = false;
    uint8_t swizzle[4] = {2, 3, 4, 5};
};

struct NativeSurface {
    Bo bo;
    TegraGM20BAbiNativeSurfaceViewReply layout = {};
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<Key, Texture> textures;
    uint64_t channelGeneration = 0, generation = 0;
    uint32_t lastUse = 0, encodingReferences = 0, seed = 0;
    bool cached = true;
};

// A view's guard is the surface's representation word as the kernel made the view (nativeSurfaceView), which the reply
// carries; a job reading through it is refused once the word moved
struct Kernel {
    TegraGM20BAbiNativeSurfaceViewReply next = {};
    int result = KERN_SUCCESS;
    unsigned queries = 0, frees = 0, jobs = 0, refused = 0;
    bool views = true;
    IOSurfaceRef surface = nullptr;
    uint64_t representation = 1;

    bool exactJobs() const
    {
        return views;
    }

    int nativeSurfaceView(const Bo &, Bo &bo, TegraGM20BAbiNativeSurfaceViewReply &layout)
    {
        ++queries;
        layout = next;
        layout.representation_generation = representation;
        bo = {next.handle, next.handle << 24, next.bytes};
        return result;
    }

    // The decoder's next picture published into the surface (switch_native_surface_publish): the guards move, then the
    // engine's write end moves the seed (tegra_vic_surface_written), as a write lock does here
    void publish()
    {
        ++representation;
        IOSurfaceLock(surface, 0, nullptr);
        IOSurfaceUnlock(surface, 0, nullptr);
    }

    void free(Bo &bo)
    {
        ++frees;
        bo = {};
    }
};

bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &out)
{
    out = {};
    out.width = width;
    out.height = height;
    out.format = format;
    out.bpp = format == 30 ? 2 : 1;
    return true;
}

struct Device {
    Kernel k_;
    gm20b::Mutex lock_;
    std::vector<std::unique_ptr<NativeSurface>> nativeSurfaces_;
    std::atomic<bool> nativeStale_{false};
    std::map<unsigned, Import *> surfaces_;
    uint64_t nativeGeneration_ = 0, currentGeneration = 1;
    uint32_t completed = 0, nextTic = 0, freedTics = 0, writes = 0;

    uint32_t allocTic()
    {
        return ++nextTic;
    }

    void freeTic(uint32_t)
    {
        ++freedTics;
    }

    void writeTic(const Texture &)
    {
        ++writes;
    }

    bool done(uint32_t sequence) const
    {
        return !sequence || int32_t(completed - sequence) >= 0;
    }

    uint64_t channelGeneration() const
    {
        return currentGeneration;
    }

    const Texture *nativeRead(const Texture &, NativeSurface *&, int &);
    void reapNativeSurfaces(bool = false);
};

struct CommandBuffer {
    Device *dev_ = nullptr;
    int nativeError = 0;
    std::map<const Texture *, const Texture *> nativeReads_;
    std::vector<NativeSurface *> nativeSurfaces_;
    std::vector<const Import *> pitchImports_;
    void requirePitch(const Import *, bool);
    bool pitchAfterNativeRead() const;
    bool nativeReadsCurrent() const;
    const Texture *readTexture(const Texture *);
    void invalidateNativeReads();
    void releaseNativeReads();
};

constexpr uint32_t kStageTextures = 31;  // stage_textures.h on 21H461

struct RenderEncoder {
    CommandBuffer *cb_ = nullptr;
    const Texture *textures_[2][kStageTextures] = {};
    unsigned dirties = 0;

    void textureMoved(uint32_t, uint32_t)
    {
        ++dirties;
    }

    void setTexture(int, uint32_t, const Texture *);
};
}  // namespace gm20b

#include "../native_surface.h"

namespace {
TegraGM20BAbiNativeSurfaceViewReply layout(uint32_t kind = TGM_LAYOUT_KIND)
{
    TegraGM20BAbiNativeSurfaceViewReply out = {};
    out.handle = 101;
    out.format = 0x34323076;
    out.kind = kind;
    out.plane_count = 2;
    out.crop_width = 1280;
    out.crop_height = 720;
    out.planes[0] = {0, 1280 * 768, 1280, 768, 1280, 1, 4, 0};
    out.planes[1] = {1280 * 768, 1280 * 384, 640, 384, 1280, 2, 4, 0};
    out.bytes = out.planes[0].bytes + out.planes[1].bytes;
    return out;
}

IOSurfaceRef makeSurface()
{
    int width = 1280, height = 720, bytes = 1;
    CFNumberRef w = CFNumberCreate(nullptr, kCFNumberIntType, &width);
    CFNumberRef h = CFNumberCreate(nullptr, kCFNumberIntType, &height);
    CFNumberRef b = CFNumberCreate(nullptr, kCFNumberIntType, &bytes);
    const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement};
    const void *values[] = {w, h, b};
    CFDictionaryRef d =
        CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(d);
    for (CFNumberRef n : {w, h, b})
        CFRelease(n);
    return s;
}

// SubmitJob over what `cb` read and wrote: refused kIOReturnNotReady where a native view's guard moved (runJob's backings);
// a write of the surface prepares it mutably, which moves the guard and not the seed (SwitchGpuSurfaceRefresh)
int submit(gm20b::Device &device, gm20b::CommandBuffer &cb)
{
    gm20b::Kernel &k = device.k_;
    const bool stale =
        std::any_of(cb.nativeSurfaces_.begin(), cb.nativeSurfaces_.end(), [&](const gm20b::NativeSurface *s) {
            return s->layout.representation_generation != k.representation;
        });
    cb.releaseNativeReads();
    if (stale) {
        ++k.refused;
        return kIOReturnNotReady;
    }
    ++k.jobs;
    k.representation += !cb.pitchImports_.empty();
    return KERN_SUCCESS;
}

class GM20BNativeSurface : public ::testing::Test {
protected:
    gm20b::Device device;
    gm20b::Import source;
    gm20b::Texture y;
    gm20b::CommandBuffer cb;

    void SetUp() override
    {
        device.k_.next = layout();
        device.k_.surface = makeSurface();
        ASSERT_NE(device.k_.surface, nullptr);
        source.owner = device.k_.surface;
        device.surfaces_[1] = &source;
        y.import = &source;
        y.nativePlane = 0;
        y.va = 0x123000;
        y.tic = 9;
        cb.dev_ = &device;
    }

    void TearDown() override
    {
        if (device.k_.surface)
            CFRelease(device.k_.surface);
    }
};

// The decoder publishes its next picture into the surface between two command buffers: the second reads it through a view
// the kernel made after the publication, and its job is not refused
TEST_F(GM20BNativeSurface, ARepublishedSurfaceIsReadThroughAFreshView)
{
    ASSERT_NE(cb.readTexture(&y), &y);
    ASSERT_EQ(submit(device, cb), KERN_SUCCESS);
    device.k_.publish();
    gm20b::CommandBuffer next;
    next.dev_ = &device;
    ASSERT_NE(next.readTexture(&y), &y);
    EXPECT_EQ(submit(device, next), KERN_SUCCESS);
    EXPECT_EQ(device.k_.queries, 2u);
    EXPECT_EQ(device.k_.jobs, 2u);
    EXPECT_EQ(device.k_.refused, 0u);
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 1u);
    EXPECT_EQ(device.nativeSurfaces_.size(), 1u);
}

// The view made after a publication stays the surface's while nothing writes it: a CPU lock that only reads leaves the
// seed, and later command buffers read through that view without a new one
TEST_F(GM20BNativeSurface, AnUnchangedSurfaceKeepsTheViewMadeAfterItsPublication)
{
    ASSERT_NE(cb.readTexture(&y), &y);
    ASSERT_EQ(submit(device, cb), KERN_SUCCESS);
    device.k_.publish();
    gm20b::CommandBuffer fresh;
    fresh.dev_ = &device;
    const gm20b::Texture *view = fresh.readTexture(&y);
    ASSERT_EQ(submit(device, fresh), KERN_SUCCESS);
    IOSurfaceLock(device.k_.surface, kIOSurfaceLockReadOnly, nullptr);
    IOSurfaceUnlock(device.k_.surface, kIOSurfaceLockReadOnly, nullptr);
    for (int i = 0; i < 3; ++i) {
        gm20b::CommandBuffer later;
        later.dev_ = &device;
        EXPECT_EQ(later.readTexture(&y), view);
        EXPECT_EQ(submit(device, later), KERN_SUCCESS);
    }
    EXPECT_EQ(device.k_.queries, 2u);
    EXPECT_EQ(device.k_.refused, 0u);
}

// A write of the surface this process submits invalidates its publication and leaves its seed: the next read is through a
// new view, not refused
TEST_F(GM20BNativeSurface, ASurfaceThisProcessWroteIsReadThroughAFreshView)
{
    ASSERT_NE(cb.readTexture(&y), &y);
    ASSERT_EQ(submit(device, cb), KERN_SUCCESS);
    gm20b::CommandBuffer writes;
    writes.dev_ = &device;
    writes.requirePitch(&source, true);
    EXPECT_EQ(writes.readTexture(&y), &y);
    ASSERT_EQ(submit(device, writes), KERN_SUCCESS);
    gm20b::CommandBuffer reads;
    reads.dev_ = &device;
    ASSERT_NE(reads.readTexture(&y), &y);
    EXPECT_EQ(submit(device, reads), KERN_SUCCESS);
    EXPECT_EQ(device.k_.queries, 2u);
    EXPECT_EQ(device.k_.refused, 0u);
}

// An encode ahead reads the surface through its view before the commit: at the commit the view is current while the
// surface is unchanged, a CPU lock that only reads included, and not once the decoder published into it meanwhile
TEST_F(GM20BNativeSurface, AnEncodeAheadsViewsAreCurrentOnlyWhileTheSurfaceIsUnchanged)
{
    ASSERT_NE(cb.readTexture(&y), &y);
    EXPECT_TRUE(cb.nativeReadsCurrent());
    IOSurfaceLock(device.k_.surface, kIOSurfaceLockReadOnly, nullptr);
    IOSurfaceUnlock(device.k_.surface, kIOSurfaceLockReadOnly, nullptr);
    EXPECT_TRUE(cb.nativeReadsCurrent());
    device.k_.publish();
    EXPECT_FALSE(cb.nativeReadsCurrent());
}

TEST_F(GM20BNativeSurface, UnchangedReadsReuseOneMappingAndHeader)
{
    const auto *native = cb.readTexture(&y);
    ASSERT_NE(native, &y);
    ASSERT_EQ(native->import, nullptr);
    EXPECT_TRUE(native->blockLinear);
    EXPECT_NE(native->va, y.va);
    EXPECT_NE(native->nativeGeneration, 0u);
    gm20b::RenderEncoder encoder;
    encoder.cb_ = &cb;
    for (unsigned i = 0; i < 10000; ++i) {
        EXPECT_EQ(cb.readTexture(&y), native);
        encoder.setTexture(2, 0, &y);
    }
    EXPECT_EQ(device.k_.queries, 1u);
    EXPECT_EQ(device.writes, 1u);
    EXPECT_EQ(encoder.dirties, 1u);
    EXPECT_EQ(cb.nativeSurfaces_.size(), 1u);
    EXPECT_EQ(y.va, 0x123000u);
    EXPECT_EQ(y.tic, 9u);
}

TEST_F(GM20BNativeSurface, StaleReplacementKeepsOldDescriptorsUntilExactRetirement)
{
    const auto *native = cb.readTexture(&y);
    const uint64_t oldAddress = native->va;
    auto *held = source.native;
    held->lastUse = 0xfffffffe;
    held->channelGeneration = 1;
    device.completed = 0xfffffffd;
    EXPECT_FALSE(device.nativeStale_);
    cb.invalidateNativeReads();
    cb.releaseNativeReads();
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeStale_);  // begin() reaps under the device's lock only then
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 0u);
    device.k_.next = layout(0);
    device.k_.next.handle++;
    gm20b::CommandBuffer next;
    next.dev_ = &device;
    const auto *pitch = next.readTexture(&y);
    ASSERT_NE(pitch, &y);
    EXPECT_FALSE(pitch->blockLinear);
    EXPECT_NE(pitch->va, oldAddress);
    EXPECT_NE(pitch->nativeGeneration, native->nativeGeneration);
    EXPECT_EQ(native->va, oldAddress);
    device.completed = 1;
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 1u);
    EXPECT_EQ(device.freedTics, 1u);
    next.invalidateNativeReads();
    next.releaseNativeReads();
    device.k_.next = layout();
    device.k_.next.handle += 2;
    gm20b::CommandBuffer final;
    final.dev_ = &device;
    EXPECT_TRUE(final.readTexture(&y)->blockLinear);
    EXPECT_EQ(device.k_.queries, 3u);
}

TEST_F(GM20BNativeSurface, WritesAndExplicitPitchAliasesPreventNativeSelection)
{
    cb.requirePitch(&source, true);
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 0u);
    EXPECT_FALSE(cb.pitchAfterNativeRead());
    gm20b::CommandBuffer other;
    other.dev_ = &device;
    ASSERT_NE(other.readTexture(&y), &y);
    EXPECT_FALSE(other.pitchAfterNativeRead());
    other.requirePitch(&source, false);
    EXPECT_EQ(other.readTexture(&y), &y);
    EXPECT_TRUE(other.pitchAfterNativeRead());
}

TEST_F(GM20BNativeSurface, ChromaAndSwizzleGetImmutableDistinctHeadersOverOneProxy)
{
    gm20b::Texture uv = y;
    uv.nativePlane = 1;
    uv.bpp = 2;
    uv.width /= 2;
    uv.height /= 2;
    uv.format = 30;
    const auto *luma = cb.readTexture(&y);
    const auto *chroma = cb.readTexture(&uv);
    ASSERT_NE(chroma, &uv);
    EXPECT_EQ(chroma->va - luma->va, layout().planes[1].offset);
    gm20b::Texture swizzle = y;
    swizzle.swizzle[0] = 5;
    const auto *swizzled = cb.readTexture(&swizzle);
    EXPECT_NE(swizzled->tic, luma->tic);
    EXPECT_EQ(swizzled->swizzle[0], 5u);
    EXPECT_EQ(luma->swizzle[0], 2u);
    EXPECT_EQ(device.k_.queries, 1u);
}

TEST_F(GM20BNativeSurface, UnsupportedRepresentationIsRequeriedOnLaterCommandBuffers)
{
    device.k_.next.planes[0].pitch = 1536;
    device.k_.next.planes[0].bytes = 1536 * 768;
    device.k_.next.bytes += 256 * 768;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeStale_);
    device.reapNativeSurfaces();
    device.k_.next = layout();
    gm20b::CommandBuffer next;
    next.dev_ = &device;
    EXPECT_NE(next.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 2u);
}

// A kernel without SubmitAddress has no native views: no query, no lock, no surface made, per read
TEST_F(GM20BNativeSurface, AKernelWithoutViewsReadsTheSourceWithoutAQuery)
{
    device.k_.views = false;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 0u);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeSurfaces_.empty());
}

TEST_F(GM20BNativeSurface, FailedProducerIsReportedAndNeverCachedAsOrdinarySuccess)
{
    device.k_.result = kIOReturnAborted;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(cb.nativeError, kIOReturnAborted);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeSurfaces_.empty());
}

TEST_F(GM20BNativeSurface, ChannelGenerationRetiresOnlyAfterCallerQuiescesOldChannel)
{
    cb.readTexture(&y);
    source.native->lastUse = 7;
    source.native->channelGeneration = 1;
    cb.invalidateNativeReads();
    cb.releaseNativeReads();
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 0u);
    device.currentGeneration = 2;
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 1u);
}

TEST_F(GM20BNativeSurface, IdleReclamationCannotFreeAnUnsubmittedView)
{
    const auto *native = cb.readTexture(&y);
    cb.invalidateNativeReads();
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 0u);
    EXPECT_NE(native->va, 0u);
    cb.releaseNativeReads();
    device.reapNativeSurfaces();
    EXPECT_EQ(device.k_.frees, 1u);
}

// A Main 10 picture ('x420', 'xf20'): its 16-bit luma and 32-bit interleaved chroma texels read in place as r16Unorm
// and rg16Unorm planes, as the 8-bit picture's are
TEST(GM20BNativePlane, QualifiesAMainTenPicturesSixteenBitPlanes)
{
    auto view = layout();
    view.format = 0x78343230;
    view.planes[0] = {0, 2560 * 768, 1280, 768, 2560, 2, 4, 0};
    view.planes[1] = {2560 * 768, 2560 * 384, 640, 384, 2560, 4, 4, 0};
    view.bytes = view.planes[0].bytes + view.planes[1].bytes;
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 0, 1280, 720, 2));
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 1, 640, 360, 4));
    view.format = 0x78663230;
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 0, 1280, 720, 2));
    view.format = 0x78343232;
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 0, 1280, 720, 2)) << "'x422' is not a 4:2:0 picture";
}

TEST(GM20BNativePlane, QualifiesHardwareStrideCropBlockHeightAndBounds)
{
    auto view = layout();
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 0, 1280, 720, 1));
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 1, 640, 360, 2));
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 0, 1200, 720, 1));
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 1, 640, 16, 2));
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 2, 640, 360, 2));
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 0, 1280, 720, 2));
    view.crop_x = 1;
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 0, 1280, 720, 1));
    view.crop_x = 0;
    view.planes[1].offset = UINT64_MAX;
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 1, 640, 360, 2));
    view = layout(0);
    EXPECT_TRUE(gm20b::nativePlaneFits(view, 0, 1200, 720, 1));
    view.planes[0].pitch += 16;
    EXPECT_FALSE(gm20b::nativePlaneFits(view, 0, 1200, 720, 1));
}
}  // namespace
