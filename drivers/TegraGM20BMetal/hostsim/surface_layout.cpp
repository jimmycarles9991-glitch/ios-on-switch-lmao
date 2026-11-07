// SPDX-License-Identifier: MIT
// IOSurfaces the kernel lays out block-linear, through NVK as the Metal frontend imports them (SurfaceLayout.h), over raw/pack's
// mock GPU, which decides a surface's layout (method 16) as the kernel does and tiles its rows in place:
//  - a surface its first importer writes: the import asks, the surface is tiled (its rows, written before, now lie as
//    TegraGM20BLayout.h's tgm_layout_write puts them), the import is bound under TGM_LAYOUT_KIND, and an optimal image over it
//    takes that VA (no VmBind) and reads the rows;
//  - another process (a second device, a connection of its own) importing it later is answered tiled, binds it so too, and
//    reads the same texels; another shape is answered OtherShape;
//  - a surface its first importer samples stays pitch for everyone, untouched;
//  - NIL's optimal layout of every texel size the GM20B tiles, over a sweep of extents, is TegraGM20BLayout.h's: the image's
//    size, and every byte a host copy writes where tgm_layout_write puts it.
// usage: surface_layout <host libvulkan_nouveau.dylib>
#include <mach/mach.h>
#include "TegraGM20BLayout.h"
#include "nvk_host.h"

namespace {
using host::Calls;
using host::expect;
using host::Vk;

struct Nvk {
    void (*importSurface)(uint32_t, const void *, uint64_t);
    void (*importShape)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*importLayout)(uint64_t *);
    uint32_t (*layout)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

    explicit Nvk(const Vk &vk)
        : importSurface(vk.symbol<decltype(importSurface)>("switch_nvk_import_surface")),
          importShape(vk.symbol<decltype(importShape)>("switch_nvk_import_surface_shape")),
          importLayout(vk.symbol<decltype(importLayout)>("switch_nvk_import_surface_layout")),
          layout(vk.symbol<decltype(layout)>("switch_nvk_surface_layout"))
    {}
};

struct Surface {
    uint32_t id, width, height, bpp, row;
    uint8_t *cpu;
    uint64_t bytes;
};

Surface surface(uint32_t id, uint32_t width, uint32_t height, uint32_t bpp)
{
    Surface s = {id, width, height, bpp, (width * bpp + 63) & ~63u, nullptr, 0};
    tgm_layout l;
    const uint64_t tiled = tgm_layout_tiled(width, height, bpp, &l) ? l.bytes : 0;
    s.bytes = (std::max<uint64_t>(uint64_t(s.row) * height, tiled) + 0x3fff) & ~uint64_t(0x3fff);
    vm_address_t at = 0;
    vm_allocate(mach_task_self(), &at, s.bytes, VM_FLAGS_ANYWHERE);
    s.cpu = reinterpret_cast<uint8_t *>(at);
    return s;
}

std::vector<uint8_t> texels(uint32_t w, uint32_t h, uint32_t bpp, uint32_t seed)
{
    std::vector<uint8_t> v(size_t(w) * h * bpp);
    for (size_t i = 0; i < v.size(); i++)
        v[i] = uint8_t(i * 131u + (i >> 9) * 7u + seed);
    return v;
}

// Rows of `t` written into the surface as a CPU writer does, `row` bytes apart
void writeRows(const Surface &s, const std::vector<uint8_t> &t)
{
    for (uint32_t y = 0; y < s.height; y++)
        memcpy(s.cpu + size_t(y) * s.row, &t[size_t(y) * s.width * s.bpp], size_t(s.width) * s.bpp);
}

// NVK's import of the surface as the frontend makes it, asking with `flags`: its memory, layout and handle
VkDeviceMemory import(const Vk &vk, const Nvk &nvk, const Surface &s, uint32_t flags, uint32_t &layout,
                      uint64_t &handle)
{
    nvk.importSurface(s.id, s.cpu, s.bytes);
    nvk.importShape(s.width, s.height, s.bpp, s.row, flags);
    VkDeviceMemory memory = vk.hostMemory(s.cpu, s.bytes);
    layout = nvk.importLayout(&handle);
    nvk.importSurface(0, nullptr, 0);
    return memory;
}

// The tiled bytes of `t` (TegraGM20BLayout.h), as long as the layout
std::vector<uint8_t> tiledBytes(const tgm_layout &l, const std::vector<uint8_t> &t)
{
    std::vector<uint8_t> v(l.bytes, 0);
    tgm_layout_write(&l, v.data(), 0, 0, l.width, l.height, t.data(), uint64_t(l.width) * l.bpp);
    return v;
}

void writerFirst(const Vk &a, const Vk &b)
{
    const Nvk nvkA(a), nvkB(b);
    const Surface s = surface(101, 100, 60, 4);
    const std::vector<uint8_t> t = texels(s.width, s.height, s.bpp, 5);
    writeRows(s, t);
    tgm_layout l;
    tgm_layout_tiled(s.width, s.height, s.bpp, &l);
    const unsigned long tiledBinds = mock_iokit_kind_maps(TGM_LAYOUT_KIND);
    uint32_t layout = 0;
    uint64_t handle = 0;
    VkDeviceMemory memory = import(a, nvkA, s, 1, layout, handle);
    char line[200];
    snprintf(line, sizeof line, "writer first: its import asked and was answered tiled (%u), handle %llu", layout,
             (unsigned long long)handle);
    expect(memory && layout == TegraGM20BLayoutTiled && handle, line);
    expect(!memcmp(s.cpu, tiledBytes(l, t).data(), l.bytes),
           "writer first: the rows written before lie tiled as tgm_layout_write puts them");
    expect(mock_iokit_kind_maps(TGM_LAYOUT_KIND) == tiledBinds + 1,
           "writer first: the import is bound under TGM_LAYOUT_KIND");
    const Calls before = Calls::now();
    VkMemoryRequirements req = {};
    VkImage image = a.image(memory, 0, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM, s.width, s.height, 1, req);
    const Calls viewed = Calls::now();
    snprintf(line, sizeof line,
             "writer first: the optimal image (%llu bytes, the layout's %llu) took the import's VA: no VmBind (%lu)",
             (unsigned long long)req.size, (unsigned long long)l.bytes, viewed.binds - before.binds);
    expect(image && req.size == l.bytes && viewed == before, line);
    std::vector<uint8_t> got;
    expect(image && a.read(image, 0, s.width, s.height, s.bpp, got) && got == t,
           "writer first: the image reads the rows written before the decision");

    // another process: a device of its own, importing the surface later, sampling it
    uint32_t layoutB = 0;
    uint64_t handleB = 0;
    VkDeviceMemory memoryB = import(b, nvkB, s, 0, layoutB, handleB);
    snprintf(line, sizeof line, "another process: its import is answered tiled (%u)", layoutB);
    expect(memoryB && layoutB == TegraGM20BLayoutTiled, line);
    const Calls beforeB = Calls::now();
    VkImage imageB = b.image(memoryB, 0, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM, s.width, s.height, 1, req);
    expect(imageB && Calls::now() == beforeB, "another process: its optimal image took its import's VA: no VmBind");
    expect(imageB && b.read(imageB, 0, s.width, s.height, s.bpp, got) && got == t,
           "another process: its image reads the same texels");
    snprintf(line, sizeof line, "another shape over it is answered OtherShape (%u)",
             nvkB.layout(handleB, 50, 60, 4, s.row, 0));
    expect(nvkB.layout(handleB, 50, 60, 4, s.row, 0) == TegraGM20BLayoutOtherShape, line);
    a.vkDestroyImage(a.device, image, nullptr);
    b.vkDestroyImage(b.device, imageB, nullptr);
    a.vkFreeMemory(a.device, memory, nullptr);
    b.vkFreeMemory(b.device, memoryB, nullptr);
}

void samplerFirst(const Vk &a, const Vk &b)
{
    const Nvk nvkA(a), nvkB(b);
    const Surface s = surface(102, 64, 40, 4);
    const std::vector<uint8_t> t = texels(s.width, s.height, s.bpp, 9);
    writeRows(s, t);
    const std::vector<uint8_t> before(s.cpu, s.cpu + s.bytes);
    uint32_t layout = 0, layoutB = 0;
    uint64_t handle = 0, handleB = 0;
    VkDeviceMemory memory = import(a, nvkA, s, 0, layout, handle);
    VkDeviceMemory memoryB = import(b, nvkB, s, 1, layoutB, handleB);
    char line[200];
    snprintf(line, sizeof line, "sampler first: pitch for its importer (%u) and for a later writer (%u)", layout,
             layoutB);
    expect(memory && memoryB && layout == 0 && layoutB == 0, line);
    expect(!memcmp(s.cpu, before.data(), before.size()), "sampler first: its rows are untouched");
    a.vkFreeMemory(a.device, memory, nullptr);
    b.vkFreeMemory(b.device, memoryB, nullptr);
}

// NIL's optimal image of each texel size the GM20B tiles, over extents, against TegraGM20BLayout.h
void sweep(const Vk &vk)
{
    const struct {
        uint32_t bpp;
        VkFormat format;
    } formats[] = {{1, VK_FORMAT_R8_UNORM},
                   {2, VK_FORMAT_R8G8_UNORM},
                   {4, VK_FORMAT_R8G8B8A8_UNORM},
                   {8, VK_FORMAT_R16G16B16A16_UNORM},
                   {16, VK_FORMAT_R32G32B32A32_UINT}};

    const uint32_t extents[][2] = {{1, 1},    {3, 2},      {17, 9},     {64, 8},    {100, 60},  {128, 128},
                                   {333, 97}, {1280, 720}, {720, 1280}, {4096, 16}, {16, 4096}, {2048, 1536}};
    unsigned cases = 0, bad = 0;
    for (const auto &f : formats)
        for (const auto &e : extents) {
            tgm_layout l;
            if (!tgm_layout_tiled(e[0], e[1], f.bpp, &l))
                continue;
            const uint64_t bytes = (l.bytes + 0x3fff) & ~uint64_t(0x3fff);
            vm_address_t at = 0;
            vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE);
            uint8_t *cpu = reinterpret_cast<uint8_t *>(at);
            VkDeviceMemory memory = vk.hostMemory(cpu, bytes);
            VkMemoryRequirements req = {};
            VkImage image =
                memory ? vk.image(memory, 0, VK_IMAGE_TILING_OPTIMAL, f.format, e[0], e[1], 1, req) : VK_NULL_HANDLE;
            const std::vector<uint8_t> t = texels(e[0], e[1], f.bpp, cases);
            const bool same = image && req.size == l.bytes && vk.write(image, 0, e[0], e[1], t) &&
                              !memcmp(cpu, tiledBytes(l, t).data(), l.bytes);
            if (!same) {
                bad++;
                printf("     %ux%u of %u bytes: NIL %llu bytes, TegraGM20BLayout.h %llu\n", e[0], e[1], f.bpp,
                       (unsigned long long)req.size, (unsigned long long)l.bytes);
            }
            cases++;
            if (image)
                vk.vkDestroyImage(vk.device, image, nullptr);
            if (memory)
                vk.vkFreeMemory(vk.device, memory, nullptr);
            vm_deallocate(mach_task_self(), at, bytes);
        }
    char line[160];
    snprintf(line, sizeof line, "NIL's optimal layout is TegraGM20BLayout.h's in %u of %u shapes", cases - bad, cases);
    expect(cases && !bad, line);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: surface_layout <host libvulkan_nouveau.dylib>\n");
        return 2;
    }
    Vk a, b;
    if (!a.open(argv[1]) || !b.open(argv[1])) {
        fprintf(stderr, "no Vulkan devices over NVK at %s\n", argv[1]);
        return 1;
    }
    writerFirst(a, b);
    samplerFirst(a, b);
    sweep(a);
    printf("%s\n", host::failures ? "FAILED" : "ALL OK");
    return host::failures ? 1 : 0;
}
