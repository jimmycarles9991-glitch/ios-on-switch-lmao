// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_lock.h"
#include "../raw/runtime/raw_native_plane.h"
#include <gtest/gtest.h>
#include <IOKit/IOReturn.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace raw {
namespace fx {
constexpr unsigned kTexTwoD = 2;
}
struct NativeSurface;

struct Bo {
    uint64_t handle = 0, va = 0, bytes = 0;
};

struct Import {
    Bo bo;
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
    uint32_t lastUse = 0, encodingReferences = 0;
    bool cached = true;
};

struct Kernel {
    TegraGM20BAbiNativeSurfaceViewReply next = {};
    int result = KERN_SUCCESS;
    unsigned queries = 0, frees = 0;
    bool views = true;

    bool exactJobs() const
    {
        return views;
    }

    int nativeSurfaceView(const Bo &, Bo &bo, TegraGM20BAbiNativeSurfaceViewReply &layout)
    {
        ++queries;
        layout = next;
        bo = {next.handle, next.handle << 24, next.bytes};
        return result;
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
    raw::Mutex lock_;
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
    void requirePitch(const Import *);
    const Texture *readTexture(const Texture *);
    void invalidateNativeReads();
    void releaseNativeReads();
};

constexpr uint32_t kStageTextures = 31;  // raw_pack.h

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
}  // namespace raw

#include "../raw/runtime/raw_native_surface.h"

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

class RawNativeSurface : public ::testing::Test {
protected:
    raw::Device device;
    raw::Import source;
    raw::Texture y;
    raw::CommandBuffer cb;

    void SetUp() override
    {
        device.k_.next = layout();
        device.surfaces_[1] = &source;
        y.import = &source;
        y.nativePlane = 0;
        y.va = 0x123000;
        y.tic = 9;
        cb.dev_ = &device;
    }
};

TEST_F(RawNativeSurface, UnchangedReadsReuseOneMappingAndHeader)
{
    const auto *native = cb.readTexture(&y);
    ASSERT_NE(native, &y);
    ASSERT_EQ(native->import, nullptr);
    EXPECT_TRUE(native->blockLinear);
    EXPECT_NE(native->va, y.va);
    EXPECT_NE(native->nativeGeneration, 0u);
    raw::RenderEncoder encoder;
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

TEST_F(RawNativeSurface, StaleReplacementKeepsOldDescriptorsUntilExactRetirement)
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
    raw::CommandBuffer next;
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
    raw::CommandBuffer final;
    final.dev_ = &device;
    EXPECT_TRUE(final.readTexture(&y)->blockLinear);
    EXPECT_EQ(device.k_.queries, 3u);
}

TEST_F(RawNativeSurface, WritesAndExplicitPitchAliasesPreventNativeSelection)
{
    cb.requirePitch(&source);
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 0u);
    raw::CommandBuffer other;
    other.dev_ = &device;
    ASSERT_NE(other.readTexture(&y), &y);
    other.requirePitch(&source);
    EXPECT_EQ(other.readTexture(&y), &y);
}

TEST_F(RawNativeSurface, ChromaAndSwizzleGetImmutableDistinctHeadersOverOneProxy)
{
    raw::Texture uv = y;
    uv.nativePlane = 1;
    uv.bpp = 2;
    uv.width /= 2;
    uv.height /= 2;
    uv.format = 30;
    const auto *luma = cb.readTexture(&y);
    const auto *chroma = cb.readTexture(&uv);
    ASSERT_NE(chroma, &uv);
    EXPECT_EQ(chroma->va - luma->va, layout().planes[1].offset);
    raw::Texture swizzle = y;
    swizzle.swizzle[0] = 5;
    const auto *swizzled = cb.readTexture(&swizzle);
    EXPECT_NE(swizzled->tic, luma->tic);
    EXPECT_EQ(swizzled->swizzle[0], 5u);
    EXPECT_EQ(luma->swizzle[0], 2u);
    EXPECT_EQ(device.k_.queries, 1u);
}

TEST_F(RawNativeSurface, UnsupportedRepresentationIsRequeriedOnLaterCommandBuffers)
{
    device.k_.next.planes[0].pitch = 1536;
    device.k_.next.planes[0].bytes = 1536 * 768;
    device.k_.next.bytes += 256 * 768;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeStale_);
    device.reapNativeSurfaces();
    device.k_.next = layout();
    raw::CommandBuffer next;
    next.dev_ = &device;
    EXPECT_NE(next.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 2u);
}

// A kernel without SubmitAddress has no native views: no query, no lock, no surface made, per read
TEST_F(RawNativeSurface, AKernelWithoutViewsReadsTheSourceWithoutAQuery)
{
    device.k_.views = false;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(device.k_.queries, 0u);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeSurfaces_.empty());
}

TEST_F(RawNativeSurface, FailedProducerIsReportedAndNeverCachedAsOrdinarySuccess)
{
    device.k_.result = kIOReturnAborted;
    EXPECT_EQ(cb.readTexture(&y), &y);
    EXPECT_EQ(cb.nativeError, kIOReturnAborted);
    EXPECT_EQ(source.native, nullptr);
    EXPECT_TRUE(device.nativeSurfaces_.empty());
}

TEST_F(RawNativeSurface, ChannelGenerationRetiresOnlyAfterCallerQuiescesOldChannel)
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

TEST_F(RawNativeSurface, IdleReclamationCannotFreeAnUnsubmittedView)
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

TEST(RawNativePlane, QualifiesHardwareStrideCropBlockHeightAndBounds)
{
    auto view = layout();
    EXPECT_TRUE(raw::nativePlaneFits(view, 0, 1280, 720, 1));
    EXPECT_TRUE(raw::nativePlaneFits(view, 1, 640, 360, 2));
    EXPECT_FALSE(raw::nativePlaneFits(view, 0, 1200, 720, 1));
    EXPECT_FALSE(raw::nativePlaneFits(view, 1, 640, 16, 2));
    EXPECT_FALSE(raw::nativePlaneFits(view, 2, 640, 360, 2));
    EXPECT_FALSE(raw::nativePlaneFits(view, 0, 1280, 720, 2));
    view.crop_x = 1;
    EXPECT_FALSE(raw::nativePlaneFits(view, 0, 1280, 720, 1));
    view.crop_x = 0;
    view.planes[1].offset = UINT64_MAX;
    EXPECT_FALSE(raw::nativePlaneFits(view, 1, 640, 360, 2));
    view = layout(0);
    EXPECT_TRUE(raw::nativePlaneFits(view, 0, 1200, 720, 1));
    view.planes[0].pitch += 16;
    EXPECT_FALSE(raw::nativePlaneFits(view, 0, 1200, 720, 1));
}
}  // namespace
