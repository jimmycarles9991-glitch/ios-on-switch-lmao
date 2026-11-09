// SPDX-License-Identifier: MIT
// raw GM20B path: copies between an RGB10A8_2P_XR10 (format 550) texture and a buffer on the copy engine, over the mock GPU
// (NVDEC_WRITETHROUGH), against Metal's: plane 0 alone, each texel's color word at the buffer's rows (an M4 Max's blits; m2v
// two_planes.rs), the buffer's other bytes and the texture's alpha words as they were. Block-linear and pitch textures.
// usage: xrcopycheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"
#include "TegraGM20BLayout.h"

namespace {

constexpr uint32_t kPlanes = 550, kW = 64, kH = 16, kSentinel = 0xeeeeeeeeu;

uint32_t pattern(uint32_t i)
{
    return i * 2654435761u + 0x1234567u;
}

// The texture's texels (color word, alpha word), as rows of kW
struct Image {
    std::vector<uint32_t> words = std::vector<uint32_t>(kW * kH * 2);

    uint32_t &color(uint32_t x, uint32_t y)
    {
        return words[(y * kW + x) * 2];
    }
};

void access(const raw::Texture &t, Image &image, bool write)
{
    uint8_t *bytes = reinterpret_cast<uint8_t *>(image.words.data());
    tgm_layout l;
    if (t.blockLinear && tgm_layout_tiled(kW, kH, t.bpp, &l)) {
        if (write)
            tgm_layout_write(&l, t.cpu, 0, 0, kW, kH, bytes, kW * 8);
        else
            tgm_layout_read(&l, t.cpu, 0, 0, kW, kH, bytes, kW * 8);
        return;
    }
    for (uint32_t y = 0; y < kH; y++)
        if (write)
            memcpy(t.cpu + uint64_t(y) * t.pitch, bytes + y * kW * 8, kW * 8);
        else
            memcpy(bytes + y * kW * 8, t.cpu + uint64_t(y) * t.pitch, kW * 8);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: xrcopycheck <pack.bin>\n");
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
    auto expect = [&](bool ok, const char *what, bool tiled) {
        std::printf("%s: %s (%s)\n", ok ? "ok  " : "FAIL", what, tiled ? "block-linear" : "pitch");
        failures += !ok;
    };
    for (bool tiled : {true, false}) {
        raw::Texture t;
        raw::Buffer up, wide, tight;
        // a region at (9, 2) of 40 x 9 texels from rows 172 bytes apart; out of the whole texture at rows of 8 and 4 bytes a texel
        constexpr uint32_t kX = 9, kY = 2, kRw = 40, kRh = 9, kUpPitch = 172;
        if (!dev.createTexture(kW, kH, kPlanes, t, false, tiled) || t.blockLinear != tiled ||
            !dev.createBuffer(kUpPitch * kRh, up) || !dev.createBuffer(kW * 8 * kH, wide) ||
            !dev.createBuffer(kW * 4 * kH, tight)) {
            expect(false, "a 64 x 16 RGB10A8_2P_XR10 texture and three buffers", tiled);
            continue;
        }
        Image before;
        for (uint32_t i = 0; i < before.words.size(); i++)
            before.words[i] = pattern(i);
        access(t, before, true);
        for (uint32_t i = 0; i < kUpPitch * kRh / 4; i++)
            reinterpret_cast<uint32_t *>(up.cpu)[i] = pattern(i + 99999);
        memset(wide.cpu, 0xee, kW * 8 * kH);
        memset(tight.cpu, 0xee, kW * 4 * kH);

        Image want = before;
        for (uint32_t y = 0; y < kRh; y++)
            for (uint32_t x = 0; x < kRw; x++)
                want.color(kX + x, kY + y) = reinterpret_cast<const uint32_t *>(up.cpu + y * kUpPitch)[x];

        raw::CommandBuffer *cb = dev.begin();
        uint32_t seq = 0;
        if (raw::BlitEncoder *e = cb ? cb->blit() : nullptr) {
            e->access(&up, &t);
            e->copyBufferTexels(up.va, kUpPitch, t, kX, kY, 0, kRw, kRh, true);
            e->access(&t, &wide);
            e->copyBufferTexels(wide.va, kW * 8, t, 0, 0, 0, kW, kH, false);
            e->access(&t, &tight);
            e->copyBufferTexels(tight.va, kW * 4, t, 0, 0, 0, kW, kH, false);
            e->end();
        }
        expect(cb && cb->commit(seq) && dev.wait(seq), "the copies were submitted and completed", tiled);

        Image got;
        access(t, got, false);
        expect(got.words == want.words, "into the texture: the region's color words, every alpha word kept", tiled);
        bool wideOk = true, tightOk = true;
        for (uint32_t y = 0; y < kH; y++)
            for (uint32_t x = 0; x < kW * 2; x++) {
                const uint32_t w = reinterpret_cast<const uint32_t *>(wide.cpu + y * kW * 8)[x];
                wideOk = wideOk && w == (x < kW ? want.color(x, y) : kSentinel);
            }
        for (uint32_t y = 0; y < kH; y++)
            for (uint32_t x = 0; x < kW; x++)
                tightOk = tightOk && reinterpret_cast<const uint32_t *>(tight.cpu + y * kW * 4)[x] == want.color(x, y);
        expect(wideOk, "out of it at rows of 8 bytes a texel: each row's color words, its second half untouched",
               tiled);
        expect(tightOk, "out of it at rows of 4 bytes a texel: the color words", tiled);
        dev.destroyTexture(t);
        dev.destroyBuffer(up);
        dev.destroyBuffer(wide);
        dev.destroyBuffer(tight);
    }
    if (failures) {
        std::printf("FAIL: xrcopycheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: xrcopycheck\n");
    return 0;
}
