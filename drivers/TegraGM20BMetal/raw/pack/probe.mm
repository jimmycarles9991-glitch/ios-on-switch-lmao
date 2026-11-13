// SPDX-License-Identifier: MIT
// raw GM20B path, pack generator (r7-raw's probe; r8-rawca: corpus lines with lib= fields, Core Animation's states only): every corpus render pipeline state (r6-precompile corpus.log R lines) made through the shipped frontend + m2v + NVK
// (host builds, mock GM20B with cap.cpp's nvdec in NVDEC_PROBE mode) and drawn once with tagged inputs, so tools/mkpack.py can read
// off, per state, the programs NVK built, the 3D registers the state sets, and where each Metal slot lands in GPU memory.
//
// Tags: every buffer slot of both stages is setBytes of 256 bytes whose words are 0xC0DE0000|stage<<8|slot, the probe index,
// then 0xA0000000|stage<<20|slot<<16|word (slots 0..PROBE_SLOTS-1, default 31: Metal's whole buffer table, as Core Animation's
// functions read slots past 7, apply_apl_dimming's dim_factor at 9; an unbound one m2v fills with zeros); fragment texture slot t (0..15) is a (16+t)x(8+t)
// BGRA8 texture, fragment sampler slot s (0..15) has max LOD clamp 1+s/2 (mip filter nearest; r8-rawca: 1+s made slot 14 look like any sampler with the default clamp, 15), linear min/mag when s&1, repeat U
// when s&2. A sampler made before them takes NVK's sampler index 0. One command buffer (one submission) per state; stdout lines "PROBE <i> <state key>".
// The corpus's lib= fields (the journal names each function's library since 11-04) are not part of the state key: the raw path
// keys QuartzCore's functions by name and constants (RawMetalLibrary.mm raw_metal_pipeline_key). PROBE_LABEL (default
// com.apple.coreanimation.): only R lines whose label starts with it (the render server's states; "" for every line), and the
// lines that keep a lib= field (rawpack.py keeps it on another framework's states): their functions come from that library
// under PROBE_LIB_ROOT (the crawl's root), and their key ends with " | l <framework>", as the raw path keys them.
// usage: probe <nvk dylib> <cache dir> <default.metallib> <corpus.log> [first] [count]
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <map>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "probe_bind.h"
#include "probe_states.h"
using namespace probe;
extern "C" void mock_iokit_report(void);
extern "C" void pushcount_report(const char *what);
static uint32_t gSlots = getenv("PROBE_SLOTS") ? atoi(getenv("PROBE_SLOTS")) : 31;

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

// The color attachments the lines' states write (writeMask not 0), by index and pixel format, from their
// "c<i>=<format>:<mask>:" fields
static std::set<std::pair<unsigned, unsigned>> writtenTargets(const std::vector<std::string> &lines)
{
    std::set<std::pair<unsigned, unsigned>> out;
    for (const std::string &line : lines) {
        const std::string head = line.substr(0, line.find(" | v "));
        for (size_t at = head.find(" c"); at != std::string::npos; at = head.find(" c", at + 2)) {
            unsigned i, format, mask;
            if (sscanf(head.c_str() + at, " c%u=%u:%u:", &i, &format, &mask) == 3 && mask)
                out.insert({i, format});
        }
    }
    return out;
}

constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);

// RGB10A8_2P_XR10, BGRA10_XR, BGR10_XR and their _sRGB twins (550 to 555). m2v holds such an attachment as its raw codes and
// the function's draw writes them; a loaded attachment adds m2v's own passes (its unused bits made Metal's), so the state's
// draw would not be the pass's only one. The raw path loads and stores such an attachment in place.
static bool extendedRange(MTLPixelFormat f)
{
    return f >= MTLPixelFormat(550) && f <= MTLPixelFormat(555);
}

// RGB8_444_2P over plane 0 of a two-plane 4:4:4 surface (RenderBox's widget snapshots): the only texture of the format Metal makes
static id<MTLTexture> planarTarget(id<MTLDevice> dev, unsigned w, unsigned h)
{
    CVPixelBufferRef buffer = nullptr;
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    if (CVPixelBufferCreate(nullptr, w, h, '444f', (__bridge CFDictionaryRef)attributes, &buffer))
        return nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kRGB8_444_2P
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(buffer) plane:0];
}

static IOSurfaceRef newSurface(unsigned w, unsigned h)
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(w * 4),
        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 5) {
            fprintf(stderr, "usage: probe <nvk dylib> <cache dir> <default.metallib> <corpus.log> [first] [count]\n");
            return 2;
        }
        const unsigned first = argc > 5 ? atoi(argv[5]) : 0, count = argc > 6 ? atoi(argv[6]) : 100000;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) {
            fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        // PROBE_OWN_LIB=<metallib>: the raw path's own states after Core Animation's, drawn and read alike, lines marked "@O ":
        // the conversions of YCbCr textures (pack/ycbcr.metal) and the extended-range stand-ins (pack/xr.metal), that library's,
        // and the moves between an RGB8_444_2P target's planes and its RGBA16Float stand-in, the frontend's own library's
        // (TwoPlaneStandIn.metal: the bytes m2v draws them from); mkpack.py keys them by name ("raw.<ycbcr|xr|two_plane>.<fragment
        // function>"), which the runtime asks for
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB")) {
            own = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [dev newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        const std::vector<std::string> lines = corpusLines(text, own != nil);
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        // tagged textures and samplers: BGRA8, for the stand-ins' conversions the integer formats they read codes as
        // (pack/xr.metal), for the two-plane store the RGBA16Float stand-in it reads (the texture's class decides the push
        // words of its read), and for the raw path's own resolves 4x multisampled ones (pack/resolve.metal: m2v binds its null
        // texture for a texture2d_ms argument given another), tagged alike by their sizes
        auto tagged = [&](MTLPixelFormat f, NSUInteger samples = 1) {
            std::vector<id<MTLTexture>> out;
            for (unsigned t = 0; t < 16; t++) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                              width:16 + t
                                                                                             height:8 + t
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                if (samples > 1) {
                    td.textureType = MTLTextureType2DMultisample;
                    td.sampleCount = samples;
                    td.usage |= MTLTextureUsageRenderTarget;
                }
                td.storageMode = MTLStorageModePrivate;
                out.push_back([dev newTextureWithDescriptor:td]);
            }
            return out;
        };
        const std::vector<id<MTLTexture>> textures = tagged(MTLPixelFormatBGRA8Unorm);
        const std::map<std::string, std::vector<id<MTLTexture>>> codeTextures = {
            {"f xr_bgr10_frag", tagged(MTLPixelFormatR32Uint)},
            {"f xr_bgra10_frag", tagged(MTLPixelFormatRGBA16Uint)},
            {"f xr_planes_frag", tagged(MTLPixelFormatRG32Uint)},
            {"f xr_two_planes_frag", tagged(MTLPixelFormatR32Uint)},
            {"f two_plane_store", tagged(MTLPixelFormatRGBA16Float)},
            {"f ycbcr_store_", tagged(MTLPixelFormatRGBA32Float)},
            {"f ycbcr_packed_", tagged(MTLPixelFormatR32Uint)},
            {"f turn_copy", tagged(MTLPixelFormatR32Uint)},
            {"f turn_masked", tagged(MTLPixelFormatR32Uint)},
            {"f turn_reach", tagged(MTLPixelFormatR32Uint)},
            {"f turn_depth", tagged(MTLPixelFormatDepth32Float)},
            {"f stencil_bits_", tagged(MTLPixelFormatR32Uint)}};
        const std::vector<id<MTLTexture>> multisampled = tagged(MTLPixelFormatBGRA8Unorm, 4),
                                          multisampledDepth = tagged(MTLPixelFormatDepth32Float, 4),
                                          multisampledCodes = tagged(MTLPixelFormatR32Uint, 4);
        auto texturesOf = [&](const std::string &line, bool ours) -> const std::vector<id<MTLTexture>> & {
            if (ours && line.find("| v resolve_vert | f resolve_depth") != std::string::npos)
                return multisampledDepth;
            if (ours && line.find("| f stencil_bits_ms_") != std::string::npos)
                return multisampledCodes;
            if (ours && line.find("| v resolve_vert | f resolve_") != std::string::npos)
                return multisampled;
            for (const auto &[fragment, set] : codeTextures)
                if (line.find("| " + fragment) != std::string::npos)
                    return set;
            return textures;
        };
        std::vector<id<MTLSamplerState>> samplers;
        // a sampler made first takes NVK's sampler index 0, so a handle naming index 0 means "no sampler" below
        static id<MTLSamplerState> dummySampler;  // kept: a released sampler gives its index back
        dummySampler = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        for (unsigned s = 0; s < 16; s++) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = s & 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.mipFilter = MTLSamplerMipFilterNearest;  // NotMipmapped clamps the LOD to 0.25 and would erase the tag
            sd.sAddressMode = s & 2 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
            sd.lodMaxClamp =
                1 +
                0.5f *
                    s;  // 1, 1.5 ... 8.5: never 15, the clamp of Metal's default (FLT_MAX), which a constexpr sampler has
            samplers.push_back([dev newSamplerStateWithDescriptor:sd]);
        }
        // render targets per format: the first BGRA8 attachment is an IOSurface (CA's framebuffer), the others private, a
        // writeMask-0 attachment memoryless (CA's second attachment)
        IOSurfaceRef fbSurface = newSurface(64, 64);
        id<MTLTexture> fb = nil;
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            fb = [dev newTextureWithDescriptor:td iosurface:fbSurface plane:0];
        }
        NSMutableDictionary<NSString *, id<MTLTexture>> *targets = [NSMutableDictionary dictionary];
        // (samples > 1: a memoryless multisampled target, as a multisampled state renders only into one of its count). `own`: a
        // color attachment's own texture, so that a fetch of one of two attachments of a format names which (layouts.py
        // reads the attachment a fetched texture is)
        auto target = [&](MTLPixelFormat f, bool memoryless, NSUInteger samples = 1,
                          unsigned own = 0) -> id<MTLTexture> {
            NSString *k =
                [NSString stringWithFormat:@"%lu.%d.%lu.%u", (unsigned long)f, memoryless, (unsigned long)samples, own];
            if (!targets[k] && f == kRGB8_444_2P) {
                targets[k] = planarTarget(dev, 64, 64);
                return targets[k];
            }
            if (!targets[k]) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                              width:64
                                                                                             height:64
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | (memoryless ? 0 : MTLTextureUsageShaderRead);
                td.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
                if (samples > 1) {
                    td.textureType = MTLTextureType2DMultisample;
                    td.sampleCount = samples;
                }
                id<MTLTexture> t = [dev newTextureWithDescriptor:td];
                if (!t && memoryless) {
                    td.storageMode = MTLStorageModePrivate;
                    t = [dev newTextureWithDescriptor:td];
                }
                if (t)
                    targets[k] = t;
            }
            return targets[k];
        };
        // PROBE_SAMPLERS: one textured uber state drawn once per sampler descriptor Core Animation may make (CA::OGL::MetalContext::
        // draw: min/mag nearest or linear, mip not/nearest/linear, the address modes, max anisotropy 1, 2, 8 or 16, LOD clamps), every
        // fragment sampler slot bound to it; mksamplers.py reads NVK's sampler header for each, raw/test/samplercheck compares.
        if (getenv("PROBE_SAMPLERS")) {
            State st;
            bool found = false;
            for (const std::string &line : lines)
                if (line.find("f fixed_frag_lph_cph") != std::string::npos &&
                    line.find("fc_texture_function:49:01") != std::string::npos && parseState(qc, line, st)) {
                    found = true;
                    break;
                }
            if (!found) {
                fprintf(stderr, "PROBE_SAMPLERS: no textured uber state\n");
                return 1;
            }
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:st.d error:&error];
            unsigned i = 0;
            for (unsigned minmag = 0; minmag < 4; minmag++)
                for (unsigned mip = 0; mip < 3; mip++)
                    for (unsigned addr : {0u, 1u, 2u, 3u, 4u})
                        for (unsigned aniso : {1u, 2u, 8u, 16u})
                            for (unsigned lod = 0; lod < 2; lod++, i++) {
                                MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                                sd.minFilter = (MTLSamplerMinMagFilter)(minmag & 1);
                                sd.magFilter = (MTLSamplerMinMagFilter)(minmag >> 1);
                                sd.mipFilter = (MTLSamplerMipFilter)mip;
                                sd.sAddressMode = (MTLSamplerAddressMode)addr;
                                sd.tAddressMode = (MTLSamplerAddressMode)(addr == 4 ? 4 : (addr + 1) % 4);
                                sd.rAddressMode = (MTLSamplerAddressMode)addr;
                                sd.maxAnisotropy = aniso;
                                if (lod) {
                                    sd.lodMinClamp = 0.5f;
                                    sd.lodMaxClamp = 3.25f;
                                }
                                id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
                                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                                for (NSUInteger c = 0; c < 8; c++) {
                                    MTLRenderPipelineColorAttachmentDescriptor *ca = st.d.colorAttachments[c];
                                    if (ca.pixelFormat == MTLPixelFormatInvalid)
                                        continue;
                                    pass.colorAttachments[c].texture = target(ca.pixelFormat, ca.writeMask == 0);
                                    pass.colorAttachments[c].loadAction = MTLLoadActionDontCare;
                                    pass.colorAttachments[c].storeAction = MTLStoreActionDontCare;
                                }
                                id<MTLCommandBuffer> cb = [queue commandBuffer];
                                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
                                [enc setRenderPipelineState:state];
                                uint32_t zero[64] = {};
                                for (uint32_t slot = 0; slot < 8; slot++) {
                                    [enc setVertexBytes:zero length:sizeof zero atIndex:slot];
                                    [enc setFragmentBytes:zero length:sizeof zero atIndex:slot];
                                }
                                for (unsigned k = 0; k < 16; k++) {
                                    [enc setFragmentTexture:textures[0] atIndex:k];
                                    [enc setFragmentSamplerState:smp atIndex:k];
                                }
                                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                                [enc endEncoding];
                                printf("SAMPLER %u %u %u %u %u %u %u %u %u %g %g\n", i, unsigned(sd.minFilter),
                                       unsigned(sd.magFilter), unsigned(sd.mipFilter), unsigned(sd.sAddressMode),
                                       unsigned(sd.tAddressMode), unsigned(sd.rAddressMode), unsigned(sd.maxAnisotropy),
                                       unsigned(lod), sd.lodMinClamp, sd.lodMaxClamp);
                                fflush(stdout);
                                [cb commit];
                                [cb waitUntilCompleted];
                            }
            pushcount_report("probe");
            return 0;
        }
        // PROBE_FORMATS: instead of the states, one textured uber state drawn once per texture format the raw path may sample,
        // its fragment texture slots all bound to one linear texture of that format over host memory (the raw path's textures are
        // pitch-linear) with a size naming it: (40 + i) x (24 + i). mkformats.py reads NVK's texture header for each.
        if (getenv("PROBE_FORMATS")) {
            static const MTLPixelFormat formats[] = {
                MTLPixelFormatA8Unorm,         MTLPixelFormatR8Unorm,         MTLPixelFormatR8Unorm_sRGB,
                MTLPixelFormatRG8Unorm,        MTLPixelFormatR16Unorm,        MTLPixelFormatR16Float,
                MTLPixelFormatRG16Unorm,       MTLPixelFormatRG16Float,       MTLPixelFormatR32Float,
                MTLPixelFormatRGBA8Unorm,      MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm,
                MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGB10A2Unorm,    MTLPixelFormatBGR10A2Unorm,
                MTLPixelFormatRG11B10Float,    MTLPixelFormatRGB9E5Float,     MTLPixelFormatRGBA16Unorm,
                MTLPixelFormatRGBA16Float,     MTLPixelFormatRG32Float,       MTLPixelFormatRGBA32Float,
                MTLPixelFormatB5G6R5Unorm,     MTLPixelFormatA1BGR5Unorm,     MTLPixelFormatABGR4Unorm,
                MTLPixelFormatBGR5A1Unorm};
            State st;
            bool found = false;
            // PROBE_FORMATS_STATE=<text>: the first state whose line holds it instead (what m2v writes per bound format)
            const char *pick = getenv("PROBE_FORMATS_STATE");
            for (const std::string &line : lines)
                if ((pick ? line.find(pick) != std::string::npos
                          : line.find("f fixed_frag_lph_cph") != std::string::npos &&
                                line.find("fc_texture_function:49:01") != std::string::npos) &&
                    parseState(qc, line, st)) {
                    found = true;
                    break;
                }
            if (!found) {
                fprintf(stderr, "PROBE_FORMATS: no textured uber state\n");
                return 1;
            }
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:st.d error:&error];
            const size_t page = (size_t)getpagesize();
            for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
                const unsigned w = 40 + i, h = 24 + i;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[i]
                                                                                              width:w
                                                                                             height:h
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                const NSUInteger bpr =
                    ([dev minimumLinearTextureAlignmentForPixelFormat:formats[i]] > 256 ? 512 : 256) *
                    ((w * 16 + 255) / 256);
                const size_t bytes = (bpr * h + page - 1) / page * page;
                void *mem = nullptr;
                posix_memalign(&mem, page, bytes);
                id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:mem
                                                           length:bytes
                                                          options:MTLResourceStorageModeShared
                                                      deallocator:nil];
                id<MTLTexture> t = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:bpr];
                if (!t) {
                    printf("FORMAT %u %lu refused\n", i, (unsigned long)formats[i]);
                    continue;
                }
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                for (NSUInteger c = 0; c < 8; c++) {
                    MTLRenderPipelineColorAttachmentDescriptor *ca = st.d.colorAttachments[c];
                    if (ca.pixelFormat == MTLPixelFormatInvalid)
                        continue;
                    pass.colorAttachments[c].texture = target(ca.pixelFormat, ca.writeMask == 0);
                    pass.colorAttachments[c].loadAction = MTLLoadActionDontCare;
                    pass.colorAttachments[c].storeAction = MTLStoreActionDontCare;
                }
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
                [enc setRenderPipelineState:state];
                uint32_t zero[64] = {};
                for (uint32_t slot = 0; slot < 8; slot++) {
                    [enc setVertexBytes:zero length:sizeof zero atIndex:slot];
                    [enc setFragmentBytes:zero length:sizeof zero atIndex:slot];
                }
                for (unsigned k = 0; k < 16; k++) {
                    [enc setFragmentTexture:t atIndex:k];
                    [enc setFragmentSamplerState:samplers[k] atIndex:k];
                }
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [enc endEncoding];
                printf("FORMAT %u %lu %ux%u %lu\n", i, (unsigned long)formats[i], w, h, (unsigned long)bpr);
                fflush(stdout);
                [cb commit];
                [cb waitUntilCompleted];
            }
            pushcount_report("probe");
            return 0;
        }
        // PROBE_TARGETS: the textured uber state drawn once into a private 64x64 target of each format the raw path may render to,
        // as its first color attachment; NVK's color target format for each is the draw's rt0 (raw/test/targetcheck.py).
        if (getenv("PROBE_TARGETS")) {
            static const MTLPixelFormat formats[] = {MTLPixelFormatA8Unorm,         MTLPixelFormatR8Unorm,
                                                     MTLPixelFormatR8Unorm_sRGB,    MTLPixelFormatRG8Unorm,
                                                     MTLPixelFormatR16Unorm,        MTLPixelFormatR16Float,
                                                     MTLPixelFormatRG16Unorm,       MTLPixelFormatRG16Float,
                                                     MTLPixelFormatR32Float,        MTLPixelFormatRGBA8Unorm,
                                                     MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm,
                                                     MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGB10A2Unorm,
                                                     MTLPixelFormatBGR10A2Unorm,    MTLPixelFormatRG11B10Float,
                                                     MTLPixelFormatRGB9E5Float,     MTLPixelFormatRGBA16Unorm,
                                                     MTLPixelFormatRGBA16Float,     MTLPixelFormatRG32Float,
                                                     MTLPixelFormatRGBA32Float,     MTLPixelFormatB5G6R5Unorm,
                                                     MTLPixelFormatA1BGR5Unorm,     MTLPixelFormatABGR4Unorm,
                                                     MTLPixelFormatBGR5A1Unorm,     MTLPixelFormat(550),
                                                     MTLPixelFormat(552),           MTLPixelFormat(554)};
            State st;
            bool found = false;
            for (const std::string &line : lines)
                if (line.find("f fixed_frag_lph_cph") != std::string::npos &&
                    line.find("fc_texture_function:49:01") != std::string::npos && parseState(qc, line, st)) {
                    found = true;
                    break;
                }
            if (!found) {
                fprintf(stderr, "PROBE_TARGETS: no textured uber state\n");
                return 1;
            }
            for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
                MTLRenderPipelineDescriptor *d = [st.d copy];
                d.colorAttachments[0].pixelFormat = formats[i];
                // with blending on, m2v's draw has no color target for an extended-range attachment (rt0 null)
                if (extendedRange(formats[i]))
                    d.colorAttachments[0].blendingEnabled = NO;
                id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:d error:&error];
                id<MTLTexture> t = target(formats[i], false);
                if (!state || !t) {
                    printf("TARGET %u %lu refused\n", i, (unsigned long)formats[i]);
                    continue;
                }
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                for (NSUInteger c = 0; c < 8; c++) {
                    MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[c];
                    if (ca.pixelFormat == MTLPixelFormatInvalid)
                        continue;
                    pass.colorAttachments[c].texture = c ? target(ca.pixelFormat, ca.writeMask == 0) : t;
                    pass.colorAttachments[c].loadAction = MTLLoadActionDontCare;
                    pass.colorAttachments[c].storeAction =
                        c || extendedRange(ca.pixelFormat) ? MTLStoreActionDontCare : MTLStoreActionStore;
                }
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
                [enc setRenderPipelineState:state];
                uint32_t zero[64] = {};
                for (uint32_t slot = 0; slot < 8; slot++) {
                    [enc setVertexBytes:zero length:sizeof zero atIndex:slot];
                    [enc setFragmentBytes:zero length:sizeof zero atIndex:slot];
                }
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [enc endEncoding];
                printf("TARGET %u %lu\n", i, (unsigned long)formats[i]);
                fflush(stdout);
                [cb commit];
                [cb waitUntilCompleted];
            }
            pushcount_report("probe");
            return 0;
        }
        std::set<std::string> seen;
        unsigned index = 0, made = 0, refused = 0, failed = 0;
        // warm-up: one draw of a state that samples, with the dummy sampler in every slot, so NVK (m2v makes samplers at first
        // use) gives it index 0 and a handle naming index 0 means "no sampler". Its command buffer first clears every private
        // target the states load: m2v writes Metal's fresh-texture pattern into a new Private texture at its first use
        // (texture.rs fresh_patterned; a float format's with a draw), which would otherwise land in the submission of the
        // first state that loads the target, as a draw that state does not make
        for (const std::string &line : lines) {
            State s;
            if (line.find("downsample_4_frag") == std::string::npos || !parseState(qc, line, s))
                continue;
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:s.d error:&error];
            if (!state)
                continue;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            for (NSUInteger c = 0; c < 8; c++) {
                MTLRenderPipelineColorAttachmentDescriptor *ca = s.d.colorAttachments[c];
                if (ca.pixelFormat == MTLPixelFormatInvalid)
                    continue;
                pass.colorAttachments[c].texture = target(ca.pixelFormat, ca.writeMask == 0);
                pass.colorAttachments[c].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[c].storeAction = MTLStoreActionDontCare;
            }
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            for (const auto &[index, format] : writtenTargets(lines)) {
                id<MTLTexture> t = target((MTLPixelFormat)format, false, 1, index + 1);
                if (!t)
                    continue;
                MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
                clear.colorAttachments[0].texture = t;
                clear.colorAttachments[0].loadAction = MTLLoadActionClear;
                clear.colorAttachments[0].storeAction = MTLStoreActionStore;
                [[cb renderCommandEncoderWithDescriptor:clear] endEncoding];
            }
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
            [enc setRenderPipelineState:state];
            uint32_t zero[64] = {};
            for (uint32_t slot = 0; slot < 31; slot++) {
                [enc setVertexBytes:zero length:sizeof zero atIndex:slot];
                [enc setFragmentBytes:zero length:sizeof zero atIndex:slot];
            }
            for (unsigned t = 0; t < 16; t++) {
                [enc setFragmentTexture:textures[0] atIndex:t];
                [enc setFragmentSamplerState:dummySampler atIndex:t];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            printf("WARMUP\n");
            break;
        }
        for (const std::string &marked : lines) {
            State s;
            const bool ours = marked.rfind("@O ", 0) == 0, framework = marked.rfind("@L ", 0) == 0;
            const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
            const std::string line = ours ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
            id<MTLLibrary> library = !ours ? qc : line.find("| f two_plane_") != std::string::npos ? standIn : own;
            if (framework && !(library = frameworkLibrary(dev, marked.substr(3, pathEnd - 3)))) {
                fprintf(stderr, "LIBRARY %s\n", marked.substr(3, pathEnd - 3).c_str());
                refused++;
                continue;
            }
            if (!parseState(library, line, s, framework ? frameworkName(marked.substr(3, pathEnd - 3)) : "")) {
                fprintf(stderr, "UNPARSED %s\n", line.substr(0, 80).c_str());
                refused++;
                continue;
            }
            if (!seen.insert(s.key).second)
                continue;
            const unsigned i = index++;
            if (i < first || i >= first + count)
                continue;
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:s.d error:&error];
            if (!state) {
                refused++;
                printf("REFUSED %u %s\n", i, error.localizedDescription.UTF8String);
                continue;
            }
            made++;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            // beside a depth/stencil attachment a linear color is m2v's stand-in, copied in by a draw and merged back by
            // dispatches (metal2vulkan linear_attachment.rs): such a state draws into private colors, which NVK renders in place
            const bool depthStencil = s.d.depthAttachmentPixelFormat != MTLPixelFormatInvalid ||
                                      s.d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid;
            const NSUInteger samples = std::max<NSUInteger>(s.d.rasterSampleCount, 1);
            bool usedFb = depthStencil || samples > 1;
            for (NSUInteger c = 0; c < 8; c++) {
                MTLRenderPipelineColorAttachmentDescriptor *ca = s.d.colorAttachments[c];
                if (ca.pixelFormat == MTLPixelFormatInvalid)
                    continue;
                const bool memoryless = ca.writeMask == 0 || samples > 1;
                id<MTLTexture> t = (!memoryless && !usedFb && ca.pixelFormat == MTLPixelFormatBGRA8Unorm)
                                       ? fb
                                       : target(ca.pixelFormat, memoryless, samples, memoryless ? 0 : unsigned(c) + 1);
                usedFb |= t == fb;
                pass.colorAttachments[c].texture = t;
                // a 542 snapshot renders into the frontend's RGBA16Float stand-in, which a load or store would draw from or
                // into: neither, so the state's draw is the pass's one
                const bool planes = ca.pixelFormat == kRGB8_444_2P;
                const bool loose = memoryless || planes || extendedRange(ca.pixelFormat);
                pass.colorAttachments[c].loadAction = loose ? MTLLoadActionDontCare : MTLLoadActionLoad;
                pass.colorAttachments[c].storeAction = loose ? MTLStoreActionDontCare : MTLStoreActionStore;
            }
            // a state with depth or stencil formats draws into a memoryless attachment of them (Metal's default Clear load and
            // DontCare store, its default depth/stencil state), so its registers are those NVK writes beside one
            if (depthStencil && s.d.depthAttachmentPixelFormat != MTLPixelFormatInvalid)
                pass.depthAttachment.texture = target(s.d.depthAttachmentPixelFormat, true, samples);
            if (s.d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid)
                pass.stencilAttachment.texture = target(s.d.stencilAttachmentPixelFormat, true, samples);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
            [enc setRenderPipelineState:state];
            // PROBE_BYTES (default 256): the tagged payloads' length; 16 and 4096 tell a uniform from a storage buffer (NVK rounds
            // a uniform buffer's range up to 256) and the function's proven window on each slot (m2v binds at most that)
            static const uint32_t bytes = getenv("PROBE_BYTES") ? atoi(getenv("PROBE_BYTES")) : 256;
            // PROBE_BIND_OFFSET: each slot's payload in a buffer of its own at an offset of its own below 256 (probe_bind.h),
            // which names each buffer slot's SlotBindOffset residual push word (mkpack.py)
            static const bool bindOffset = getenv("PROBE_BIND_OFFSET") != nullptr;
            static std::vector<id<MTLBuffer>> payloads;
            if (bindOffset && payloads.empty())
                for (uint32_t k = 0; k < 2 * gSlots; k++)
                    payloads.push_back([dev newBufferWithLength:256 + bytes options:MTLResourceStorageModeShared]);
            std::vector<uint32_t> tag(bytes / 4);
            for (uint32_t stage = 1; stage <= 2; stage++)
                for (uint32_t slot = 0; slot < gSlots; slot++) {
                    tag[0] = 0xC0DE0000u | stage << 8 | slot;
                    tag[1] = i;
                    for (uint32_t k = 2; k < tag.size(); k++)
                        tag[k] = 0xA0000000u | stage << 20 | slot << 16 | k;
                    if (bindOffset) {
                        const NSUInteger at = raw::probeBindOffset(stage, slot);
                        id<MTLBuffer> b = payloads[(stage - 1) * gSlots + slot];
                        memcpy((uint8_t *)b.contents + at, tag.data(), bytes);
                        if (stage == 1)
                            [enc setVertexBuffer:b offset:at atIndex:slot];
                        else
                            [enc setFragmentBuffer:b offset:at atIndex:slot];
                    } else if (stage == 1) {
                        [enc setVertexBytes:tag.data() length:bytes atIndex:slot];
                    } else {
                        [enc setFragmentBytes:tag.data() length:bytes atIndex:slot];
                    }
                }
            for (unsigned t = 0; t < 16; t++)
                [enc setFragmentTexture:texturesOf(line, ours)[t] atIndex:t];
            for (unsigned t = 0; t < 16; t++)
                [enc setFragmentSamplerState:samplers[t] atIndex:t];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [enc endEncoding];
            printf("PROBE %u %s\n", i, s.key.c_str());
            fflush(stdout);
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) {
                failed++;
                printf("FAILED %u %s\n", i, cb.error.localizedDescription.UTF8String);
            }
        }
        fprintf(stderr, "probe: %u states made, %u refused, %u failed\n", made, refused, failed);
        pushcount_report("probe");
        mock_iokit_report();
    }
    return 0;
}
