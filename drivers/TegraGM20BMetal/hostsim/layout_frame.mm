// SPDX-License-Identifier: MIT
// The Metal frontend's optimal views (native_bridge_metal_texture_tiled_in_place, SurfaceLayout.h) through the whole stack
// (frontend, m2v, NVK) over raw/pack's mock GPU, which decides surfaces' layouts as the kernel does (surface_layout.cpp checks
// the texels through NVK; the mock's copy engine runs no block-linear copy, so m2v reads none back here):
//  - a raw mip chain (raw/fx's levels in a chunk bound under fx::kBlockLinearKind on the connection the raw path shares) as one
//    N-level texture over foreign memory: made, with no BoImport, BoAlloc or VmBind;
//  - an IOSurface whose first texture renders to it: its import asks, the surface is tiled (the rows written before lie as
//    tgm_layout_write puts them) and bound under TGM_LAYOUT_KIND, its texture is the optimal view (no other bind); a texture of
//    another shape and a buffer over it are refused;
//  - an IOSurface whose first texture samples it: pitch, untouched, and a later render target over it is the in-place texture.
// usage: layout_frame <cache dir> <host libvulkan_nouveau.dylib>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <string>
#include <vector>
#include "../raw/fx/fx_mip.h"
#include "../raw/runtime/raw_kernel.h"
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "TegraGM20BLayout.h"
#include "bridge/NativeMetalObjects.h"

@protocol LayoutSurfaceBuffer
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface;
@end

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" unsigned long mock_iokit_kind_maps(unsigned kind);

namespace {
int failures, holds, drops;

void missing(const char *, const char *)
{}

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    failures += !ok;
}

void hold(void *)
{
    holds++;
}

void drop(void *)
{
    drops++;
}

struct Calls {
    unsigned long imports, allocs, binds, tiled;

    static Calls now()
    {
        return {mock_iokit_calls(TegraGM20BMethodBoImport), mock_iokit_calls(TegraGM20BMethodBoAlloc),
                mock_iokit_calls(TegraGM20BMethodVmBind), mock_iokit_kind_maps(TGM_LAYOUT_KIND)};
    }
};

std::vector<uint8_t> texels(size_t n, unsigned seed)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = uint8_t(i * 131u + (i >> 9) * 7u + seed);
    return v;
}

MTLTextureDescriptor *descriptor(unsigned w, unsigned h, unsigned levels, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:levels > 1];
    d.mipmapLevelCount = levels;
    d.usage = usage;
    d.storageMode = MTLStorageModePrivate;
    return d;
}

void chain()
{
    raw::Kernel k;
    std::string err;
    expect(k.open(err) && k.shared(), "chain: the raw path's connection is NVK's");
    const uint32_t w = 100, h = 60, levels = 7, bytes = 1 << 20;
    raw::fx::MipChain c;
    raw::fx::mipChainLayout(w, h, levels, 4, c);
    vm_address_t at = 0;
    vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE);
    uint8_t *cpu = reinterpret_cast<uint8_t *>(at);
    raw::Bo bo;
    expect(k.importPages(cpu, bytes, bo) && k.bindImport(cpu, bo, raw::fx::kBlockLinearKind),
           "chain: the chunk is bound under the block-linear kind");
    const Calls before = Calls::now();
    const switch_foreign_memory memory = {cpu,  bo.bytes, bo.handle, bo.vaBase, raw::fx::kBlockLinearKind,
                                          hold, drop,     nullptr};
    id<MTLBuffer> buffer = native_bridge_metal_buffer_over_foreign(&memory, cpu, bytes, MTLResourceStorageModeShared);
    id<MTLTexture> t =
        buffer
            ? native_bridge_metal_texture_tiled_in_place(buffer, descriptor(w, h, levels, MTLTextureUsageShaderRead), 0)
            : nil;
    const Calls after = Calls::now();
    char line[200];
    snprintf(line, sizeof line,
             "chain: a %u-level texture over it, with no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             unsigned(t.mipmapLevelCount), after.imports - before.imports, after.allocs - before.allocs,
             after.binds - before.binds);
    expect(t && t.mipmapLevelCount == levels && after.imports == before.imports && after.allocs == before.allocs &&
               after.binds == before.binds,
           line);
    t = nil;
    buffer = nil;
    expect(holds == drops && holds > 0, "chain: the views gave back every hold of the raw path's lease");
    k.free(bo);
}

IOSurfaceRef surface(unsigned w, unsigned h, const std::vector<uint8_t> &rows)
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(w * 4),
        (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
    };
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    IOSurfaceLock(s, 0, nullptr);
    for (unsigned y = 0; y < h; y++)
        memcpy((uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s), &rows[size_t(y) * w * 4], w * 4);
    IOSurfaceUnlock(s, 0, nullptr);
    return s;
}

void writerFirst(id<MTLDevice> m2v)
{
    const unsigned w = 100, h = 60;
    const std::vector<uint8_t> rows = texels(w * h * 4, 3);
    IOSurfaceRef s = surface(w, h, rows);
    const Calls before = Calls::now();
    id<MTLTexture> t =
        [m2v newTextureWithDescriptor:descriptor(w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
                            iosurface:s
                                plane:0];
    const Calls after = Calls::now();
    tgm_layout l;
    tgm_layout_tiled(w, h, 4, &l);
    std::vector<uint8_t> tiled(l.bytes, 0);
    std::vector<uint8_t> dense(w * h * 4);
    for (unsigned y = 0; y < h; y++)
        memcpy(&dense[size_t(y) * w * 4], &rows[size_t(y) * w * 4], w * 4);
    tgm_layout_write(&l, tiled.data(), 0, 0, w, h, dense.data(), w * 4);
    char line[200];
    snprintf(line, sizeof line,
             "writer first: its texture is made (one BoImport %lu, one bind under TGM_LAYOUT_KIND %lu, no other %lu)",
             after.imports - before.imports, after.tiled - before.tiled, after.binds - before.binds);
    expect(t && after.imports - before.imports == 1 && after.tiled - before.tiled == 1 &&
               after.binds - before.binds == 1,
           line);
    expect(!memcmp(IOSurfaceGetBaseAddress(s), tiled.data(), l.bytes),
           "writer first: the rows written before lie tiled as tgm_layout_write puts them");
    expect(![m2v newTextureWithDescriptor:descriptor(w / 2, h, 1, MTLTextureUsageShaderRead) iosurface:s plane:0],
           "writer first: a texture of another shape over it is refused");
    expect(![(id<LayoutSurfaceBuffer>)m2v newBufferWithIOSurface:s], "writer first: a buffer over it is refused");
    t = nil;
    CFRelease(s);
}

void samplerFirst(id<MTLDevice> m2v)
{
    const unsigned w = 64, h = 40;
    const std::vector<uint8_t> rows = texels(w * h * 4, 11);
    IOSurfaceRef s = surface(w, h, rows);
    const Calls before = Calls::now();
    id<MTLTexture> sampled = [m2v newTextureWithDescriptor:descriptor(w, h, 1, MTLTextureUsageShaderRead)
                                                 iosurface:s
                                                     plane:0];
    id<MTLTexture> target = [m2v newTextureWithDescriptor:descriptor(w, h, 1, MTLTextureUsageRenderTarget)
                                                iosurface:s
                                                    plane:0];
    const Calls after = Calls::now();
    expect(sampled && target && after.tiled == before.tiled,
           "sampler first: its texture and a later render target over it are made in place, pitch");
    expect(!memcmp(IOSurfaceGetBaseAddress(s), rows.data(), rows.size()), "sampler first: its rows are untouched");
    CFRelease(s);
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: layout_frame <cache dir> <host libvulkan_nouveau.dylib>\n");
            return 2;
        }
        setenv("NVDEC_WRITETHROUGH", "1", 1);
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[2], argv[1], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        @autoreleasepool {
            chain();
        }
        writerFirst(m2v);
        samplerFirst(m2v);
    }
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
