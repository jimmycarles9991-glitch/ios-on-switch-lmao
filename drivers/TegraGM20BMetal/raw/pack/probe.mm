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
// keys QuartzCore's functions by name and constants (RawMetal.mm raw_metal_pipeline_key). PROBE_LABEL (default
// com.apple.coreanimation.): only R lines whose label starts with it (the render server's states; "" for every line), and the
// lines that keep a lib= field (rawpack.py keeps it on another framework's states): their functions come from that library
// under PROBE_LIB_ROOT (the crawl's root), and their key ends with " | l <framework>", as the raw path keys them.
// usage: probe <nvk dylib> <cache dir> <default.metallib> <corpus.log> [first] [count]
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
extern "C" void mock_iokit_report(void);
extern "C" void pushcount_report(const char *what);
static uint32_t gSlots = getenv("PROBE_SLOTS") ? atoi(getenv("PROBE_SLOTS")) : 31;

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

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

static MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
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

static id<MTLFunction> function(id<MTLLibrary> qc, const std::vector<std::string> &stage)
{
    if (stage.empty() || stage[0] == "-")
        return nil;
    if (stage.size() == 1)
        return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    return [qc newFunctionWithName:@(stage[0].c_str())
                    constantValues:constants(std::vector<std::string>(stage.begin() + 1, stage.end()))
                             error:&e];
}

struct State {
    std::string key, label;
    MTLRenderPipelineDescriptor *d;
    unsigned vertexBuffer = 99, vertexStride = 0;
};

// precompile.mm's R-line parser (the frontend's journal format), key = every descriptor field but the label.
static bool parseState(id<MTLLibrary> qc, const std::string &line, State &s)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return false;
    std::vector<std::string> head = split(line.substr(0, bar1));
    std::vector<std::string> v = split(line.substr(bar1 + 5, bar2 - bar1 - 5));
    std::vector<std::string> f =
        split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    std::string key;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) {
            s.label = h.substr(6);
            if (s.label != "-")
                d.label = @(s.label.c_str());
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
                       x + 7, x + 8) != 10 ||
                i >= 8)
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
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 || i >= 31 ||
                x[2] >= 31)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
            s.vertexBuffer = x[2];
            s.vertexStride = x[3];
        } else
            continue;
        if (h.rfind("label=", 0) != 0)
            key += h + " ";
    }
    d.vertexDescriptor = vd;
    d.vertexFunction = function(qc, v);
    d.fragmentFunction = function(qc, f);
    if (!d.vertexFunction || (!d.fragmentFunction && !f.empty() && f[0] != "-"))
        return false;
    s.key = key + line.substr(bar1, (bar3 == std::string::npos ? line.size() : bar3) - bar1);
    s.d = d;
    return true;
}

// The pixel formats of the color attachments the lines' states write (writeMask not 0), from their "c<i>=<format>:<mask>:"
// fields
static std::set<unsigned> writtenFormats(const std::vector<std::string> &lines)
{
    std::set<unsigned> out;
    for (const std::string &line : lines) {
        const std::string head = line.substr(0, line.find(" | v "));
        for (size_t at = head.find(" c"); at != std::string::npos; at = head.find(" c", at + 2)) {
            unsigned i, format, mask;
            if (sscanf(head.c_str() + at, " c%u=%u:%u:", &i, &format, &mask) == 3 && mask)
                out.insert(format);
        }
    }
    return out;
}

// A line's library (its vertex function's lib= path) where it is a framework's default.metallib other than QuartzCore's
static std::string libraryPath(const std::string &line)
{
    const size_t at = line.find(" lib=");
    if (at == std::string::npos)
        return "";
    const std::string path = line.substr(at + 5, line.find(' ', at + 5) - at - 5);
    const std::string suffix = ".framework/default.metallib";
    if (path.size() < suffix.size() || path.compare(path.size() - suffix.size(), suffix.size(), suffix) ||
        path.find("/QuartzCore.framework/") != std::string::npos)
        return "";
    return path;
}

static std::string frameworkName(const std::string &path)
{
    const size_t end = path.rfind(".framework/"), begin = path.rfind('/', end) + 1;
    return path.substr(begin, end - begin);
}

static id<MTLLibrary> frameworkLibrary(id<MTLDevice> dev, const std::string &path)
{
    static NSMutableDictionary<NSString *, id<MTLLibrary>> *loaded = [NSMutableDictionary dictionary];
    NSString *key = @(path.c_str());
    if (!loaded[key]) {
        const char *root = getenv("PROBE_LIB_ROOT");
        NSError *e = nil;
        if (id<MTLLibrary> l =
                [dev newLibraryWithURL:[NSURL fileURLWithPath:@((std::string(root ? root : "") + path).c_str())]
                                 error:&e])
            loaded[key] = l;
    }
    return loaded[key];
}

constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);

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
        std::vector<std::string> lines;
        const char *labelPrefix = getenv("PROBE_LABEL") ? getenv("PROBE_LABEL") : "com.apple.coreanimation.";
        NSRegularExpression *lib = [NSRegularExpression regularExpressionWithPattern:@" lib=\\S+" options:0 error:nil];
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"R "] || ![l containsString:@" ok=1 "])
                continue;
            if (const std::string path = libraryPath(l.UTF8String); !path.empty()) {
                lines.push_back("@L " + path + " " +
                                [lib stringByReplacingMatchesInString:l
                                                              options:0
                                                                range:NSMakeRange(0, l.length)
                                                         withTemplate:@""]
                                    .UTF8String);
                continue;
            }
            if (*labelPrefix && ![l containsString:[@" label=" stringByAppendingString:@(labelPrefix)]])
                continue;
            NSString *plain = [lib stringByReplacingMatchesInString:l
                                                            options:0
                                                              range:NSMakeRange(0, l.length)
                                                       withTemplate:@""];
            lines.push_back(plain.UTF8String);
        }
        // PROBE_YCBCR_LIB=<metallib>: the raw path's own states after Core Animation's, drawn and read alike: the conversions of
        // YCbCr textures (pack/ycbcr.metal), lines marked "@Y " (their functions are that library's); mkpack.py keys
        // them by name ("raw.ycbcr.<fragment function>"), which the runtime asks for
        id<MTLLibrary> ycbcr = nil;
        if (const char *path = getenv("PROBE_YCBCR_LIB")) {
            ycbcr = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            if (!ycbcr) {
                fprintf(stderr, "ycbcr library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
            for (const char *out :
                 {"115 | v ycbcr_vert | f ycbcr_converted_frag", "125 | v ycbcr_vert | f ycbcr_srgb_frag",
                  "70 | v ycbcr_vert | f ycbcr_codes_frag", "90 | v ycbcr_vert | f ycbcr_codes10_frag"}) {
                std::string o = out;
                const size_t bar = o.find(' ');
                lines.push_back("@Y R ok=1 label=raw.ycbcr samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) +
                                ":15:0:1:0:0:1:0:0" + o.substr(bar));
            }
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        // tagged textures and samplers
        std::vector<id<MTLTexture>> textures;
        for (unsigned t = 0; t < 16; t++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:16 + t
                                                                                         height:8 + t
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            textures.push_back([dev newTextureWithDescriptor:td]);
        }
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
        auto target = [&](MTLPixelFormat f, bool memoryless) -> id<MTLTexture> {
            NSString *k = [NSString stringWithFormat:@"%lu.%d", (unsigned long)f, memoryless];
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
        // draw: min/mag nearest or linear, mip not/nearest/linear, the address modes, max anisotropy 1 or 8, LOD clamps), every
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
                        for (unsigned aniso : {1u, 8u})
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
            for (unsigned format : writtenFormats(lines)) {
                id<MTLTexture> t = target((MTLPixelFormat)format, false);
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
            const bool own = marked.rfind("@Y ", 0) == 0, framework = marked.rfind("@L ", 0) == 0;
            const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
            const std::string line = own ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
            id<MTLLibrary> library = own ? ycbcr : qc;
            if (framework && !(library = frameworkLibrary(dev, marked.substr(3, pathEnd - 3)))) {
                fprintf(stderr, "LIBRARY %s\n", marked.substr(3, pathEnd - 3).c_str());
                refused++;
                continue;
            }
            if (!parseState(library, line, s)) {
                fprintf(stderr, "UNPARSED %s\n", line.substr(0, 80).c_str());
                refused++;
                continue;
            }
            if (framework)
                s.key += " | l " + frameworkName(marked.substr(3, pathEnd - 3));
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
            bool usedFb = depthStencil;
            for (NSUInteger c = 0; c < 8; c++) {
                MTLRenderPipelineColorAttachmentDescriptor *ca = s.d.colorAttachments[c];
                if (ca.pixelFormat == MTLPixelFormatInvalid)
                    continue;
                const bool memoryless = ca.writeMask == 0;
                id<MTLTexture> t = (!memoryless && !usedFb && ca.pixelFormat == MTLPixelFormatBGRA8Unorm)
                                       ? fb
                                       : target(ca.pixelFormat, memoryless);
                usedFb |= t == fb;
                pass.colorAttachments[c].texture = t;
                // a 542 snapshot renders into the frontend's RGBA16Float stand-in, which a load or store would draw from or
                // into: neither, so the state's draw is the pass's one
                const bool planes = ca.pixelFormat == kRGB8_444_2P;
                pass.colorAttachments[c].loadAction = memoryless || planes ? MTLLoadActionDontCare : MTLLoadActionLoad;
                pass.colorAttachments[c].storeAction =
                    memoryless || planes ? MTLStoreActionDontCare : MTLStoreActionStore;
            }
            // a state with depth or stencil formats draws into a memoryless attachment of them (Metal's default Clear load and
            // DontCare store, its default depth/stencil state), so its registers are those NVK writes beside one
            if (depthStencil && s.d.depthAttachmentPixelFormat != MTLPixelFormatInvalid)
                pass.depthAttachment.texture = target(s.d.depthAttachmentPixelFormat, true);
            if (s.d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid)
                pass.stencilAttachment.texture = target(s.d.stencilAttachmentPixelFormat, true);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
            [enc setRenderPipelineState:state];
            // PROBE_BYTES (default 256): the tagged payloads' length; 16 and 4096 tell a uniform from a storage buffer (NVK rounds
            // a uniform buffer's range up to 256) and the function's proven window on each slot (m2v binds at most that)
            static const uint32_t bytes = getenv("PROBE_BYTES") ? atoi(getenv("PROBE_BYTES")) : 256;
            std::vector<uint32_t> tag(bytes / 4);
            for (uint32_t stage = 1; stage <= 2; stage++)
                for (uint32_t slot = 0; slot < gSlots; slot++) {
                    tag[0] = 0xC0DE0000u | stage << 8 | slot;
                    tag[1] = i;
                    for (uint32_t k = 2; k < tag.size(); k++)
                        tag[k] = 0xA0000000u | stage << 20 | slot << 16 | k;
                    if (stage == 1)
                        [enc setVertexBytes:tag.data() length:bytes atIndex:slot];
                    else
                        [enc setFragmentBytes:tag.data() length:bytes atIndex:slot];
                }
            for (unsigned t = 0; t < 16; t++)
                [enc setFragmentTexture:textures[t] atIndex:t];
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
