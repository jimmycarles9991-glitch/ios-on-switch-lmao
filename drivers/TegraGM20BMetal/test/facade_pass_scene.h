// SPDX-License-Identifier: MIT
// What the facade tests of turned, converted and failing passes share: a device served by the test's shader compiler
// (facade_variant_compiler.h, one test binary includes it once) whose m2v device is the stand-in counting its opens
// (harness::open), the pack's own states a test adds by name, the record of a state that runs the depth test in its
// fragment program, a turned surface plane, the texture headers a test takes away, and how a command buffer ended.
#pragma once
#include "facade_variant_compiler.h"
#include "../fragment_tests.h"
#include "../record.h"
#include "../render_variant.h"
#include "../shader_tests.h"
#include "../turn_copy.h"
#include "../zeta_colour.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <unistd.h>
#include <string>

namespace pass_scene {

using namespace harness;

constexpr uint32_t kR32UintWord0 = 0x6014920f;  // colour 0's storage view: R32_UINT, a 4-byte texel as BGRA8's
constexpr uint32_t kR32FloatWord0 = 0x0f;       // the depth stand-in's view (view.h texelBytes: 4 bytes)

inline std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

inline std::string path(id<MTLCommandBuffer> cb)
{
    NSString *p = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
    return p.UTF8String ?: "";
}

// The facade over `pack`, its compiles answered by `compiler` from a directory of `name`'s; the test's library on it
inline id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, const std::string &pack,
                             id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory) || !(device = open(pack)))
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

// The pack's own state `name` (turn_copy.h, zeta_colour.h), of key fields `fields`, on the open device
inline bool ownState(const char *name, const char *fields)
{
    gm20b::RuntimeRecord r;
    r.keyString = fields;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    return gRM->raw->addRecord(r, false, why) != nullptr;
}

inline bool depthConversionIn()
{
    return ownState(gm20b::turnDepthState(gm20b::kZetaFormat), "samples=1 ds=260/260 ");
}

inline bool depthConversionBack()
{
    return ownState(gm20b::turnDepthState(MTLPixelFormatDepth16Unorm), "samples=1 ds=250/0 ");
}

// A Depth32Float_Stencil8 moved by a turn copy: both words in one draw of the zeta's colour route
inline bool zetaColourRoute()
{
    return ownState(gm20b::stencilColourState(gm20b::kZetaFormat, 1, gm20b::StencilSample::Zero), "samples=1 c0=103 ");
}

inline bool depth32Turned()
{
    return ownState(gm20b::turnDepthState(MTLPixelFormatDepth32Float), "samples=1 ds=252/0 ");
}

// The record of a state of colour 0's storage form whose fragment program runs the depth test on its stand-in
// (shader_tests.h): entry 0 the colour, entry 1 the depth stand-in, the test's words at push word 2
inline void testsDepthInItsShader(const gm20b::CompileRequest &, gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    gm20b::setEntryCount(pp, 2);
    pp.entry[0] = {0, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0};
    pp.entry[1] = {16, gm20b::EntryDepthTest, 2, 0, gm20b::SamplerNone, 0, 0};
    pp.setBytes = 32;
    pp.memoryEntries = 3;
    pp.flags |= gm20b::PipelineFetches | gm20b::PipelineStorageMetadata | gm20b::PipelinePushMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTests = uint8_t(gm20b::FragmentTests::DepthWrite);
    pp.storageTic[0] = kR32UintWord0;
    pp.shaderTests.flags = gm20b::kShaderTestDepth;
    pp.shaderTests.depthWord = 2;
    pp.shaderTests.depthTic = kR32FloatWord0;
    pp.pushMask = 0xfu << 1;
    record.blob = {0x1224, 1, 0x1a00, 0};
}

// A BGRA8 state of a Depth16Unorm attachment made by `compiler` as testsDepthInItsShader shapes it
inline id<MTLRenderPipelineState> shaderTestingState(id<MTLDevice> device, id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth16Unorm;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:nil];
    __unsafe_unretained SwitchMetalPipeline *made = (SwitchMetalPipeline *)state;
    return made && made->_raw && made->_raw->rec && (made->_raw->rec->shaderTests.flags & gm20b::kShaderTestDepth)
               ? state
               : nil;
}

inline id<MTLDepthStencilState> depthWrite(id<MTLDevice> device)
{
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    return [device newDepthStencilStateWithDescriptor:ds];
}

// A 64x32 frame of rotation 1 over the 32x64 BGRA plane of a surface of its own
inline id<MTLTexture> turnedPlane(id<MTLDevice> device)
{
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    [d setValue:@YES forKey:@"framebufferOnly"];
    [d setValue:@1 forKey:@"rotation"];
    id<MTLTexture> t = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
    if (surface)
        CFRelease(surface);
    return t && ((SwitchMetalTexture *)t)->_raw.rotation ? t : nil;
}

inline id<MTLTexture> attachmentTexture(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width,
                                        NSUInteger height, NSUInteger layers = 1,
                                        MTLStorageMode storage = MTLStorageModePrivate)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    if (layers > 1) {
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = layers;
    }
    d.storageMode = storage;
    d.usage = storage == MTLStorageModeMemoryless ? MTLTextureUsageRenderTarget
                                                  : MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:d];
}

inline MTLRenderPassDescriptor *pass(id<MTLTexture> colour, id<MTLTexture> depth, MTLLoadAction depthLoad,
                                     NSUInteger depthSlice = 0)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = colour;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.depthAttachment.texture = depth;
    p.depthAttachment.slice = depthSlice;
    p.depthAttachment.loadAction = depthLoad;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.depthAttachment.clearDepth = 0.5;
    return p;
}

// One encoder of `p`, `body` recording into it, committed and waited for
inline id<MTLCommandBuffer> run(id<MTLDevice> device, MTLRenderPassDescriptor *p,
                                void (^body)(id<MTLRenderCommandEncoder>) = nil)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    if (body)
        body(e);
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

inline void expectRaw(id<MTLDevice>, id<MTLCommandBuffer> cb)
{
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(path(cb), "gm20b");
}

// `cb` failed `code` with the driver's own line `line`, its predicate `pred` named in its path, nothing of it on m2v
inline void expectFailed(id<MTLDevice>, id<MTLCommandBuffer> cb, const char *pred, MTLCommandBufferError code,
                         const char *line)
{
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, code) << text(cb.error);
    EXPECT_EQ(std::string(cb.error.localizedDescription.UTF8String ?: ""), line);
    EXPECT_EQ(path(cb), std::string("error:") + pred);
}

inline uint32_t headersLeft(id<MTLDevice> device)
{
    return [memory(device)[@"headers_left"] unsignedIntValue];
}

// Views of `of` holding every texture header the device has left (Device::createView takes one a view), for the caller
// to keep while its work needs a header it cannot have
inline NSMutableArray *takeEveryHeader(id<MTLDevice> device, id<MTLTexture> of)
{
    NSMutableArray *views = [NSMutableArray array];
    for (uint32_t left = headersLeft(device); left; left--) {
        id<MTLTexture> view = [of newTextureViewWithPixelFormat:of.pixelFormat];
        if (!view)
            break;
        [views addObject:view];
    }
    return views;
}

// The variant of `state` of `forms`, null where none was asked
inline gm20b::Variant<gm20b::Pipeline> *variantOf(id<MTLRenderPipelineState> state, const gm20b::RenderVariant &forms)
{
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    const auto found = pipeline->_variants.find(forms.name());
    return found == pipeline->_variants.end() ? nullptr : found->second.get();
}

}  // namespace pass_scene
