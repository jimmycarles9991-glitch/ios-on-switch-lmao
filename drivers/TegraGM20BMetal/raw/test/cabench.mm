// r8-rawca: CABENCH_RAW=<pack>: the same frames through the raw facade (RawMetal.mm) over the m2v device (the render server's
// arrangement), else the m2v device alone. Copied from r8-nullmoth/bench (r4-encode's).
// r4-encode: a Core Animation shaped frame through the frontend + m2v + NVK (host build over mock_iokit), to measure and
// profile the CPU cost of encoding. Pipelines are rebuilt from a device pipelines.log (R lines: QuartzCore's own descriptors
// and function constants), so the variants, the framebuffer fetch and the second (writeMask 0) attachment are CA's.
// usage: cabench <libvulkan_nouveau.dylib> <cache dir> <QuartzCore default.metallib> <pipelines.log> [frames] [draws] [mode]
//   mode bits: 1 = blur chain passes too, 2 = new IOSurface texture objects every frame (CA's per-frame wrappers),
//              4 = one replaceRegion per frame (a glyph/image upload), 8 = only the fetching uber pipelines,
//              16 = uniforms and vertices through one shared buffer at offsets (else setBytes), 32 = only the two setBytes of
//              the first benchmark (the uber shaders' other buffer slots unbound: m2v binds zero buffers there)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <IOSurface/IOSurface.h>
#include <algorithm>
#include <mach/mach.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <dlfcn.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/RawMetal.h"
extern "C" void mock_iokit_report(void);
extern "C" void pushcount_report(const char *what);
static void (*const r8_set_stats)(void) = nullptr;  // r8-nullmoth's instrumented bridge only
// A malloc counter preloaded into the run (DYLD_INSERT_LIBRARIES): the steady frames' allocations, every thread's
static const auto mc_start = reinterpret_cast<void (*)(void)>(dlsym(RTLD_DEFAULT, "mc_start"));
static const auto mc_stop = reinterpret_cast<void (*)(unsigned)>(dlsym(RTLD_DEFAULT, "mc_stop"));

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static uint64_t threadNs()
{
    thread_basic_info_data_t t{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&t, &n);
    mach_port_deallocate(mach_task_self(), self);
    return ((uint64_t)t.user_time.seconds * 1000000 + t.user_time.microseconds +
            (uint64_t)t.system_time.seconds * 1000000 + t.system_time.microseconds) *
           1000;
}

static uint64_t wallNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// "name:type:hex" or "#index:type:hex" items into constant values.
static MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2)
            bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#')
            [v setConstantValue:bytes
                           type:(MTLDataType)atoi(type.c_str())
                        atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}

struct Recipe {
    std::string label, vname, fname;
    MTLRenderPipelineDescriptor *d;
    bool fetch;
    bool image;
};

static std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(' ', at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

static bool recipe(id<MTLLibrary> qc, const std::string &line, Recipe &r)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | qc");
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return false;
    std::vector<std::string> head = split(line.substr(0, bar1));
    std::vector<std::string> v = split(line.substr(bar1 + 5, bar2 - bar1 - 5));
    std::vector<std::string> f =
        split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    if (v.empty() || f.empty())
        return false;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) {
            r.label = h.substr(6);
            d.label = @(r.label.c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthAttachmentPixelFormat = (MTLPixelFormat)a;
            d.stencilAttachmentPixelFormat = (MTLPixelFormat)b;
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0];
            c.writeMask = x[1];
            c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3];
            c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5];
            c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7];
            c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
        }
    }
    d.vertexDescriptor = vd;
    NSError *e = nil;
    std::vector<std::string> vc(v.begin() + 1, v.end()), fc(f.begin() + 1, f.end());
    d.vertexFunction = vc.empty() ? [qc newFunctionWithName:@(v[0].c_str())]
                                  : [qc newFunctionWithName:@(v[0].c_str()) constantValues:constants(vc) error:&e];
    d.fragmentFunction = fc.empty() ? [qc newFunctionWithName:@(f[0].c_str())]
                                    : [qc newFunctionWithName:@(f[0].c_str()) constantValues:constants(fc) error:&e];
    if (!d.vertexFunction || !d.fragmentFunction) {
        fprintf(stderr, "function %s/%s: %s\n", v[0].c_str(), f[0].c_str(), e.localizedDescription.UTF8String);
        return false;
    }
    r.vname = v[0];
    r.fname = f[0];
    r.d = d;
    r.fetch = line.find("fc_framebuffer_fetch:53:01") != std::string::npos;
    r.image = line.find("fc_image_count:49:01") != std::string::npos;
    return true;
}

static id<MTLTexture> surfaceTexture(id<MTLDevice> dev, IOSurfaceRef s, MTLTextureUsage usage)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:IOSurfaceGetWidth(s)
                                                                                 height:IOSurfaceGetHeight(s)
                                                                              mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td iosurface:s plane:0];
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
            fprintf(stderr, "usage\n");
            return 2;
        }
        const unsigned frames = argc > 5 ? atoi(argv[5]) : 200, draws = argc > 6 ? atoi(argv[6]) : 60,
                       mode = argc > 7 ? strtoul(argv[7], 0, 0) : 1;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        if (const char *pack = getenv("CABENCH_RAW")) {
            NSString *why = nil;
            dev = raw_metal_device_open(pack, 0, dev, &why);
            if (!dev) {
                fprintf(stderr, "NO RAW DEVICE: %s\n", why.UTF8String);
                return 1;
            }
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) {
            fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        // CA's pipelines from the journal: the main pass's BGRA8 pair-attachment states and the blur chain's.
        std::vector<Recipe> main, blur;
        {
            NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
            unsigned seen = 0;
            for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
                if (![l hasPrefix:@"R "] || [l containsString:@" ok=0 "])
                    continue;
                std::string line = l.UTF8String;
                for (size_t at; (at = line.find(" lib=")) != std::string::npos;)
                    line.erase(at, line.find(' ', at + 1) - at);
                Recipe r;
                bool uber = line.find("v fixed_vert_lph_spc") != std::string::npos,
                     ds = line.find("downsample_blur_vert") != std::string::npos;
                if (!uber && !ds)
                    continue;
                if (line.find(" c0=80:") == std::string::npos || line.find(" c1=80:") == std::string::npos)
                    continue;
                if (uber && getenv("CABENCH_MATCH") && line.find(getenv("CABENCH_MATCH")) == std::string::npos)
                    continue;
                if (uber && main.size() >= 12)
                    continue;
                if (ds && blur.size() >= 4)
                    continue;
                if (!recipe(qc, line, r))
                    continue;
                if (uber && (mode & 8) && !r.fetch)
                    continue;
                ++seen;
                (uber ? main : blur).push_back(r);
            }
            fprintf(stderr, "recipes: main %zu blur %zu\n", main.size(), blur.size());
        }
        std::vector<id<MTLRenderPipelineState>> mainStates, blurStates;
        uint64_t t0 = wallNs();
        for (Recipe &r : main) {
            if (getenv("R4_PROBE"))
                fprintf(stderr, "recipe %s fetch=%d\n", r.label.c_str(), r.fetch);
            id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:r.d error:&error];
            if (!s) {
                fprintf(stderr, "REFUSED %s: %s\n", r.label.c_str(), error.localizedDescription.UTF8String);
                continue;
            }
            mainStates.push_back(s);
        }
        for (Recipe &r : blur) {
            id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:r.d error:&error];
            if (!s) {
                fprintf(stderr, "REFUSED %s: %s\n", r.label.c_str(), error.localizedDescription.UTF8String);
                continue;
            }
            blurStates.push_back(s);
        }
        fprintf(stderr, "states: main %zu blur %zu in %.0f ms\n", mainStates.size(), blurStates.size(),
                (wallNs() - t0) / 1e6);
        if (mainStates.empty())
            return 1;
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        // The framebuffers: two 1280x720 BGRA IOSurfaces (double buffered), CA's second attachment memoryless.
        IOSurfaceRef fb[2] = {newSurface(1280, 720), newSurface(1280, 720)};
        id<MTLTexture> fbTex[2] = {surfaceTexture(dev, fb[0], MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
                                   surfaceTexture(dev, fb[1], MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)};
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:1280
                                                                                     height:720
                                                                                  mipmapped:NO];
        md.usage = MTLTextureUsageRenderTarget;
        md.storageMode = MTLStorageModeMemoryless;
        id<MTLTexture> second = [dev newTextureWithDescriptor:md];
        if (!second) {
            md.storageMode = MTLStorageModePrivate;
            second = [dev newTextureWithDescriptor:md];
            fprintf(stderr, "memoryless refused; private second attachment\n");
        }
        // Layer contents: a few IOSurfaces (icons, wallpaper) sampled by the image draws.
        std::vector<IOSurfaceRef> images;
        std::vector<id<MTLTexture>> imageTex;
        for (unsigned i = 0; i < 8; i++) {
            images.push_back(newSurface(i == 0 ? 1280 : 128, i == 0 ? 720 : 128));
            imageTex.push_back(surfaceTexture(dev, images.back(), MTLTextureUsageShaderRead));
        }
        MTLTextureDescriptor *gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:256
                                                                                     height:256
                                                                                  mipmapped:NO];
        gd.usage = MTLTextureUsageShaderRead;
        gd.storageMode = MTLStorageModeShared;
        id<MTLTexture> glyphs = [dev newTextureWithDescriptor:gd];
        std::vector<uint8_t> glyphBytes(64 * 64 * 4, 0x7f);
        MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:320
                                                                                     height:180
                                                                                  mipmapped:NO];
        bd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        bd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> blurTex[2] = {[dev newTextureWithDescriptor:bd], [dev newTextureWithDescriptor:bd]};
        bd.usage = MTLTextureUsageRenderTarget;
        bd.storageMode = second.storageMode;
        id<MTLTexture> blurSecond = [dev newTextureWithDescriptor:bd];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
        float vertices[4 * 12] = {};
        for (int i = 0; i < 4; i++) {
            vertices[i * 12 + 0] = (i & 1) ? 1 : -1;
            vertices[i * 12 + 1] = (i & 2) ? 1 : -1;
            vertices[i * 12 + 3] = 1;
            vertices[i * 12 + 4] = i & 1;
            vertices[i * 12 + 5] = (i >> 1) & 1;
        }
        float uniforms[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1};
        const NSUInteger ringBytes = 1 << 20;
        // CABENCH_RING_CA: the ring as Core Animation's buffer pool makes its buffers (options 0: shared, default cache mode;
        // QuartzCore MetalBufferPool::allocate_buffer), else write-combined
        id<MTLBuffer> ring =
            [dev newBufferWithLength:ringBytes
                             options:getenv("CABENCH_RING_CA")
                                         ? MTLResourceStorageModeShared
                                         : MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        NSUInteger ringAt = 0;
        std::vector<double> cpu, wall;
        uint64_t warm = frames > 20 ? 10 : 0;
        id<MTLCommandBuffer> last = nil;
        const char *pause = getenv("CABENCH_PAUSE");
        const unsigned blurPasses = getenv("CABENCH_PASSES") ? atoi(getenv("CABENCH_PASSES")) : 3;
        for (unsigned frame = 0; frame < frames; frame++) {
            if (frame == warm) {
                char t[2048];
                native_bridge_metal_rates(t, sizeof t);
                pushcount_report("warm-up");
                if (r8_set_stats)
                    r8_set_stats();
                if (mc_start)
                    mc_start();
            }
            if (pause && frame == warm) {
                fprintf(stderr, "pid %d warm; press enter\n", getpid());
                getchar();
            }
            @autoreleasepool {
                const uint64_t c0 = threadNs(), w0 = wallNs();
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                // as Core Animation does on every command buffer (r7l: the raw path once sent each of them to m2v for it)
                static const SEL protection = NSSelectorFromString(@"setProtectionOptions:");
                if ([(id)cb respondsToSelector:protection])
                    ((void (*)(id, SEL, uint64_t))objc_msgSend)(cb, protection, 0);
                if (mode & 4)
                    [glyphs replaceRegion:MTLRegionMake2D((frame * 64) % 256, 0, 64, 64)
                              mipmapLevel:0
                                withBytes:glyphBytes.data()
                              bytesPerRow:64 * 4];
                if ((mode & 1) && !blurStates.empty()) {
                    for (unsigned p = 0; p < blurPasses; p++) {
                        MTLRenderPassDescriptor *bp = [MTLRenderPassDescriptor renderPassDescriptor];
                        bp.colorAttachments[0].texture = blurTex[p & 1];
                        bp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                        bp.colorAttachments[0].storeAction = MTLStoreActionStore;
                        bp.colorAttachments[1].texture = blurSecond;
                        bp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
                        bp.colorAttachments[1].storeAction = MTLStoreActionDontCare;
                        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:bp];
                        [e setRenderPipelineState:blurStates[p % blurStates.size()]];
                        [e setVertexBytes:vertices length:sizeof vertices atIndex:1];
                        if (!(mode & 32))
                            for (unsigned v : {2u, 3u})
                                [e setVertexBytes:uniforms length:64 atIndex:v];
                        [e setFragmentBytes:uniforms length:64 atIndex:0];
                        if (!(mode & 32))
                            for (unsigned f : {1u, 6u})
                                [e setFragmentBytes:uniforms length:64 atIndex:f];
                        [e setFragmentTexture:p ? blurTex[(p + 1) & 1] : imageTex[0] atIndex:0];
                        [e setFragmentSamplerState:sampler atIndex:0];
                        if (mode & 256)
                            [e setScissorRect:(MTLScissorRect){0, 0, 160, 90}];
                        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                        [e endEncoding];
                    }
                }
                id<MTLTexture> target = fbTex[frame & 1];
                if (mode & 2)
                    target =
                        surfaceTexture(dev, fb[frame & 1], MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = getenv("CABENCH_LOAD") ? MTLLoadActionLoad : MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[1].texture = second;
                pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[1].storeAction = MTLStoreActionDontCare;
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
                for (unsigned i = 0; i < draws; i++) {
                    static const unsigned run = getenv("CABENCH_RUN") ? (unsigned)atoi(getenv("CABENCH_RUN"))
                                                                      : 3;  // r8-nullmoth: draws per pipeline
                    const unsigned k = (i / (run ? run : 3)) % mainStates.size();
                    [e setRenderPipelineState:mainStates[k]];
                    vertices[0] = (float)i / draws;
                    uniforms[16] = (float)i;
                    if (mode & 16) {
                        // CA's per-frame buffer: vertices and uniforms written at offsets, bound by offset.
                        uint8_t *at = (uint8_t *)ring.contents + ringAt;
                        memcpy(at, vertices, sizeof vertices);
                        memcpy(at + 256, uniforms, sizeof uniforms);
                        if (i == 0) {
                            for (unsigned v : {1u, 2u, 3u})
                                [e setVertexBuffer:ring offset:ringAt atIndex:v];
                            for (unsigned f : {0u, 1u, 2u, 5u, 6u})
                                [e setFragmentBuffer:ring offset:ringAt + 256 atIndex:f];
                        } else {
                            for (unsigned v : {1u, 2u, 3u})
                                [e setVertexBufferOffset:ringAt atIndex:v];
                            for (unsigned f : {0u, 1u, 2u, 5u, 6u})
                                [e setFragmentBufferOffset:ringAt + 256 atIndex:f];
                        }
                        ringAt = (ringAt + 512) % (ringBytes - 512);
                    } else {
                        [e setVertexBytes:vertices length:sizeof vertices atIndex:1];
                        if (!(mode & 32))
                            for (unsigned v : {2u, 3u})
                                [e setVertexBytes:uniforms length:64 atIndex:v];
                        [e setFragmentBytes:uniforms length:sizeof uniforms atIndex:0];
                        if (!(mode & 32))
                            for (unsigned f : {1u, 2u, 5u, 6u})
                                [e setFragmentBytes:uniforms length:64 atIndex:f];
                    }
                    if (main[k].image) {
                        [e setFragmentTexture:(mode & 4) && (i & 7) == 7 ? glyphs : imageTex[i % imageTex.size()]
                                      atIndex:0];
                        [e setFragmentSamplerState:sampler atIndex:0];
                    }
                    [e setScissorRect:(MTLScissorRect){(i * 13) % 1000, (i * 7) % 500, 280, 220}];
                    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                }
                [e endEncoding];
                [cb commit];
                const uint64_t c1 = threadNs(), w1 = wallNs();
                if (frame >= warm) {
                    cpu.push_back((c1 - c0) / 1e6);
                    wall.push_back((w1 - w0) / 1e6);
                }
                if (last)
                    [last waitUntilCompleted];
                if (last.error)
                    fprintf(stderr, "frame %u error: %s\n", frame, last.error.localizedDescription.UTF8String);
                last = cb;
            }
        }
        [last waitUntilCompleted];
        std::sort(cpu.begin(), cpu.end());
        std::sort(wall.begin(), wall.end());
        double sum = 0;
        for (double c : cpu)
            sum += c;
        printf("frames=%zu draws=%u mode=%u: encode thread cpu mean %.3f ms p50 %.3f p90 %.3f | wall p50 %.3f ms | per "
               "draw %.1f us\n",
               cpu.size(), draws, mode, sum / cpu.size(), cpu[cpu.size() / 2], cpu[cpu.size() * 9 / 10],
               wall[wall.size() / 2], 1000 * sum / cpu.size() / draws);
        if (mc_stop)
            mc_stop(unsigned(frames - warm));
        char text[2048] = {};
        native_bridge_metal_rates(text, sizeof text);
        printf("rates: %s\n", text);
        if (getenv("CABENCH_RAW") && raw_metal_rates(text, sizeof text))
            printf("raw rates: %s\n", text);
        native_bridge_metal_status(text, sizeof text);
        printf("status: %s\n", text);
        mock_iokit_report();
        pushcount_report("steady");
        if (r8_set_stats)
            r8_set_stats();
    }
    return 0;
}
