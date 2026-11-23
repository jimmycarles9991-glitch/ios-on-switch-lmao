// SPDX-License-Identifier: MIT
// Passes into RGB10A8_2P_XR10 (550) colours, over a surface's two planes or a device-made texture, and a multisampled
// BGRA10_XR resolved by its codes' mean: each renders through its RGBA16Float stand-in (format_standin.h), on the driver,
// nothing of it on m2v.
#include "facade_harness.h"
#include "../format_standin.h"
#include "../samples.h"
#include "../xr_pools.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace harness;

constexpr uint32_t kSide = 64;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

std::string path(id<MTLCommandBuffer> cb)
{
    NSString *p = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
    return p.UTF8String ?: "";
}

id<MTLTexture> planesTexture(id<MTLDevice> device)
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : kSide * kSide * 4),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(kSide * kSide * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
    if (surface)
        CFRelease(surface);
    return t;
}

id<MTLTexture> plain(id<MTLDevice> device, MTLPixelFormat format)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:kSide height:kSide mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:d];
}

void expectRaw(id<MTLDevice> device, MTLRenderPassDescriptor *p)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(path(cb), "gm20b");
}

using States = std::vector<std::pair<std::string, MTLPixelFormat>>;

// The load and store states of 550 over a surface's two planes and of a device-made 550 (pack/xr_target.metal), and the
// surface texture's sampling conversion, with `more`
States standInStates(States more = {})
{
    using gm20b::XrStep;
    States states = {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                     {gm20b::xrTargetState(550, XrStep::Load, 1, true), MTLPixelFormatRGBA16Float},
                     {gm20b::xrTargetState(550, XrStep::Store, 1, true), MTLPixelFormatR32Uint},
                     {gm20b::xrTargetState(550, XrStep::Load), MTLPixelFormatRGBA16Float},
                     {gm20b::xrTargetState(550, XrStep::Store), MTLPixelFormatRG32Uint}};
    states.insert(states.end(), more.begin(), more.end());
    return states;
}

id<MTLDevice> openWithStandIns(States more = {})
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", standInStates(std::move(more))));
}

// A surface's 550 after seven BGRA8 colours, at the last colour index: its stand-in takes that index alone
void aSurface550AtTheLastIndexRendersThroughItsStandIn()
{
    id<MTLDevice> device = openWithStandIns();
    ASSERT_TRUE(device != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger c = 0; c < 7; c++) {
        p.colorAttachments[c].texture = plain(device, MTLPixelFormatBGRA8Unorm);
        p.colorAttachments[c].loadAction = MTLLoadActionClear;
        p.colorAttachments[c].storeAction = MTLStoreActionStore;
        ASSERT_TRUE(p.colorAttachments[c].texture != nil);
    }
    p.colorAttachments[7].texture = planesTexture(device);
    ASSERT_TRUE(p.colorAttachments[7].texture != nil);
    p.colorAttachments[7].loadAction = MTLLoadActionClear;
    p.colorAttachments[7].storeAction = MTLStoreActionStore;
    expectRaw(device, p);
}

// A device-made 550 beside a surface's: each renders through a stand-in of its own and is stored in its own form
void aDeviceMade550BesideASurfacesIsRaws()
{
    id<MTLDevice> device = openWithStandIns();
    ASSERT_TRUE(device != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = planesTexture(device);
    p.colorAttachments[1].texture = plain(device, MTLPixelFormat(550));
    ASSERT_TRUE(p.colorAttachments[0].texture != nil && p.colorAttachments[1].texture != nil);
    for (NSUInteger c = 0; c < 2; c++) {
        p.colorAttachments[c].loadAction = MTLLoadActionClear;
        p.colorAttachments[c].storeAction = MTLStoreActionStore;
    }
    expectRaw(device, p);
}

// A pass that clears a BGR10_XR and a BGRA10_XR target and draws nothing writes their codes through the clear registers,
// Metal's straight route on memory it compresses: no stand-in, so a pack without their states completes it
void aDrawlessClearTakesTheClearRegisters()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = plain(device, MTLPixelFormatBGR10_XR);
    p.colorAttachments[1].texture = plain(device, MTLPixelFormatBGRA10_XR);
    ASSERT_TRUE(p.colorAttachments[0].texture != nil && p.colorAttachments[1].texture != nil);
    for (NSUInteger c = 0; c < 2; c++) {
        p.colorAttachments[c].loadAction = MTLLoadActionClear;
        p.colorAttachments[c].clearColor = MTLClearColorMake(0.5, -0.25, 1.2, 0.75);
        p.colorAttachments[c].storeAction = MTLStoreActionStore;
    }
    expectRaw(device, p);
}

// A multisampled BGRA10_XR resolves from its stand-in by the mean of its samples' store codes into a one-sample stand-in
// of the resolve texture, which the pass stores into its codes
void aMultisampledBgra10XrResolvesByItsState()
{
    using gm20b::XrStep;
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{gm20b::xrTargetState(552, XrStep::Load, 4), MTLPixelFormatRGBA16Float},
                                           {gm20b::xrTargetState(552, XrStep::Store, 4), MTLPixelFormatRGBA16Uint},
                                           {gm20b::xrTargetState(552, XrStep::Load), MTLPixelFormatRGBA16Float},
                                           {gm20b::xrTargetState(552, XrStep::Store), MTLPixelFormatRGBA16Uint},
                                           {gm20b::xrTargetState(552, XrStep::Resolve), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA10_XR
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> four = [device newTextureWithDescriptor:d];
    id<MTLTexture> into = plain(device, MTLPixelFormatBGRA10_XR);
    ASSERT_TRUE(four != nil && into != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = four;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    p.colorAttachments[0].resolveTexture = into;
    expectRaw(device, p);
}

// The copy engine's launches' destinations in the jobs from `first` on, in push order; `pitches`: each one's PITCH_OUT
std::vector<uint64_t> copiesInto(size_t first, std::vector<uint32_t> *pitches = nullptr)
{
    constexpr uint32_t kCopySubchannel = 4, kOutUpper = 0x0408, kOutLower = 0x040c, kPitchOut = 0x0414,
                       kLaunchDma = 0x0300;
    std::vector<uint64_t> out;
    uint64_t to = 0;
    uint32_t pitch = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            for (uint32_t k = 0; op != 4 && k < count && i < w.size(); ++k, ++i) {
                const uint32_t method = op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m;
                if (subc != kCopySubchannel)
                    continue;
                if (method == kOutUpper)
                    to = (to & 0xffffffffull) | uint64_t(w[i]) << 32;
                else if (method == kOutLower)
                    to = (to & ~0xffffffffull) | w[i];
                else if (method == kPitchOut)
                    pitch = w[i];
                else if (method == kLaunchDma) {
                    out.push_back(to);
                    if (pitches)
                        pitches->push_back(pitch);
                }
            }
            if (op == 4 && subc == kCopySubchannel && m == kLaunchDma) {
                out.push_back(to);
                if (pitches)
                    pitches->push_back(pitch);
            }
        }
    }
    return out;
}

// RGB10A8_2P_XR10 over words of rows 64 for 32 texels and alpha bytes of rows 16 for 32: an M4 Max makes it, reads each
// row at its plane's pitch, and its pass stores each byte from the row overlap_order.h names (five runs alike). A
// Clear pass's stand-in is stored into both planes' pools, then each plane's bytes are drawn from its pool by the
// pack's overlap state into a backing the copy engine drains into the rows (rows 64 and 16 bytes apart are no linear
// render target)
void planesWhoseRowsOverlapAreStoredFromTheirPools()
{
    id<MTLDevice> device = openWithStandIns({{"raw.ycbcr_store.ycbcr_overlap_r32ui_frag", MTLPixelFormatR32Uint},
                                             {"raw.ycbcr_store.ycbcr_overlap_r8_frag", MTLPixelFormatR8Unorm}});
    ASSERT_TRUE(device != nil);
    NSMutableArray *planes = [NSMutableArray array];
    // IOSurfaceCreate makes no plane of 32 four-byte elements in rows of 64: the words' plane is declared of two-byte
    // elements, as the M4 Max was probed over it, and Metal and the driver read 550's words at the plane's rows
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(bytes == 4 ? 32 : 16),
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes == 4 ? 2 : 1),
            (id)kIOSurfacePlaneBytesPerRow : @(bytes == 4 ? 64 : 16),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 1024),
            (id)kIOSurfacePlaneSize : @(bytes == 4 ? 512 : 128)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @2048,
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(surface);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:32 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "Metal makes it";
    ASSERT_TRUE(t->_raw.xrPlanes && t->_raw.luma->sampled && t->_raw.chroma->sampled);
    EXPECT_EQ(t->_raw.luma->pitch, 64u);
    EXPECT_EQ(t->_raw.chroma->pitch, 16u);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, -0.25, 0.75);
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, p);
    const std::vector<uint64_t> copies = copiesInto(first);
    for (const gm20b::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        EXPECT_NE(std::find(copies.begin(), copies.end(), plane->va), copies.end())
            << "plane of rows " << plane->pitch << " drawn into its rows after the pass";
    CFRelease(surface);
}

// RGB10A8_2P_XR10 32x8 over words of rows 128 at 0 and alpha bytes of rows 32 at 512, so that word rows 4 to 7 lie in the
// alpha plane: an M4 Max makes it and its pass gives each byte both planes reach to the later store across both planes. It is
// held through both pools, and a Clear pass's planes are drawn into their span by the pack's overlap planes state: a pack
// without that state fails the command buffer
void crossingPlanesCleared(bool state)
{
    States states = standInStates();
    if (state)
        states.push_back({gm20b::kOverlapPlanesState, MTLPixelFormatR8Uint});
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", states));
    ASSERT_TRUE(device != nil);
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @32,
            (id)kIOSurfacePlaneHeight : @4,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(32 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 512),
            (id)kIOSurfacePlaneSize : @(128 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @1024,
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(surface);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:32 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "Metal makes it";
    ASSERT_TRUE(t->_raw.xrPlanes && t->_raw.luma->sampled && t->_raw.chroma->sampled);
    EXPECT_TRUE(gm20b::xrPlanesCross(*t->_raw.luma, *t->_raw.chroma));
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, -0.25, 0.75);
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, state ? MTLCommandBufferStatusCompleted : MTLCommandBufferStatusError);
    CFRelease(surface);
}

// RGB10A8_2P_XR10 8x8 over words of rows 32 at 0 and alpha bytes of rows 8 at 256: an M4 Max makes it, reads the alpha
// rows 1 MiB apart and stores them 16 apart (m2v-textures11 probes/h1/rows550.txt m3), and a pass that loads the planes and
// draws nothing stores none (m2v-textures12 probes/f2/targets-full.txt #168, #169). A Clear pass stores the alpha pool at
// rows 16 apart; a Load pass with no draw neither fills the pools nor copies anything back into the planes
void planesOfRowsUnderSixteenBytesStoreSixteenApart()
{
    id<MTLDevice> device = openWithStandIns();
    ASSERT_TRUE(device != nil);
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @8,
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(8 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 256),
            (id)kIOSurfacePlaneSize : @(64 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @8,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(surface);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "Metal makes it";
    const gm20b::Texture &alpha = *t->_raw.chroma;
    EXPECT_EQ(alpha.pitch, 1u << 20);
    EXPECT_EQ(alpha.storePitch, 16u);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, -0.25, 0.75);
    size_t first = kernel_model::model.jobs.size();
    expectRaw(device, p);
    std::vector<uint32_t> pitches;
    std::vector<uint64_t> copies = copiesInto(first, &pitches);
    const auto stored = std::find(copies.begin(), copies.end(), alpha.va);
    ASSERT_NE(stored, copies.end()) << "the alpha pool stored into the plane";
    EXPECT_EQ(pitches[size_t(stored - copies.begin())], 16u) << "at rows 16 apart";

    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    first = kernel_model::model.jobs.size();
    expectRaw(device, p);
    copies = copiesInto(first);
    for (const gm20b::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        EXPECT_EQ(std::find(copies.begin(), copies.end(), plane->va), copies.end()) << "nothing stored back";
    EXPECT_EQ(std::find(copies.begin(), copies.end(), alpha.sampled->va), copies.end()) << "no pool loaded";
    CFRelease(surface);
}

// RGB10A8_2P_XR10 turned once over planes of rows under 16 bytes, its alpha rows stored on its word rows 4-7 (2x8 words of
// rows 8 at 0, alpha of rows 2 at 64): an M4 Max makes it and stores each plane 16 bytes a row apart by its frame, the
// later quad keeping the bytes both reach (m2v-textures13 probes/c4misc/RESULT.txt C1, R2, R3). It is held in its planes
// through both pools, turned, and a Clear pass draws their span through the pack's overlap planes state
void turnedPlanesOfRowsUnderSixteenBytes()
{
    id<MTLDevice> device = openWithStandIns({{gm20b::kOverlapPlanesState, MTLPixelFormatR8Uint}});
    ASSERT_TRUE(device != nil);
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @2,
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(2 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 64),
            (id)kIOSurfacePlaneSize : @(16 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @2,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_TRUE(surface);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:8 height:2 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(d, sel_registerName("setRotation:"), 1);
    SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk) << "Metal makes it";
    const gm20b::Texture &x = t->_raw;
    ASSERT_TRUE(x.xrPlanes && x.luma && x.chroma && x.luma->sampled && x.chroma->sampled);
    EXPECT_EQ(x.rotation, 1u);
    EXPECT_EQ(x.width, 2u) << "made at its plane's extent";
    EXPECT_EQ(x.luma->storePitch, 16u);
    EXPECT_EQ(x.chroma->storePitch, 16u);
    gm20b::Texture words, alpha;
    EXPECT_TRUE(gm20b::xrPlanesCross(gm20b::storedRows(*x.luma, x, words), gm20b::storedRows(*x.chroma, x, alpha)));
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, -0.25, 0.75);
    expectRaw(device, p);
    CFRelease(surface);
}

void crossingPlanesStoredTogether()
{
    crossingPlanesCleared(true);
}

void crossingPlanesWithNoStateFail()
{
    crossingPlanesCleared(false);
}

}  // namespace

TEST(GM20BFacadeXrPlanesPass, PlanesWhoseWordsRunIntoTheAlphaPlaneAreStoredTogether)
{
    switch_ios::test::ExpectPassesInFreshProcess(crossingPlanesStoredTogether);
    switch_ios::test::ExpectPassesInFreshProcess(crossingPlanesWithNoStateFail);
}

TEST(GM20BFacadeXrPlanesPass, PlanesOfRowsUnderSixteenBytesStoreSixteenApartAndNotFromALoadedPassThatDrewNothing)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesOfRowsUnderSixteenBytesStoreSixteenApart);
}

TEST(GM20BFacadeXrPlanesPass, PlanesWhoseRowsOverlapAreStoredFromTheirPools)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesWhoseRowsOverlapAreStoredFromTheirPools);
}

TEST(GM20BFacadeXrPlanesPass, AMultisampledBgra10XrResolvesByItsState)
{
    switch_ios::test::ExpectPassesInFreshProcess(aMultisampledBgra10XrResolvesByItsState);
}

TEST(GM20BFacadeXrPlanesPass, ADrawlessClearOfBgr10XrAndBgra10XrTakesTheClearRegisters)
{
    switch_ios::test::ExpectPassesInFreshProcess(aDrawlessClearTakesTheClearRegisters);
}

TEST(GM20BFacadeXrPlanesPass, ASurface550AtTheLastIndexRendersThroughItsStandIn)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSurface550AtTheLastIndexRendersThroughItsStandIn);
}

TEST(GM20BFacadeXrPlanesPass, ADeviceMade550BesideASurfacesIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aDeviceMade550BesideASurfacesIsRaws);
}

TEST(GM20BFacadeXrPlanesPass, ATurnedTextureOverPlanesOfRowsUnderSixteenBytesIsHeldInItsPools)
{
    switch_ios::test::ExpectPassesInFreshProcess(turnedPlanesOfRowsUnderSixteenBytes);
}
