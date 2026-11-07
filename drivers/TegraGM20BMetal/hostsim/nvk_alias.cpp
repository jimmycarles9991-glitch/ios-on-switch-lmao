// SPDX-License-Identifier: MIT
// The raw GM20B path's memory as NVK sees it in place: a chunk the raw path bound on NVK's connection (raw_kernel.h), handed
// to NVK as foreign memory (switch_nvk_import_foreign) and viewed by a Vulkan image of the raw texture's shape:
//  - pitch: a pool chunk (PTE kind 0) and a VK_IMAGE_TILING_LINEAR image at a span inside it, as wide as the raw texture's
//    rows (m2v's texture in place takes the row pitch itself);
//  - block-linear: a mip chain's chunk (fx::kBlockLinearKind) and a VK_IMAGE_TILING_OPTIMAL image of the chain's format, size
//    and levels, whose texels raw/fx placed (and NVK writes back where raw/fx reads them);
//  - a block-linear pool span: a one-level VK_IMAGE_TILING_OPTIMAL image at a span inside a kind-0xFE chunk, whose texels
//    TegraGM20BLayout.h placed.
// For each: NVK's GPU address of the memory is the raw path's VA, NIL's layout is the raw path's (VK_EXT_host_image_copy reads
// and writes through it on the CPU), the view costs no BoImport, BoAlloc or VmBind, and freeing it drops the raw path's
// lease with no BoFree or VmBind.
// usage: nvk_alias <host libvulkan_nouveau.dylib>
#include <mach/mach.h>
#include <string>
#include "../raw/fx/fx_mip.h"
#include "../raw/runtime/raw_kernel.h"
#include "TegraGM20BLayout.h"
#include "nvk_host.h"

namespace {
using host::Calls;
using host::expect;
using host::Vk;
int holds, drops;

void hold(void *)
{
    holds++;
}

void drop(void *)
{
    drops++;
}

uint8_t texel(uint32_t level, uint32_t x, uint32_t y, uint32_t c, uint32_t seed)
{
    return uint8_t(level * 61u + x * 7u + y * 13u + c * 29u + seed);
}

std::vector<uint8_t> image(uint32_t w, uint32_t h, uint32_t level, uint32_t seed)
{
    std::vector<uint8_t> v(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t c = 0; c < 4; c++)
                v[(size_t(y) * w + x) * 4 + c] = texel(level, x, y, c, seed);
    return v;
}

// NVK's memory over [cpu, cpu + bytes) of `bo` (whose first byte is `base`), as the frontend makes it
VkDeviceMemory foreignMemory(const Vk &vk, const raw::Bo &bo, const uint8_t *base, uint8_t *cpu, uint64_t bytes,
                             uint32_t kind)
{
    using Foreign =
        void (*)(const void *, uint64_t, uint64_t, uint64_t, uint32_t, void (*)(void *), void (*)(void *), void *);
    const auto foreign = vk.symbol<Foreign>("switch_nvk_import_foreign");
    foreign(base, bo.bytes, bo.handle, bo.vaBase, kind, hold, drop, nullptr);
    VkDeviceMemory memory = vk.hostMemory(cpu, bytes);
    foreign(nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr);
    return memory;
}

// A raw chunk of `bytes`, bound with PTE kind `kind` on the raw path's connection
uint8_t *chunk(raw::Kernel &k, uint64_t bytes, uint32_t kind, raw::Bo &bo)
{
    vm_address_t at = 0;
    vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE);
    uint8_t *cpu = reinterpret_cast<uint8_t *>(at);
    expect(k.importPages(cpu, bytes, bo) && k.bindImport(cpu, bo, kind), "the raw path's chunk is bound");
    return cpu;
}

// Frees the view; the lease is dropped once, with no kernel call
void release(Vk &vk, VkImage image, VkDeviceMemory memory, const char *what)
{
    const int dropped = drops;
    const Calls before = Calls::now();
    vk.vkDestroyImage(vk.device, image, nullptr);
    vk.vkFreeMemory(vk.device, memory, nullptr);
    const Calls after = Calls::now();
    char line[200];
    snprintf(line, sizeof line, "%s: freeing the view dropped the lease (%d) with no BoFree (%lu) or VmBind (%lu)",
             what, drops - dropped, after.frees - before.frees, after.binds - before.binds);
    expect(drops - dropped == 1 && after == before, line);
}

// A pool span of a raw pitch texture (rows 256-byte aligned, as raw::Device::createTexture lays them) in a kind-0 chunk
void pitch(Vk &vk, raw::Kernel &k)
{
    const uint32_t rowPitch = 512, w = rowPitch / 4, h = 60;
    const uint64_t chunkBytes = 1 << 20, spanOffset = 0x40000, spanBytes = uint64_t(rowPitch) * h;
    raw::Bo bo;
    uint8_t *base = chunk(k, chunkBytes, 0, bo), *span = base + spanOffset;
    const std::vector<uint8_t> texels = image(w, h, 0, 3);
    memcpy(span, texels.data(), texels.size());
    const Calls before = Calls::now();
    const int held = holds;
    VkDeviceMemory memory = foreignMemory(vk, bo, base, span, (spanBytes + 0x3fff) & ~uint64_t(0x3fff), 0);
    VkMemoryRequirements req = {};
    VkImage image =
        memory ? vk.image(memory, 0, VK_IMAGE_TILING_LINEAR, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1, req) : VK_NULL_HANDLE;
    expect(image && holds - held == 1, "pitch: NVK's memory over the span holds the raw path's lease");
    char line[200];
    const uint64_t va = memory ? vk.address(memory, spanBytes) : 0;
    snprintf(line, sizeof line, "pitch: NVK's GPU address of the span is the raw path's VA (%llx, %llx)",
             (unsigned long long)va, (unsigned long long)(bo.vaBase + spanOffset));
    expect(va == bo.vaBase + spanOffset, line);
    VkSubresourceLayout layout = {};
    const VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    if (image)
        vk.vkGetImageSubresourceLayout(vk.device, image, &sub, &layout);
    snprintf(line, sizeof line,
             "pitch: NIL's linear image has the raw texture's row pitch (%llu, %u) at offset 0 (%llu)",
             (unsigned long long)layout.rowPitch, rowPitch, (unsigned long long)layout.offset);
    expect(layout.rowPitch == rowPitch && !layout.offset, line);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line, "pitch: the view cost no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             viewed.imports - before.imports, viewed.allocs - before.allocs, viewed.binds - before.binds);
    expect(viewed == before, line);
    std::vector<uint8_t> got;
    expect(image && vk.read(image, 0, w, h, 4, got) && got == texels, "pitch: NVK reads the texels the raw path wrote");
    if (image)
        release(vk, image, memory, "pitch");
    k.free(bo);
}

// A raw mip chain (raw/fx's layout) in a kind-0xFE chunk
void blockLinear(Vk &vk, raw::Kernel &k)
{
    const uint32_t w = 100, h = 60, levels = 7;
    raw::fx::MipChain c;
    expect(raw::fx::mipChainLayout(w, h, levels, 4, c), "block-linear: raw/fx lays out the chain");
    raw::Bo bo;
    uint8_t *base = chunk(k, 1 << 20, raw::fx::kBlockLinearKind, bo);
    for (uint32_t l = 0; l < levels; l++) {
        const std::vector<uint8_t> v = image(c.level[l].width, c.level[l].height, l, 1);
        raw::fx::writeLevel(c, base, l, 0, 0, c.level[l].width, c.level[l].height, v.data(), c.level[l].width * 4);
    }
    const Calls before = Calls::now();
    const int held = holds;
    VkDeviceMemory memory = foreignMemory(vk, bo, base, base, 1 << 20, raw::fx::kBlockLinearKind);
    VkMemoryRequirements req = {};
    VkImage image = memory ? vk.image(memory, 0, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_B8G8R8A8_UNORM, w, h, levels, req)
                           : VK_NULL_HANDLE;
    expect(image && holds - held == 1, "block-linear: NVK's memory over the chain holds the raw path's lease");
    char line[200];
    const uint64_t va = memory ? vk.address(memory, c.bytes) : 0;
    snprintf(line, sizeof line, "block-linear: NVK's GPU address of the chain is the raw path's VA (%llx, %llx)",
             (unsigned long long)va, (unsigned long long)bo.vaBase);
    expect(va == bo.vaBase, line);
    snprintf(line, sizeof line, "block-linear: NIL's image is raw/fx's chain size (%llu, %llu)",
             (unsigned long long)req.size, (unsigned long long)c.bytes);
    expect(req.size == c.bytes, line);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line, "block-linear: the view cost no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             viewed.imports - before.imports, viewed.allocs - before.allocs, viewed.binds - before.binds);
    expect(viewed == before, line);
    for (uint32_t l = 0; image && l < levels; l++) {
        std::vector<uint8_t> got;
        snprintf(line, sizeof line, "block-linear: level %u (%ux%u): NVK reads the texels raw/fx wrote", l,
                 c.level[l].width, c.level[l].height);
        expect(vk.read(image, l, c.level[l].width, c.level[l].height, 4, got) &&
                   got == ::image(c.level[l].width, c.level[l].height, l, 1),
               line);
    }
    if (image) {
        const uint32_t l = 2;
        const std::vector<uint8_t> v = ::image(c.level[l].width, c.level[l].height, l, 9);
        std::vector<uint8_t> got(v.size(), 0);
        const bool written = vk.write(image, l, c.level[l].width, c.level[l].height, v);
        raw::fx::readLevel(c, base, l, 0, 0, c.level[l].width, c.level[l].height, got.data(), c.level[l].width * 4);
        expect(written && got == v, "block-linear: level 2: raw/fx reads the texels NVK wrote");
        release(vk, image, memory, "block-linear");
    }
    k.free(bo);
}

// A block-linear pool span (a private colour texture) in a kind-0xFE chunk, its texels where tgm_layout_write puts them
void blockLinearSpan(Vk &vk, raw::Kernel &k)
{
    const uint32_t w = 200, h = 72;
    const uint64_t chunkBytes = 1 << 20, spanOffset = 0x30000;
    tgm_layout l;
    expect(tgm_layout_tiled(w, h, 4, &l), "span: TegraGM20BLayout.h tiles the texture");
    raw::Bo bo;
    uint8_t *base = chunk(k, chunkBytes, TGM_LAYOUT_KIND, bo), *span = base + spanOffset;
    const std::vector<uint8_t> texels = image(w, h, 0, 5);
    tgm_layout_write(&l, span, 0, 0, w, h, texels.data(), uint64_t(w) * 4);
    const Calls before = Calls::now();
    const int held = holds;
    VkDeviceMemory memory = foreignMemory(vk, bo, base, span, (l.bytes + 0x3fff) & ~uint64_t(0x3fff), TGM_LAYOUT_KIND);
    VkMemoryRequirements req = {};
    VkImage image =
        memory ? vk.image(memory, 0, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1, req) : VK_NULL_HANDLE;
    expect(image && holds - held == 1, "span: NVK's memory over the span holds the raw path's lease");
    char line[200];
    const uint64_t va = memory ? vk.address(memory, l.bytes) : 0;
    snprintf(line, sizeof line, "span: NVK's GPU address of the span is the raw path's VA (%llx, %llx)",
             (unsigned long long)va, (unsigned long long)(bo.vaBase + spanOffset));
    expect(va == bo.vaBase + spanOffset, line);
    snprintf(line, sizeof line, "span: NIL's image is TegraGM20BLayout.h's size (%llu, %llu)",
             (unsigned long long)req.size, (unsigned long long)l.bytes);
    expect(req.size == l.bytes, line);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line, "span: the view cost no BoImport (%lu), BoAlloc (%lu) or VmBind (%lu)",
             viewed.imports - before.imports, viewed.allocs - before.allocs, viewed.binds - before.binds);
    expect(viewed == before, line);
    std::vector<uint8_t> got;
    expect(image && vk.read(image, 0, w, h, 4, got) && got == texels,
           "span: NVK reads the texels tgm_layout_write placed");
    if (image)
        release(vk, image, memory, "span");
    k.free(bo);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: nvk_alias <host libvulkan_nouveau.dylib>\n");
        return 2;
    }
    Vk vk;
    if (!vk.open(argv[1])) {
        fprintf(stderr, "no Vulkan device over NVK at %s\n", argv[1]);
        return 1;
    }
    raw::Kernel k;
    std::string err;
    expect(k.open(err) && k.shared(), "the raw path's kernel connection is NVK's");
    pitch(vk, k);
    blockLinear(vk, k);
    blockLinearSpan(vk, k);
    printf("%s\n", host::failures ? "FAILED" : "ALL OK");
    return host::failures ? 1 : 0;
}
