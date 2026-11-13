// SPDX-License-Identifier: MIT
// cubiccheck: a state drawn with a cubic sampler state (MTLSamplerMinMagFilterCubic, RenderBox's) where it samples
// (raw_cubic.h). The direct compile (runtime/RawDirect.h) makes its cubic variant, whose fragment program is not the
// state's own (m2v's translator emulates the bicubic filter in it) and differs again for a texture of another format family.
// Then through the facade over the mock GPU: the cubic sampler is raw's, a draw with it is raw's on that variant (made once
// for those binds, kept by the state), and a draw with a linear sampler is on the state's own record.
// usage: cubiccheck <nvk dylib> <cache dir> <pack.bin> <cubic.metallib> (test/cubic/cubic.metal)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/RawDirect.h"
#include "../runtime/RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "record_compare.h"

namespace {

constexpr const char *kKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0";
int failed;

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

MTLRenderPipelineDescriptor *descriptor(id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"cubic_vert"];
    d.fragmentFunction = [library newFunctionWithName:@"cubic_frag"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    return d;
}

raw::CubicBinds binds(uint32_t format)
{
    raw::CubicBinds b;
    b.samplers.push_back({0, 0, 0, 0, 0, true});
    b.textures.push_back({0, format});
    return b;
}

NSString *path(id<MTLCommandBuffer> cb)
{
    const SEL sel = NSSelectorFromString(@"switchRawPath");
    return [(id)cb respondsToSelector:sel] ? ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel) : @"device";
}

void directVariants(const char *pack, id<MTLLibrary> library)
{
    raw::Pack p;
    std::string err;
    if (!p.load(pack, 0, err)) {
        expect(false, err.c_str());
        return;
    }
    raw::Direct direct(p);
    raw::RuntimeRecord own, bgra, half;
    std::string whyOwn, whyBgra, whyHalf;
    raw::ProbeTextures plain, cubicBgra, cubicHalf;
    cubicBgra.cubic = binds(MTLPixelFormatBGRA8Unorm);
    cubicHalf.cubic = binds(MTLPixelFormatRGBA16Float);
    MTLRenderPipelineDescriptor *d = descriptor(library);
    const bool made =
        direct.make(d, kKey, own, whyOwn, plain) == raw::CompileOutcome::Made &&
        direct.make(d, kKey + cubicBgra.cubic.key(), bgra, whyBgra, cubicBgra) == raw::CompileOutcome::Made &&
        direct.make(d, kKey + cubicHalf.cubic.key(), half, whyHalf, cubicHalf) == raw::CompileOutcome::Made;
    printf("     own %s; cubic BGRA8 %s; cubic RGBA16Float %s\n", whyOwn.c_str(), whyBgra.c_str(), whyHalf.c_str());
    expect(made, "the state and its two cubic variants are made directly");
    if (!made)
        return;
    printf("     fragment programs: own %zu bytes, cubic BGRA8 %zu, cubic RGBA16Float %zu\n",
           own.programs[1].image.size(), bgra.programs[1].image.size(), half.programs[1].image.size());
    expect(own.programs[1].image != bgra.programs[1].image, "the cubic variant's fragment program is not the state's");
    expect(own.programs[0].image == bgra.programs[0].image, "its vertex program is the state's");
    expect(bgra.programs[1].image != half.programs[1].image, "the cubic variant depends on the texture format");
    for (const auto &[textures, whole] : {std::pair{cubicBgra, &bgra}, std::pair{cubicHalf, &half}}) {
        raw::RuntimeRecord split;
        std::string why;
        const bool madeSplit = direct.make(d, kKey + textures.cubic.key(), split, why, textures, nullptr, true) ==
                               raw::CompileOutcome::Made;
        expect(madeSplit && split.splitTried, "the cubic variant is compiled with its split requested");
        if (!madeSplit)
            continue;
        for (int stage = 0; stage < 2; stage++) {
            const raw::CapturedProgram &a = whole->programs[stage], &b = split.programs[stage];
            expect(a.image == b.image && a.data == b.data && a.cbufTypes == b.cbufTypes,
                   stage ? "split compilation preserves the cubic fragment program"
                         : "split compilation preserves the cubic vertex program");
        }
    }
}

void facade(id<MTLDevice> m2v, const char *pack, const char *cacheDir, NSString *metallib)
{
    NSString *why = nil;
    id<MTLDevice> dev = raw_metal_device_open(pack, 0, m2v, &why);
    if (!dev) {
        expect(false, why.UTF8String);
        return;
    }
    raw_metal_complete_states(cacheDir);
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithURL:[NSURL fileURLWithPath:metallib] error:&error];
    id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:descriptor(library) error:&error];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilter(2);
    sd.lodAverage = YES;
    id<MTLSamplerState> cubic = [dev newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> linear = [dev newSamplerStateWithDescriptor:sd];
    expect([(id)cubic isKindOfClass:[RMSampler class]] && ((RMSampler *)cubic)->_raw.desc.cubic,
           "the cubic sampler is raw's");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> image = [dev newTextureWithDescriptor:td], target = [dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    auto draw = [&](id<MTLSamplerState> s) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        [e setRenderPipelineState:state];
        [e setFragmentTexture:image atIndex:0];
        [e setFragmentSamplerState:s atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return path(cb);
    };
    RMPipeline *p = (RMPipeline *)state;
    NSString *first = draw(cubic), *again = draw(cubic), *plain = draw(linear);
    size_t variants = 0;
    {
        std::lock_guard<std::mutex> l(p->_variantLock);
        for (const auto &[name, made] : p->_variants)
            variants += made != nullptr;
    }
    printf("     paths: cubic %s, again %s, linear %s; variants %zu\n", first.UTF8String, again.UTF8String,
           plain.UTF8String, variants);
    expect([first isEqualToString:@"raw"] && [again isEqualToString:@"raw"], "draws with the cubic sampler are raw's");
    expect(variants == 1, "the state keeps one cubic variant for those binds");
    expect([plain isEqualToString:@"raw"], "a draw with a linear sampler stays raw on the state's own record");
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 5) {
            fprintf(stderr, "usage: cubiccheck <nvk dylib> <cache dir> <pack.bin> <cubic.metallib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "cubiccheck: %s\n", nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> library = [m2v newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&error];
        if (!library) {
            fprintf(stderr, "cubiccheck: %s unread\n", argv[4]);
            return 1;
        }
        directVariants(argv[3], library);
        facade(m2v, argv[3], argv[2], @(argv[4]));
        printf("%s: cubiccheck\n", failed ? "FAIL" : "PASS");
        return failed ? 1 : 0;
    }
}
