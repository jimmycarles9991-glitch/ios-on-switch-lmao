// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVJPG's boot self-test (J3): embedded JPEGs decoded by the engine and compared with libjpeg-turbo's decode of the
// same bytes (selftest/J3Vectors.h: libjpeg-turbo's encoding of a synthetic picture with fine luma texture and
// chroma ramps, and its integer-IDCT raw YCbCr planes; the host test checks the planes against a fresh decode). NVJPG's
// IDCT is not libjpeg's, so pixels are compared within a tolerance (a few 8-bit steps per pixel, under one on average);
// a decode with a wrong table, stride, plane order or sampling misses by tens. RGB expectations are the reference
// planes through the JFIF matrix the job programs (TegraNVJPGJpeg.h), with the nearest chroma sample, and a looser
// tolerance for NVJPG's own chroma upsampling. Kernel buffers in the arena (mapped 1:1), one job at a time.
//
//   translation  J3A (44x40 4:2:0, restart markers) to BGRA at an identity IOVA whose page table entries send it to
//                other pages: it must land there, not at the bus address (the SMMU translates the nvjpg swgroup)
//   core         J3A to NV12 and to BGRA, each decoded twice over guard bytes 0x00 and 0xff: the picture within
//                tolerance, nothing written past the MCU grid, and whether the partial MCU column and row are written
//                (TegraNVJPGTraits.clipped)
//   windows      J3A to BGRA with the scan's bytes read through the input window and the picture written through the
//                output window, its pages mapped in reverse order (the service's path)
//   optional     RGBA, ARGB and I420 targets; J3B (4:2:2, optimised Huffman tables) and J3C (4:4:4) to NV12 and BGRA;
//                J3D (grey, restart every MCU) to grey; J3A at 1/2, 1/4 and 1/8 (DCT scaling) to NV12 and BGRA against
//                libjpeg-turbo's scaled decode. Each passing case turns its trait on; the service refuses the rest
//                (the caller decodes on the CPU).
// The translation, core and window cases must pass, else one "J3 FAIL" line with what was seen, and the service is not
// published. A job that times out or loses its interrupt fails J3 whatever the case.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "TegraNVJPG.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "selftest/J3Vectors.h"

namespace {
TegraNVJPGBuffer gBits, gOut, gPageA, gPageB;
uint32_t gJobs;
uint64_t gBusyUs, gCycles, gPixels;
char gWhy[200];

constexpr uint32_t kGuardRows = 8;
// Planes in the output buffer, 256-byte aligned. YUV cases use a 128-byte pitch, BGRA 256: both leave guard columns
// past the MCU grid's last 64-byte burst (48 -> 64 bytes of luma, 192 of BGRA), 8 guard rows below it.
constexpr uint32_t kPlaneOffset[3] = {0, 8192, 12288};

struct Vector {
    const char *name;
    const uint8_t *jpeg;
    uint32_t bytes;
    uint32_t w, h;            // the picture at this scale
    const uint8_t *plane[3];  // libjpeg-turbo's raw planes
    uint32_t cw, ch;          // their chroma size
    uint32_t scale;
};

const Vector kA = {"J3A", kJ3AJpeg, sizeof(kJ3AJpeg), 44, 40, {kJ3APlane0, kJ3APlane1, kJ3APlane2}, 22, 20, 0};
const Vector kB = {"J3B", kJ3BJpeg, sizeof(kJ3BJpeg), 40, 24, {kJ3BPlane0, kJ3BPlane1, kJ3BPlane2}, 20, 24, 0};
const Vector kC = {"J3C", kJ3CJpeg, sizeof(kJ3CJpeg), 24, 16, {kJ3CPlane0, kJ3CPlane1, kJ3CPlane2}, 24, 16, 0};
const Vector kD = {"J3D", kJ3DJpeg, sizeof(kJ3DJpeg), 24, 16, {kJ3DPlane0, nullptr, nullptr}, 0, 0, 0};
const Vector kAScaled[3] = {
    {"J3A/2", kJ3AJpeg, sizeof(kJ3AJpeg), 22, 20, {kJ3AScale1Plane0, kJ3AScale1Plane1, kJ3AScale1Plane2}, 22, 20, 1},
    {"J3A/4", kJ3AJpeg, sizeof(kJ3AJpeg), 11, 10, {kJ3AScale2Plane0, kJ3AScale2Plane1, kJ3AScale2Plane2}, 11, 10, 2},
    {"J3A/8", kJ3AJpeg, sizeof(kJ3AJpeg), 6, 5, {kJ3AScale3Plane0, kJ3AScale3Plane1, kJ3AScale3Plane2}, 6, 5, 3},
};

inline uint32_t refY(const Vector &v, uint32_t x, uint32_t y)
{
    return v.plane[0][y * v.w + x];
}

// Chroma k (1 Cb, 2 Cr) at picture coordinates: the reference sample covering them (neutral for a grey file).
inline uint32_t refC(const Vector &v, uint32_t k, uint32_t x, uint32_t y)
{
    if (!v.plane[k])
        return 128;
    return v.plane[k][(y * v.ch / v.h) * v.cw + x * v.cw / v.w];
}

// A 4:2:0 output sample: the mean of the chroma over its (up to) 2x2 pixels.
uint32_t refC420(const Vector &v, uint32_t k, uint32_t i, uint32_t j)
{
    uint32_t sum = 0, count = 0;
    for (uint32_t y = 2 * j; y < 2 * j + 2 && y < v.h; y++)
        for (uint32_t x = 2 * i; x < 2 * i + 2 && x < v.w; x++, count++)
            sum += refC(v, k, x, y);
    return (sum + count / 2) / count;
}

struct Tolerance {
    uint32_t max, mean4;  // largest step, and 4 x the mean step
};

const Tolerance kLuma = {4, 3}, kChroma = {4, 3}, kConverted = {6, 6}, kRgb = {16, 12};
const Tolerance kScaledLuma = {24, 12}, kScaledChroma = {12, 8}, kScaledRgb = {32, 16};

struct Score {
    uint32_t max, count;
    uint64_t sum;
    uint32_t atX, atY, got, want;

    void add(uint32_t x, uint32_t y, uint32_t g, uint32_t w)
    {
        const uint32_t d = g > w ? g - w : w - g;
        if (d > max || !count)
            max = d, atX = x, atY = y, got = g, want = w;
        sum += d;
        count++;
    }

    bool within(const Tolerance &t) const
    {
        return count && max <= t.max && sum * 4 <= (uint64_t)t.mean4 * count;
    }
};

struct Output {
    uint64_t iova[3];  // where the engine writes each plane
    uint8_t *va[3];    // where the CPU reads it
    uint32_t pitch[3];
};

// One decode of `v` into `out` (`format`, `v.scale`), the scan's bytes read at `bits` (IOVA; the CPU copy goes to
// gBits). 0, or -1 with gWhy set; an engine failure (timeout, lost interrupt) is -2.
int decode(const Vector &v, uint32_t format, uint64_t bits, const Output &out, nvjpg_dec_status *status)
{
    TegraNVJPGImage img;
    const char *why = tegra_nvjpg_parse(v.jpeg, v.bytes, &img);
    if (why) {
        snprintf(gWhy, sizeof(gWhy), "%s does not parse: %s", v.name, why);
        return -1;
    }
    const uint32_t scan = img.scanEnd - img.scanOffset;
    if (scan + 2 > gBits.size) {
        snprintf(gWhy, sizeof(gWhy), "%s scan of %u bytes", v.name, scan);
        return -1;
    }
    memcpy(gBits.va, v.jpeg + img.scanOffset, scan);
    uint32_t bytes = scan;
    if (!img.eoi)
        gBits.va[bytes++] = 0xff, gBits.va[bytes++] = 0xd9;
    switch_dcache_clean(gBits.va, bytes);
    static TegraNVJPGJob job;  // the setup is 2.8 KiB: not on the kernel stack (J3 runs on one thread)
    bzero(&job, sizeof(job));
    tegra_nvjpg_setup_image(&img, bytes, &job.setup);
    TegraNVJPGTarget target = {format, v.scale, {out.pitch[0], out.pitch[1], out.pitch[2]}, 0xff};
    TegraNVJPGExtent extent;
    if ((why = tegra_nvjpg_setup_output(&img, &target, 1, &job.setup, &extent))) {
        snprintf(gWhy, sizeof(gWhy), "%s target refused: %s", v.name, why);
        return -1;
    }
    job.bitstream = bits;
    job.planes = extent.planes;
    for (uint32_t p = 0; p < extent.planes; p++)
        job.plane[p] = out.iova[p];
    job.pixels = (uint64_t)img.mcusX * img.mcuW * img.mcusY * img.mcuH;
    uint64_t us = 0;
    const int ret = tegra_nvjpg_run(&job, status, &us);
    gJobs++;
    gBusyUs += us;
    if (ret) {
        char host[200];
        tegra_host1x_describe(host, sizeof(host));
        snprintf(gWhy, sizeof(gWhy), "%s job %s (%d) after %llu us; %s", v.name,
                 ret == TEGRA_HOST1X_ELOST ? "lost its interrupt" : "did not complete", ret, us, host);
        return -2;
    }
    if (status->error_status || !status->bytes_offset) {
        snprintf(gWhy, sizeof(gWhy), "%s decode error %#x after %u of %u bytes (MCU %u,%u, %u cycles)", v.name,
                 status->error_status, status->bytes_offset, bytes, status->mcu_x, status->mcu_y, status->reserved0);
        return -1;
    }
    if (!v.scale && format == TEGRA_NVJPG_OUT_BGRA && v.jpeg == kJ3AJpeg) {
        gCycles = status->reserved0;  // the cycle count (DUMP_CYCLE_COUNT; NVIDIA's later header: cycle_count)
        gPixels = (uint64_t)v.w * v.h;
    }
    return 0;
}

// The picture against the reference (visible pixels only). Formats' byte orders: BGRA B,G,R,A; RGBA R,G,B,A; ARGB
// A,R,G,B.
bool compare(const Vector &v, uint32_t format, const Output &out)
{
    const bool scaled = v.scale != 0;
    Score y = {}, c = {}, rgb = {}, alpha = {};
    if (format == TEGRA_NVJPG_OUT_NV12 || format == TEGRA_NVJPG_OUT_I420 || format == TEGRA_NVJPG_OUT_GREY) {
        for (uint32_t j = 0; j < v.h; j++)
            for (uint32_t i = 0; i < v.w; i++)
                y.add(i, j, out.va[0][j * out.pitch[0] + i], refY(v, i, j));
        if (format != TEGRA_NVJPG_OUT_GREY)
            for (uint32_t j = 0; j < (v.h + 1) / 2; j++)
                for (uint32_t i = 0; i < (v.w + 1) / 2; i++)
                    for (uint32_t k = 1; k <= 2; k++) {
                        const uint8_t got = format == TEGRA_NVJPG_OUT_NV12
                                                ? out.va[1][j * out.pitch[1] + 2 * i + (k - 1)]
                                                : out.va[k][j * out.pitch[k] + i];
                        c.add(i, j, got, refC420(v, k, i, j));
                    }
        const bool native420 = v.cw * 2 >= v.w && v.cw * 2 <= v.w + 1 && v.ch * 2 >= v.h && v.ch * 2 <= v.h + 1;
        const bool ok = y.within(scaled ? kScaledLuma : kLuma) &&
                        (format == TEGRA_NVJPG_OUT_GREY || c.within(scaled      ? kScaledChroma
                                                                    : native420 ? kChroma
                                                                                : kConverted));
        if (!ok)
            snprintf(gWhy, sizeof(gWhy),
                     "%s format %u: luma max %u mean %llu/%u (at %u,%u got %u want %u), chroma max %u mean %llu/%u (at "
                     "%u,%u got %u want %u)",
                     v.name, format, y.max, y.sum, y.count, y.atX, y.atY, y.got, y.want, c.max, c.sum, c.count, c.atX,
                     c.atY, c.got, c.want);
        return ok;
    }
    const uint32_t r = format == TEGRA_NVJPG_OUT_BGRA ? 2 : format == TEGRA_NVJPG_OUT_RGBA ? 0 : 1;
    const uint32_t b = format == TEGRA_NVJPG_OUT_BGRA ? 0 : format == TEGRA_NVJPG_OUT_RGBA ? 2 : 3;
    const uint32_t g = format == TEGRA_NVJPG_OUT_ARGB ? 2 : 1, a = format == TEGRA_NVJPG_OUT_ARGB ? 0 : 3;
    for (uint32_t j = 0; j < v.h; j++)
        for (uint32_t i = 0; i < v.w; i++) {
            uint8_t want[3];
            tegra_nvjpg_jfif_rgb(refY(v, i, j), refC(v, 1, i, j), refC(v, 2, i, j), want);
            const uint8_t *px = out.va[0] + j * out.pitch[0] + 4 * i;
            rgb.add(i, j, px[r], want[0]);
            rgb.add(i, j, px[g], want[1]);
            rgb.add(i, j, px[b], want[2]);
            alpha.add(i, j, px[a], 0xff);
        }
    const bool ok = rgb.within(scaled ? kScaledRgb : kRgb) && alpha.max == 0;
    if (!ok)
        snprintf(gWhy, sizeof(gWhy), "%s format %u: RGB max %u mean %llu/%u (at %u,%u got %u want %u), alpha max %u",
                 v.name, format, rgb.max, rgb.sum, rgb.count, rgb.atX, rgb.atY, rgb.got, rgb.want, alpha.max);
    return ok;
}

// The output buffer as the core cases lay it out: planes at kPlaneOffset, `pitch` bytes per row.
Output layout(uint32_t format, uint32_t pitch)
{
    Output o = {};
    const uint32_t planes = tegra_nvjpg_planes(format);
    for (uint32_t p = 0; p < planes; p++) {
        o.iova[p] = gOut.pa + kPlaneOffset[p];
        o.va[p] = gOut.va + kPlaneOffset[p];
        o.pitch[p] = pitch;
    }
    return o;
}

void prefill(uint8_t guard)
{
    memset(gOut.va, guard, gOut.size);
    switch_dcache_clean_invalidate(gOut.va, gOut.size);
}

// A case on the 1:1 output buffer: 0 passed, -1 failed (gWhy), -2 engine failure.
int run(const Vector &v, uint32_t format, uint32_t pitch)
{
    nvjpg_dec_status status;
    prefill(0x5a);
    const Output out = layout(format, pitch);
    const int ret = decode(v, format, gBits.pa, out, &status);
    if (ret)
        return ret;
    switch_dcache_clean_invalidate(gOut.va, gOut.size);
    return compare(v, format, out) ? 0 : -1;
}

// The core cases: J3A twice over guard bytes 0x00 and 0xff, so a byte the engine wrote is one that differs from the
// guard in either run. Writes past the MCU grid (its rows rounded up to 64-byte bursts) fail; writes to the partial MCU
// column and row (x 44..47, y 40..47) decide `clipped`.
int extent(uint32_t format, uint32_t pitch, bool *clipped)
{
    static uint8_t first[16384];
    const uint32_t planes = tegra_nvjpg_planes(format), bpp = format == TEGRA_NVJPG_OUT_BGRA ? 4 : 1;
    const uint32_t gridW[2] = {48 * bpp, 48}, gridH[2] = {48, 24}, picW[2] = {44 * bpp, 44}, picH[2] = {40, 20};
    for (uint32_t pass = 0; pass < 2; pass++) {
        nvjpg_dec_status status;
        prefill(pass ? 0xff : 0x00);
        const Output out = layout(format, pitch);
        const int ret = decode(kA, format, gBits.pa, out, &status);
        if (ret)
            return ret;
        switch_dcache_clean_invalidate(gOut.va, gOut.size);
        if (!compare(kA, format, out))
            return -1;
        if (!pass) {
            memcpy(first, gOut.va, sizeof(first));
            continue;
        }
        bool padX = false, padY = false;
        for (uint32_t p = 0; p < planes; p++) {
            const uint32_t rows = gridH[p] + kGuardRows;
            for (uint32_t row = 0; row < rows; row++)
                for (uint32_t col = 0; col < pitch; col++) {
                    const uint32_t at = kPlaneOffset[p] + row * pitch + col;
                    if (first[at] == 0x00 && gOut.va[at] == 0xff)
                        continue;                                              // untouched
                    if (row >= gridH[p] || col >= ((gridW[p] + 63) & ~63u)) {  // a row's last 64-byte burst
                        snprintf(gWhy, sizeof(gWhy),
                                 "J3A format %u writes past its MCU grid: plane %u row %u byte %u (grid %u rows x %u "
                                 "bytes)",
                                 format, p, row, col, gridH[p], gridW[p]);
                        return -1;
                    }
                    if (col >= picW[p] && row < picH[p])
                        padX = true;
                    if (row >= picH[p])
                        padY = true;
                }
        }
        *clipped = !padX && !padY;
    }
    return 0;
}

// J3A to BGRA at IOVA gPageA.pa, whose four page table entries point at gPageB: the picture must be in B, A untouched.
int translation()
{
    memset(gPageA.va, 0x5a, gPageA.size);
    memset(gPageB.va, 0x5a, gPageB.size);
    switch_dcache_clean_invalidate(gPageA.va, gPageA.size);
    switch_dcache_clean_invalidate(gPageB.va, gPageB.size);
    for (uint64_t at = 0; at < gPageA.size; at += 4096)
        tegra_nvjpg_identity_override(gPageA.pa + at, gPageB.pa + at);
    Output out = {};
    out.iova[0] = gPageA.pa;
    out.va[0] = gPageB.va;
    out.pitch[0] = 256;
    nvjpg_dec_status status;
    const int ret = decode(kA, TEGRA_NVJPG_OUT_BGRA, gBits.pa, out, &status);
    for (uint64_t at = 0; at < gPageA.size; at += 4096)
        tegra_nvjpg_identity_override(gPageA.pa + at, gPageA.pa + at);
    if (ret)
        return ret;
    switch_dcache_clean_invalidate(gPageA.va, gPageA.size);
    switch_dcache_clean_invalidate(gPageB.va, gPageB.size);
    uint32_t inA = 0;
    for (uint64_t i = 0; i < gPageA.size; i++)
        inA += gPageA.va[i] != 0x5a;
    if (inA || !compare(kA, TEGRA_NVJPG_OUT_BGRA, out)) {
        char seen[200];
        strlcpy(seen, gWhy, sizeof(seen));
        snprintf(gWhy, sizeof(gWhy),
                 "translation: a decode at IOVA %#llx mapped to %#llx changed %u bytes at the bus "
                 "address; %s",
                 gPageA.pa, gPageB.pa, inA, inA ? "(the SMMU is not translating)" : seen);
        return -1;
    }
    return 0;
}

// The service's path in small: the scan's bytes through the input window, the picture through the output window
// with its four pages in reverse order.
int windows()
{
    uint64_t in[2] = {gBits.pa, gBits.pa + 4096}, outPages[4];
    for (uint32_t i = 0; i < 4; i++)
        outPages[i] = gPageB.pa + (uint64_t)(3 - i) * 4096;
    memset(gPageB.va, 0x5a, gPageB.size);
    switch_dcache_clean_invalidate(gPageB.va, gPageB.size);
    const uint64_t inBase = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_IN, in, 2, false);
    const uint64_t outBase = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_OUT, outPages, 4, true);
    if (!inBase || !outBase) {
        snprintf(gWhy, sizeof(gWhy), "windows: map refused");
        return -1;
    }
    Output out = {};
    out.iova[0] = outBase;
    out.pitch[0] = 256;
    nvjpg_dec_status status;
    const int ret = decode(kA, TEGRA_NVJPG_OUT_BGRA, inBase, out, &status);
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_IN);
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_OUT);
    if (ret)
        return ret;
    switch_dcache_clean_invalidate(gPageB.va, gPageB.size);
    // Undo the permutation into gOut and compare there.
    for (uint32_t i = 0; i < 4; i++)
        memcpy(gOut.va + i * 4096, gPageB.va + (3 - i) * 4096, 4096);
    Output cpu = layout(TEGRA_NVJPG_OUT_BGRA, 256);
    if (!compare(kA, TEGRA_NVJPG_OUT_BGRA, cpu)) {
        char seen[200];
        strlcpy(seen, gWhy, sizeof(seen));
        snprintf(gWhy, sizeof(gWhy), "windows (pages out of order): %s", seen);
        return -1;
    }
    return 0;
}
}  // namespace

extern "C" int tegra_nvjpg_self_test(TegraNVJPGTraits *traits)
{
    bzero(traits, sizeof(*traits));
    if (tegra_nvjpg_buffer(8192, &gBits) || tegra_nvjpg_buffer(16384, &gOut) || tegra_nvjpg_buffer(16384, &gPageA) ||
        tegra_nvjpg_buffer(16384, &gPageB)) {
        IOLog("TegraNVJPG: J3 FAIL no self-test buffers in the arena\n");
        return -1;
    }
    const uint64_t began = switch_now_us();
    tegra_nvjpg_lock();
    int ret = translation();
    bool clipNv12 = false, clipBgra = false;
    if (!ret)
        ret = extent(TEGRA_NVJPG_OUT_NV12, 128, &clipNv12);
    if (!ret)
        ret = extent(TEGRA_NVJPG_OUT_BGRA, 256, &clipBgra);
    if (!ret)
        ret = windows();
    if (ret) {
        tegra_nvjpg_unlock();
        IOLog("TegraNVJPG: J3 FAIL %s\n", gWhy);
        return -1;
    }
    traits->clipped = clipNv12 && clipBgra;
    traits->formats = 1u << TEGRA_NVJPG_OUT_NV12 | 1u << TEGRA_NVJPG_OUT_BGRA;
    traits->chroma = 1u << TEGRA_NVJPG_CHROMA_420;
    traits->scales = 1;
    // Optional cases: each passing one turns its trait on; the first line of each refusal says what was seen. A job
    // that times out or loses its interrupt (-2) is the engine's failure and fails J3.
    char off[256] = "";
    bool engine = true;
    auto attempt = [&](const Vector &v, uint32_t format, uint32_t pitch, const char *what) {
        if (!engine)
            return false;
        const int result = run(v, format, pitch);
        if (result == -2)
            engine = false;
        if (result != -1)
            return result == 0;
        const size_t used = strlen(off);
        snprintf(off + used, sizeof(off) - used, "%s%s", used ? ", " : "", what);
        IOLog("TegraNVJPG: J3 %s off: %s\n", what, gWhy);
        return false;
    };
    if (attempt(kA, TEGRA_NVJPG_OUT_RGBA, 256, "RGBA"))
        traits->formats |= 1u << TEGRA_NVJPG_OUT_RGBA;
    if (attempt(kA, TEGRA_NVJPG_OUT_ARGB, 256, "ARGB"))
        traits->formats |= 1u << TEGRA_NVJPG_OUT_ARGB;
    if (attempt(kA, TEGRA_NVJPG_OUT_I420, 128, "I420"))
        traits->formats |= 1u << TEGRA_NVJPG_OUT_I420;
    // CMPhoto's native 4:2:2 and 4:4:4 destinations: the same YUV path in nvjpg_drv.h's other chroma and memory modes.
    traits->formats |= 1u << TEGRA_NVJPG_OUT_NV16 | 1u << TEGRA_NVJPG_OUT_NV24 | 1u << TEGRA_NVJPG_OUT_YUY2;
    const bool b420 = attempt(kB, TEGRA_NVJPG_OUT_NV12, 128, "4:2:2 to NV12");
    if (attempt(kB, TEGRA_NVJPG_OUT_BGRA, 256, "4:2:2 to BGRA") && b420)
        traits->chroma |= 1u << TEGRA_NVJPG_CHROMA_422H;
    const bool c420 = attempt(kC, TEGRA_NVJPG_OUT_NV12, 128, "4:4:4 to NV12");
    if (attempt(kC, TEGRA_NVJPG_OUT_BGRA, 256, "4:4:4 to BGRA") && c420)
        traits->chroma |= 1u << TEGRA_NVJPG_CHROMA_444;
    if (attempt(kD, TEGRA_NVJPG_OUT_GREY, 128, "grey"))
        traits->formats |= 1u << TEGRA_NVJPG_OUT_GREY, traits->chroma |= 1u << TEGRA_NVJPG_CHROMA_MONO;
    if ((traits->chroma >> TEGRA_NVJPG_CHROMA_MONO & 1) && attempt(kD, TEGRA_NVJPG_OUT_BGRA, 256, "grey to BGRA"))
        traits->greyRgb = true;
    static const char *const kScaleNames[3] = {"1/2", "1/4", "1/8"};
    for (uint32_t s = 0; s < 3; s++) {
        const bool nv12 = attempt(kAScaled[s], TEGRA_NVJPG_OUT_NV12, 128, kScaleNames[s]);
        if (attempt(kAScaled[s], TEGRA_NVJPG_OUT_BGRA, 256, kScaleNames[s]) && nv12)
            traits->scales |= 1u << (s + 1);
    }
    tegra_nvjpg_unlock();
    if (!engine) {
        IOLog("TegraNVJPG: J3 FAIL %s\n", gWhy);
        return -1;
    }
    traits->cyclesPerPixel16 = gPixels ? gCycles * 16 / gPixels : 0;
    IOLog("TegraNVJPG: J3 ok %u decodes in %llu us (engine %llu us): SMMU translates (windows, pages out of order); "
          "formats %#x, sampling %#x, scales %#x; writes %s; 44x40 4:2:0 to BGRA in %llu cycles (%llu per 16 "
          "pixels at %u MHz)%s%s\n",
          gJobs, switch_now_us() - began, gBusyUs, traits->formats, traits->chroma, traits->scales,
          traits->clipped ? "clipped to the picture" : "whole MCUs (destinations must hold the MCU grid)", gCycles,
          traits->cyclesPerPixel16, tegra_nvjpg_clock_mhz(), off[0] ? "; off: " : "", off);
    return 0;
}
