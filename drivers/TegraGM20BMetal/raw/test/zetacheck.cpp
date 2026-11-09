// SPDX-License-Identifier: MIT
// raw GM20B path: a stored depth/stencil texture's copies with a buffer on the copy engine (raw::Device::createZetaOver, the
// zeta memory here a pool buffer as NVK's image would lie in it; BlitEncoder::copyBufferTexels), over the mock GPU (NVDEC_WRITETHROUGH), against Metal's: Depth32Float_Stencil8's depth aspect
// each texel's first word (4 bytes in the buffer), its stencil aspect each texel's fifth byte (1 byte); Depth32Float's texel.
// Out of the texture and back into it, whole and a region at an origin, at rows past the texels' bytes; a buffer's bytes past
// the texels and a texel's other bytes stay as they were.
// usage: zetacheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"
#include "TegraGM20BLayout.h"

namespace {

uint8_t pattern(uint32_t i)
{
    return uint8_t((i * 2654435761u + 0x9e3779b9u) >> 24);
}

// Byte b of texel (x, y) of a stored zeta texture over `cpu`, as the copy engine addresses it
uint8_t &texelByte(uint8_t *cpu, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t b)
{
    return cpu[tgm_layout_offset_of(t.pitch, t.blockHeight, x * t.bpp + b, y)];
}

struct Region {
    uint32_t x, y, w, h;
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: zetacheck <pack.bin>\n");
        return 2;
    }
    setenv("NVDEC_WRITETHROUGH", "1", 0);
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    auto run = [&](auto encode) {
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return false;
        encode(e);
        e->end();
        uint32_t seq = 0;
        return cb->commit(seq) && dev.wait(seq);
    };

    struct Case {
        uint32_t format, w, h;
        Region region;
    };

    const Case cases[] = {{260, 64, 64, {0, 0, 64, 64}}, {260, 37, 5, {3, 2, 30, 3}}, {260, 70, 33, {9, 17, 53, 16}},
                          {252, 64, 64, {0, 0, 64, 64}}, {252, 37, 5, {3, 2, 30, 3}}, {253, 64, 64, {0, 0, 64, 64}},
                          {253, 70, 33, {9, 17, 53, 16}}};
    for (const Case &c : cases) {
        raw::Texture t;
        char what[96];
        std::snprintf(what, sizeof what, "format %u %ux%u, region %u,%u %ux%u", c.format, c.w, c.h, c.region.x,
                      c.region.y, c.region.w, c.region.h);
        auto expect = [&](bool ok, const char *check) {
            std::printf("%s: %s: %s\n", ok ? "ok  " : "FAIL", what, check);
            failures += !ok;
        };
        raw::Texture shape;
        uint64_t bytes = 0, align = 0;
        raw::Buffer memory;
        if (!raw::zetaShape(c.w, c.h, c.format, 1, shape, bytes, align) || !dev.createBuffer(bytes, memory)) {
            expect(false, "zeta memory");
            continue;
        }
        const raw::ZetaMemory m{memory.va, bytes, shape.layerBytes, shape.pitch, shape.bpp, shape.blockHeight, 0,
                                true,      false};
        const char *declined = nullptr;
        if (!dev.createZetaOver(m, c.w, c.h, c.format, false, t, &declined)) {
            expect(false, declined);
            continue;
        }
        expect(t.zeta && !t.memoryless && t.va == memory.va && !t.tic && (c.format != 260 || dev.stencilTemp(t)),
               "a stored zeta texture over that memory");
        raw::Texture sampled;
        raw::ZetaMemory wide = m;
        wide.pitch += 64;
        wide.bytes += uint64_t(64) * c.h;
        expect(dev.createZetaOver(m, c.w, c.h, c.format, true, sampled, &declined) && sampled.tic &&
                   !dev.createZetaOver(wide, c.w, c.h, c.format, true, shape, &declined),
               "sampled: a texture header, only where its rows are the ones a header implies");
        dev.destroyTexture(sampled);
        auto texelByte = [&](const raw::Texture &z, uint32_t x, uint32_t y, uint32_t b) -> uint8_t & {
            return ::texelByte(memory.cpu, z, x, y, b);
        };
        for (uint32_t y = 0; y < c.h; y++)
            for (uint32_t x = 0; x < c.w; x++)
                for (uint32_t b = 0; b < t.bpp; b++)
                    texelByte(t, x, y, b) = pattern((y * c.w + x) * 8 + b);
        const Region r = c.region;
        const std::vector<uint8_t> aspects = c.format == 260
                                                 ? std::vector<uint8_t>{raw::kAspectDepth, raw::kAspectStencil}
                                                 : std::vector<uint8_t>{raw::kAspectWhole};
        for (uint8_t aspect : aspects) {
            const uint32_t unit = raw::bufferUnit(t, aspect), offset = aspect == raw::kAspectStencil ? 4 : 0,
                           pitch = r.w * unit + 12;
            raw::Buffer out, in;
            if (!dev.createBuffer(uint64_t(pitch) * r.h, out) || !dev.createBuffer(uint64_t(pitch) * r.h, in)) {
                expect(false, "buffers");
                continue;
            }
            memset(out.cpu, 0xee, uint64_t(pitch) * r.h);
            std::vector<uint8_t> want(uint64_t(pitch) * r.h, 0xee);
            for (uint32_t y = 0; y < r.h; y++)
                for (uint32_t x = 0; x < r.w; x++)
                    for (uint32_t b = 0; b < unit; b++)
                        want[y * pitch + x * unit + b] = texelByte(t, r.x + x, r.y + y, offset + b);
            const bool out1 = run([&](raw::BlitEncoder *e) {
                e->copyBufferTexels(out.va, pitch, t, r.x, r.y, 0, r.w, r.h, false, aspect);
            });
            expect(out1 && !memcmp(out.cpu, want.data(), want.size()), aspect == raw::kAspectStencil || c.format == 253
                                                                           ? "out: the stencil bytes"
                                                                           : "out: the depth words");

            for (uint32_t i = 0; i < uint64_t(pitch) * r.h; i++)
                in.cpu[i] = pattern(i + 77777 * (aspect + 1));
            std::vector<std::vector<uint8_t>> texels(c.h, std::vector<uint8_t>(c.w * t.bpp));
            for (uint32_t y = 0; y < c.h; y++)
                for (uint32_t x = 0; x < c.w; x++)
                    for (uint32_t b = 0; b < t.bpp; b++)
                        texels[y][x * t.bpp + b] = texelByte(t, x, y, b);
            for (uint32_t y = 0; y < r.h; y++)
                for (uint32_t x = 0; x < r.w; x++)
                    for (uint32_t b = 0; b < unit; b++)
                        texels[r.y + y][(r.x + x) * t.bpp + offset + b] = in.cpu[y * pitch + x * unit + b];
            const bool in1 = run([&](raw::BlitEncoder *e) {
                e->copyBufferTexels(in.va, pitch, t, r.x, r.y, 0, r.w, r.h, true, aspect);
            });
            size_t differ = 0;
            for (uint32_t y = 0; y < c.h; y++)
                for (uint32_t x = 0; x < c.w; x++)
                    for (uint32_t b = 0; b < t.bpp; b++)
                        if (!(aspect == raw::kAspectStencil && b == 5))  // the X24 padding's first byte: the temp's
                            differ += texelByte(t, x, y, b) != texels[y][x * t.bpp + b];
            char check[96];
            std::snprintf(check, sizeof check, "in: %s, every other byte kept (%zu differ)",
                          aspect == raw::kAspectStencil || c.format == 253 ? "the stencil bytes" : "the depth words",
                          differ);
            expect(in1 && !differ, check);
            dev.destroyBuffer(out);
            dev.destroyBuffer(in);
        }
        dev.destroyTexture(t);
        dev.destroyBuffer(memory);
    }
    if (failures) {
        std::printf("FAIL: zetacheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: zetacheck\n");
    return 0;
}
