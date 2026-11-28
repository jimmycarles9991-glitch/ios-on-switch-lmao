// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of TegraNVJPGRequest.h (test:request_host_test): AppleJPEGDriver decode requests, as CMPhoto and
// JPEGH1.videodecoder build them, translated into NVJPG plans against made-up destination surfaces shaped as
// CoreVideo lays them out; every refusal, with its kind (UNSUPPORTED: the caller decodes on the CPU; BAD: the request
// contradicts the file or the surfaces).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../TegraNVJPGRequest.h"
#include "../TegraNVJPGRoute.h"
#include "../../TegraGM20B/TegraGM20BLayoutFit.h"

static int failures;
#define CHECK(cond, ...)         \
    do {                         \
        if (!(cond)) {           \
            printf("FAIL ");     \
            printf(__VA_ARGS__); \
            printf("\n");        \
            failures++;          \
        }                        \
    } while (0)

static const uint64_t kSink = 1ull << 20;
static const TegraNVJPGAbility kAll = {0x1ff, 0x1f, 0xf, 0, 1};  // everything proven, writes whole MCUs

static TegraNVJPGImage image(uint32_t w, uint32_t h, uint32_t chroma)
{
    TegraNVJPGImage img;
    memset(&img, 0, sizeof(img));
    img.width = w, img.height = h, img.chroma = chroma;
    img.components = chroma == TEGRA_NVJPG_CHROMA_MONO ? 1 : 3;
    img.mcuW = chroma == TEGRA_NVJPG_CHROMA_420 || chroma == TEGRA_NVJPG_CHROMA_422H ? 16 : 8;
    img.mcuH = chroma == TEGRA_NVJPG_CHROMA_420 || chroma == TEGRA_NVJPG_CHROMA_422V ? 16 : 8;
    img.mcusX = (w + img.mcuW - 1) / img.mcuW, img.mcusY = (h + img.mcuH - 1) / img.mcuH;
    return img;
}

static uint64_t align(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

// CoreVideo's shapes: rows padded to 64 bytes, planes one after the other, each on a 16 KiB boundary (page multiples).
static TegraNVJPGSurface surface(uint32_t fourcc, uint32_t w, uint32_t h)
{
    TegraNVJPGSurface s;
    memset(&s, 0, sizeof(s));
    s.fourcc = fourcc, s.width = w, s.height = h;
    const int f422 = fourcc == TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'),
              f444 = fourcc == TEGRA_NVJPG_FOURCC('4', '4', '4', 'f');
    if (fourcc == TEGRA_NVJPG_FOURCC('4', '2', '0', 'f') || f422 || f444) {
        s.planes = 2;
        s.planeBytesPerRow[0] = align(w, 64);
        s.planeBytesPerRow[1] = align(f444 ? 2ull * w : w, 64);
        s.planeHeight[0] = h, s.planeHeight[1] = f422 || f444 ? h : (h + 1) / 2;
        s.planeOffset[1] = align(s.planeBytesPerRow[0] * h, 16384);
        s.length = align(s.planeOffset[1] + s.planeBytesPerRow[1] * s.planeHeight[1], 16384);
    } else {
        const uint64_t bpp = fourcc == TEGRA_NVJPG_FOURCC('y', 'u', 'v', 'f') ? 2 : 4;
        s.planes = 0;
        s.planeBytesPerRow[0] = align(bpp * w, 64);
        s.planeHeight[0] = h;
        s.length = align(s.planeBytesPerRow[0] * h, 16384);
    }
    return s;
}

static TegraNVJPGDecodeRequest request(uint32_t w, uint32_t h, uint32_t sub, uint32_t scale)
{
    TegraNVJPGDecodeRequest r;
    memset(&r, 0, sizeof(r));
    r.src_id = 1, r.src_size = 1 << 20, r.dst_id = 2, r.dst_size = 1 << 24;
    r.width = w, r.height = h, r.subsampling = sub;
    r.out_width = tegra_nvjpg_shr_up(w, scale), r.out_height = tegra_nvjpg_shr_up(h, scale);
    return r;
}

static void expect(const char *name, const TegraNVJPGDecodeRequest *r, const TegraNVJPGImage *img,
                   const TegraNVJPGSurface *d, const TegraNVJPGAbility *a, uint32_t kind, const char *reason,
                   TegraNVJPGPlan *p)
{
    uint32_t got = 99;
    const char *why = tegra_nvjpg_plan(r, img, d, a, kSink, p, &got);
    CHECK(got == kind, "%s: kind %u want %u (%s)", name, got, kind, why ? why : "ok");
    CHECK(!!why == (kind != TEGRA_NVJPG_PLAN_OK), "%s: reason/kind disagree", name);
    if (reason)
        CHECK(why && strstr(why, reason), "%s: refused as '%s', want '%s'", name, why ? why : "ok", reason);
}

int main(void)
{
    TegraNVJPGPlan p;
    const uint32_t f420 = TEGRA_NVJPG_FOURCC('4', '2', '0', 'f'), bgra = TEGRA_NVJPG_FOURCC('B', 'G', 'R', 'A');

    // An iPhone photo: 4032x3024 4:2:0 into '420f'. MCU-aligned: whole-MCU writes fit exactly.
    TegraNVJPGImage img = image(4032, 3024, TEGRA_NVJPG_CHROMA_420);
    TegraNVJPGDecodeRequest r = request(4032, 3024, TEGRA_NVJPG_SUB_420, 0);
    TegraNVJPGSurface d = surface(f420, 4032, 3024);
    expect("photo 420f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_NV12 && p.planes == 2 && p.planeStart[0] == 0 &&
              p.planeStart[1] == d.planeOffset[1] && p.target.pitch[0] == 4032 && !p.fillChroma,
          "photo 420f: plan");
    CHECK(p.writeEnd == d.planeOffset[1] + 4032ull * 1511 + 4032, "photo 420f: write end %llu",
          (unsigned long long)p.writeEnd);
    // The same at 1/8 (a thumbnail), and 1/2 with the scale unproven.
    r = request(4032, 3024, TEGRA_NVJPG_SUB_420, 3);
    d = surface(bgra, 504, 378);
    expect("photo 1/8 BGRA", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.scale == 3 && p.target.format == TEGRA_NVJPG_OUT_BGRA && p.extent.width == 504 &&
              p.extent.rows[0] == 378,
          "photo 1/8 BGRA: plan");
    TegraNVJPGAbility noScale = kAll;
    noScale.scales = 1;
    expect("photo 1/8 unproven", &r, &img, &d, &noScale, TEGRA_NVJPG_PLAN_UNSUPPORTED, "scale not proven", &p);
    r.out_width = 505;  // not 4032 / 2^n rounded up
    expect("odd decode size", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "decode size", &p);

    // 1080p: the last MCU row is half outside the picture.
    img = image(1920, 1080, TEGRA_NVJPG_CHROMA_420);
    r = request(1920, 1080, TEGRA_NVJPG_SUB_420, 0);
    d = surface(bgra, 1920, 1080);
    expect("1080p BGRA padded", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.extent.rows[0] == 1088 && p.writeEnd == 7680ull * 1088 && p.writeEnd > d.length - 0 - 0,
          "1080p BGRA padded: 8 rows run past the surface into the sink (%llu, length %llu)",
          (unsigned long long)p.writeEnd, (unsigned long long)d.length);
    // CoreVideo's 16 KiB plane alignment leaves 3.7 rows after the luma: direct writes would touch chroma, so the
    // planner stages into 64B-aligned scratch and Copy Engine DMA-repacks exact picture rows.
    d = surface(f420, 1920, 1080);
    expect("1080p 420f padded", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack == TEGRA_NVJPG_REPACK_CE && p.planes == 2 && p.target.pitch[0] == 1920 &&
              p.target.pitch[1] == 1920 && p.copyRows[0] == 1080 && p.copyRows[1] == 540 && p.copyRowBytes[0] == 1920 &&
              p.copyRowBytes[1] == 1920 && p.copyDstStart[1] == d.planeOffset[1],
          "1080p 420f padded: CE repack plan");
    d.planeOffset[1] = 1920ull * 1080;  // chroma straight after the luma
    d.length = d.planeOffset[1] + 1920ull * 540;
    expect("1080p 420f packed planes", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack == TEGRA_NVJPG_REPACK_CE && p.copyDstStart[1] == 1920ull * 1080, "1080p 420f packed: CE repack");
    TegraNVJPGAbility clipped = kAll;
    clipped.clipped = 1;
    expect("1080p 420f packed, clipped engine", &r, &img, &d, &clipped, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(!p.repack && p.extent.rows[0] == 1080 && p.extent.rows[1] == 540, "clipped extent");

    d = surface(TEGRA_NVJPG_FOURCC('4', '2', '0', 'v'), 1920, 1080);
    expect("420v", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "pixel format", &p);
    d = surface(TEGRA_NVJPG_FOURCC('L', '0', '0', '8'), 1920, 1080);
    expect("L008", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "pixel format", &p);
    d = surface(TEGRA_NVJPG_FOURCC('R', 'G', 'B', 'A'), 1920, 1080);
    expect("RGBA", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_RGBA, "RGBA format");
    d = surface(0x20, 1920, 1080);
    expect("ARGB", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    TegraNVJPGAbility noArgb = kAll;
    noArgb.formats &= ~(1u << TEGRA_NVJPG_OUT_ARGB);
    expect("ARGB unproven", &r, &img, &d, &noArgb, TEGRA_NVJPG_PLAN_UNSUPPORTED, "does not write", &p);

    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_422H);
    r = request(1600, 1200, TEGRA_NVJPG_SUB_422, 0);
    d = surface(f420, 1600, 1200);
    expect("422 to 420f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    r.subsampling = TEGRA_NVJPG_SUB_420;
    expect("subsampling code wrong", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "subsampling", &p);
    TegraNVJPGAbility no422 = kAll;
    no422.chroma &= ~(1u << TEGRA_NVJPG_CHROMA_422H);
    r.subsampling = TEGRA_NVJPG_SUB_422;
    expect("422 unproven", &r, &img, &d, &no422, TEGRA_NVJPG_PLAN_UNSUPPORTED, "sampling not proven", &p);
    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_422V);
    expect("440", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "4:4:0", &p);

    // CMPhoto's native formats under the interchange formats: 4:2:2 files into 'yuvf' (or '422f'), 4:4:4 into '444f'.
    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_422H);
    r = request(1600, 1200, TEGRA_NVJPG_SUB_422, 0);
    d = surface(TEGRA_NVJPG_FOURCC('y', 'u', 'v', 'f'), 1600, 1200);
    expect("422 to yuvf", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_YUY2 && p.planes == 1 && p.extent.rowBytes[0] == 3200 &&
              p.extent.rows[0] == 1200 && p.target.pitch[0] == 3200,
          "422 to yuvf: plan");
    // '422f' and '444f': three planes at the file's own sampling into the service's buffer, rows and planes on 256
    // bytes, which VIC re-packs into the destination at the request's place (semi-planar output above 4:2:0 never
    // completed on this unit). The jobs that hung (boots 19 and 21) are among them: SpringBoard's 205x83 4:4:4 file at
    // 1/1 and image.jpeg_422/444's 1/2 thumbnails.
    d = surface(TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'), 1600, 1200);
    expect("422 to 422f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack && p.target.format == TEGRA_NVJPG_OUT_NV16 && p.planes == 3 && p.target.pitch[0] == 1792 &&
              p.target.pitch[1] == 1024 && p.target.pitch[2] == 1024 && p.extent.rows[1] == 1200 &&
              p.planeStart[0] == 0 && p.planeStart[1] == 1792ull * 1200 &&
              p.planeStart[2] == 1792ull * 1200 + 1024ull * 1200 && p.writeEnd == p.planeStart[2] + 1024ull * 1200 &&
              p.repackBytes == 4608000,
          "422 to 422f: plan (pitches %u %u, buffer %llu)", p.target.pitch[0], p.target.pitch[1],
          (unsigned long long)p.repackBytes);
    img = image(2048, 1536, TEGRA_NVJPG_CHROMA_422H);
    r = request(2048, 1536, TEGRA_NVJPG_SUB_422, 1);
    d = surface(TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'), 1024, 768);
    expect("422 thumbnail to 422f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack && p.target.scale == 1 && p.target.pitch[0] == 1024 && p.target.pitch[1] == 512 &&
              p.extent.rows[0] == 768,
          "422 thumbnail to 422f: plan");
    img = image(2048, 1536, TEGRA_NVJPG_CHROMA_444);
    r = request(2048, 1536, TEGRA_NVJPG_SUB_444, 1);
    d = surface(TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'), 1024, 768);
    expect("444 thumbnail to 444f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack && p.target.format == TEGRA_NVJPG_OUT_NV24 && p.target.pitch[1] == 1024, "444 thumbnail: plan");
    img = image(205, 83, TEGRA_NVJPG_CHROMA_444);
    r = request(205, 83, TEGRA_NVJPG_SUB_444, 0);
    d = surface(TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'), 205, 83);
    expect("SpringBoard's 205x83 444 to 444f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack && p.target.pitch[0] == 256 && p.target.pitch[1] == 256 && p.extent.rows[0] == 88 &&
              p.extent.rows[2] == 88,
          "SpringBoard's 205x83 444 to 444f: whole MCUs into the buffer");
    {
        TegraNVJPGImage four20 = image(1600, 1200, TEGRA_NVJPG_CHROMA_420);
        TegraNVJPGDecodeRequest r420 = request(1600, 1200, TEGRA_NVJPG_SUB_420, 0);
        TegraNVJPGSurface d422 = surface(TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'), 1600, 1200);
        expect("420 to 422f", &r420, &four20, &d422, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "sampled otherwise", &p);
        TegraNVJPGImage four22 = image(1600, 1200, TEGRA_NVJPG_CHROMA_422H);
        TegraNVJPGDecodeRequest odd = request(1600, 1200, TEGRA_NVJPG_SUB_422, 0);
        odd.x_offset = 1;
        d422 = surface(TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'), 1602, 1200);
        expect("odd offset into 422f", &odd, &four22, &d422, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED,
               "odd offset into 4:2:2", &p);
    }
    TegraNVJPGAbility shipped = kAll;  // VIC not up: the service drops the formats VIC re-packs
    shipped.formats &= ~(1u << TEGRA_NVJPG_OUT_NV16 | 1u << TEGRA_NVJPG_OUT_NV24);
    expect("SpringBoard's 205x83 444 to 444f, VIC down", &r, &img, &d, &shipped, TEGRA_NVJPG_PLAN_UNSUPPORTED,
           "does not write", &p);
    d = surface(TEGRA_NVJPG_FOURCC('B', 'G', 'R', 'A'), 205, 83);
    expect("SpringBoard's 205x83 444 to BGRA", &r, &img, &d, &shipped, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_422H);
    r = request(1600, 1200, TEGRA_NVJPG_SUB_422, 0);
    r.x_offset = 1;
    d = surface(TEGRA_NVJPG_FOURCC('y', 'u', 'v', 'f'), 2048, 1200);
    expect("odd 422 offset", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "odd offset into 4:2:2", &p);
    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_MONO);
    r = request(1600, 1200, TEGRA_NVJPG_SUB_GREY, 0);
    d = surface(TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'), 1600, 1200);
    expect("grey to 444f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "grey file", &p);

    // Grey: into '420f' the chroma is the CPU's; into BGRA only once proven.
    img = image(1000, 750, TEGRA_NVJPG_CHROMA_MONO);
    r = request(1000, 750, TEGRA_NVJPG_SUB_GREY, 0);
    d = surface(f420, 1000, 750);
    expect("grey 420f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_GREY && p.planes == 1 && p.fillChroma && p.fillRows == 375 &&
              p.fillRowBytes == 1000 && p.fillStart == d.planeOffset[1] && p.fillPitch == 1024,
          "grey 420f: plan");
    d = surface(bgra, 1000, 750);
    expect("grey BGRA", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    TegraNVJPGAbility noGreyRgb = kAll;
    noGreyRgb.greyRgb = 0;
    expect("grey BGRA unproven", &r, &img, &d, &noGreyRgb, TEGRA_NVJPG_PLAN_UNSUPPORTED, "grey to RGB", &p);

    // Placement: an offset that keeps 256-byte plane starts, one that does not, an odd one into 4:2:0.
    img = image(512, 512, TEGRA_NVJPG_CHROMA_420);
    r = request(512, 512, TEGRA_NVJPG_SUB_420, 0);
    d = surface(bgra, 1024, 1024);
    r.x_offset = 64, r.y_offset = 3;
    expect("offset 64,3", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.planeStart[0] == 3ull * 4096 + 256, "offset plane start %llu", (unsigned long long)p.planeStart[0]);
    r.x_offset = 8;
    expect("offset 8", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack == TEGRA_NVJPG_REPACK_CE && p.copyDstStart[0] == 3ull * 4096 + 32 && p.copyDstPitch[0] == 4096 &&
              p.copyRowBytes[0] == 2048 && p.copyRows[0] == 512,
          "offset 8: CE repack");
    r.x_offset = 1;  // 4 bytes: not 16-byte aligned for Copy Engine either
    expect("offset 1", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "256 bytes", &p);
    r.x_offset = 600;
    expect("does not fit", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "fit", &p);
    r.x_offset = 0x2000;
    expect("offset bound", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "bounds", &p);
    r.x_offset = 2, r.y_offset = 1;
    d = surface(f420, 1024, 1024);
    expect("odd 420 offset", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "odd", &p);

    // Strides and surfaces.
    r = request(512, 512, TEGRA_NVJPG_SUB_420, 0);
    d = surface(bgra, 512, 512);
    d.planeBytesPerRow[0] = 2080;  // 16-byte rows (a CPU-made buffer)
    d.length = align(2080ull * 512, 16384);
    expect("stride 2080", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.repack == TEGRA_NVJPG_REPACK_CE && p.target.pitch[0] == 2048 && p.copyDstPitch[0] == 2080 &&
              p.copyRowBytes[0] == 2048 && p.copyRows[0] == 512 && p.planeStart[0] == 0,
          "stride 2080: CE repack plan");
    d.length = 2048ull * 512;  // too short for 512 rows at stride 2080
    expect("stride 2080 short surface", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "picture runs past", &p);
    d = surface(bgra, 512, 512);
    d.planeBytesPerRow[0] = 2000;  // shorter than one 512-pixel BGRA row
    expect("stride below row", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "stride below", &p);
    d = surface(bgra, 512, 512);
    d.protection = 1;
    expect("protected", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "protected", &p);
    d = surface(f420, 512, 512);
    d.planes = 3;
    expect("plane count", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "plane count", &p);
    d = surface(bgra, 512, 512);
    d.length = 4096;  // a surface far smaller than its rows say
    expect("short surface", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "picture runs past", &p);
    // A refusal for size reports BAD, not UNSUPPORTED (the file and the request disagree).
    r.width = 511;
    d = surface(bgra, 512, 512);
    expect("request size", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "file's", &p);
    r = request(512, 512, TEGRA_NVJPG_SUB_420, 0);
    r.dst_size = 6;
    expect("dst size", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "bounds", &p);
    r = request(3, 512, TEGRA_NVJPG_SUB_420, 0);
    expect("width 3", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_BAD, "bounds", &p);

    // Selector 7's 0xda0 struct carries selector 1's request at 0x00..0x57, and the user client plans every selector
    // from that prefix: a photo asked for through either gives the same plan and the same picture setup.
    {
        static TegraNVJPGDecodeRequest2024 v2024;
        memset(&v2024, 0, sizeof(v2024));
        const TegraNVJPGDecodeRequest r1 = request(4032, 3024, TEGRA_NVJPG_SUB_420, 0);
        memcpy(&v2024, &r1, sizeof(r1));
        const TegraNVJPGImage photo = image(4032, 3024, TEGRA_NVJPG_CHROMA_420);
        const TegraNVJPGSurface out = surface(f420, 4032, 3024);
        TegraNVJPGPlan p1, p7;
        memset(&p1, 0, sizeof(p1));
        memset(&p7, 0, sizeof(p7));
        expect("selector 1 photo", &r1, &photo, &out, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p1);
        expect("selector 7 photo", &v2024.base, &photo, &out, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p7);
        CHECK(memcmp(&p1, &p7, sizeof(p1)) == 0, "selector 7's plan differs from selector 1's");
        nvjpg_dec_drv_pic_setup_s s1, s7;
        memset(&s1, 0, sizeof(s1));
        memset(&s7, 0, sizeof(s7));
        TegraNVJPGExtent e1, e7;
        tegra_nvjpg_setup_image(&photo, 1 << 20, &s1);
        tegra_nvjpg_setup_image(&photo, 1 << 20, &s7);
        CHECK(!tegra_nvjpg_setup_output(&photo, &p1.target, true, &s1, &e1) &&
                  !tegra_nvjpg_setup_output(&photo, &p7.target, true, &s7, &e7),
              "selector photo setups refused");
        CHECK(memcmp(&s1, &s7, sizeof(s1)) == 0, "selector 7's picture setup differs from selector 1's");
    }

    // BGRA destinations GM20B laid out block-linear (a sampler asked first), in the layouts its decision gives them
    // (tgm_layout_tiled_within over the surface's allocation): HW Test gpu.iosurface.cross_process's 256x256 keeps
    // NIL's 16-GOB blocks, its 260x260 fits only 1-GOB blocks. NVJPG writes its rows into the copy engine's scratch and
    // the copy engine writes the blocks: the T210's NVJPG told tile_mode 2 wrote rows (25 Nov, nvjpg256 explained=rows).
    for (uint32_t size = 256; size <= 260; size += 4) {
        TegraNVJPGImage tile = image(size, size, TEGRA_NVJPG_CHROMA_420);
        TegraNVJPGDecodeRequest rt = request(size, size, TEGRA_NVJPG_SUB_420, 0);
        TegraNVJPGSurface dt = surface(bgra, size, size);
        TegraNVJPGDestinationLayout held;
        memset(&held, 0, sizeof(held));
        held.held = held.described = true;
        struct tgm_layout *l = &held.layout;
        CHECK(tgm_layout_tiled_within(size, size, 4, dt.length, l), "%u BGRA: tiles within its surface", size);
        CHECK(l->y_log2 == (size == 256 ? 4 : 0) && l->row_bytes == (size == 256 ? 1024 : 1088),
              "%u BGRA: layout y_log2 %u row_bytes %u", size, l->y_log2, l->row_bytes);
        expect("tiled BGRA", &rt, &tile, &dt, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
        nvjpg_dec_drv_pic_setup_s st;
        memset(&st, 0, sizeof(st));
        TegraNVJPGExtent et;
        CHECK(!tegra_nvjpg_setup_output(&tile, &p.target, true, &st, &et) && st.tile_mode == 0, "%u BGRA: rows", size);
        uint32_t route = TEGRA_NVJPG_ROUTE_COUNT;
        const char *why = tegra_nvjpg_route(&rt, &dt, 256ull << 20, true, &held, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_COPY_BLOCKS && p.repack == TEGRA_NVJPG_REPACK_CE,
              "%u BGRA block linear: through the copy engine (%s)", size, why ? why : "ok");
        CHECK(st.tile_mode == TEGRA_NVJPG_TILE_PITCH && st.block_linear_height == 0 &&
                  st.output_stride_luma == (int)p.target.pitch[0] && (p.target.pitch[0] & 63) == 0 &&
                  p.target.pitch[0] >= size * 4 && p.planeStart[0] == 0,
              "%u BGRA block linear: NVJPG's scratch rows (tile_mode %d, stride %d)", size, st.tile_mode,
              st.output_stride_luma);
        CHECK(p.copyDstStart[0] == 0 && p.copyRowBytes[0] == size * 4 && p.copyRows[0] == size &&
                  tegra_nvjpg_copy_covers_blocks(&p, l),
              "%u BGRA block linear: the copy writes every block", size);
        expect("tiled BGRA", &rt, &tile, &dt, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
        TegraNVJPGDestinationLayout compressed = held;
        compressed.compressed = true;
        why = tegra_nvjpg_route(&rt, &dt, 256ull << 20, true, &compressed, &p, &st, &route);
        CHECK(why && strstr(why, "compressed"), "%u BGRA compressed: %s", size, why ? why : "written");
        expect("tiled BGRA", &rt, &tile, &dt, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
        why = tegra_nvjpg_route(&rt, &dt, 4096, true, &held, &p, &st, &route);
        CHECK(why && strstr(why, "scratch"), "%u BGRA past the copy engine's scratch: %s", size, why ? why : "written");
    }

    // HW Test's tile probe: the whole 256x256 BGRA picture into GM20B's 16-GOB blocks, the setup's tiling as asked and
    // the plan's rows a row of GOBs apart, its writes bounded for the mapping by the worst reading of the tiling (a
    // stride of 64-byte GOB columns: 256 rows of 64 KiB, and a block row of 2^8 GOBs, 2 MiB); a placed picture, block
    // rows past the destination, or a worst reading past one mapping's window, refused
    {
        const uint64_t window = 256ull << 20;
        TegraNVJPGImage tile = image(256, 256, TEGRA_NVJPG_CHROMA_420);
        TegraNVJPGDecodeRequest rt = request(256, 256, TEGRA_NVJPG_SUB_420, 0);
        TegraNVJPGSurface dt = surface(bgra, 256, 256);
        TegraNVJPGDestinationLayout held;
        memset(&held, 0, sizeof(held));
        held.held = held.described = true;
        CHECK(tgm_layout_tiled_within(256, 256, 4, dt.length, &held.layout), "probe: 256 BGRA tiles");
        nvjpg_dec_drv_pic_setup_s st;
        TegraNVJPGExtent et;
        uint32_t route = TEGRA_NVJPG_ROUTE_COUNT;
        expect("probe", &rt, &tile, &dt, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
        memset(&st, 0, sizeof(st));
        CHECK(!tegra_nvjpg_setup_output(&tile, &p.target, true, &st, &et), "probe: setup");
        const char *why = tegra_nvjpg_route_tile_probe(&rt, dt.length, window, &held, 1, 3, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_TILE_PROBE && st.tile_mode == 1 && st.block_linear_height == 3 &&
                  st.output_stride_luma == 1024 && p.target.pitch[0] == 1024 && p.planeStart[0] == 0 &&
                  p.writeEnd == 256ull * 1024 * 64 + 1024ull * 2048 && p.repack == TEGRA_NVJPG_REPACK_NONE,
              "probe: the blocks as asked (%s)", why ? why : "ok");
        why = tegra_nvjpg_route_tile_probe(&rt, dt.length - 4096, window, &held, 2, 3, &p, &st, &route);
        CHECK(why && strstr(why, "past the destination"), "probe: block rows past it: %s", why ? why : "written");
        why = tegra_nvjpg_route_tile_probe(&rt, dt.length, window, &held, 2, 4, &p, &st, &route);
        CHECK(!why, "probe: code 4, 32-GOB blocks at most, lie inside 256 rows (%s)", why ? why : "ok");
        why = tegra_nvjpg_route_tile_probe(&rt, dt.length, window, &held, 2, 5, &p, &st, &route);
        CHECK(why && strstr(why, "past the destination"), "probe: code 5 may ask 64-GOB blocks: %s",
              why ? why : "written");
        why = tegra_nvjpg_route_tile_probe(&rt, dt.length, 16ull << 20, &held, 1, 3, &p, &st, &route);
        CHECK(why && strstr(why, "worst reading"), "probe: a worst reading past the window: %s", why ? why : "written");
        TegraNVJPGDecodeRequest placed = request(64, 64, TEGRA_NVJPG_SUB_420, 0);
        placed.x_offset = 64;
        why = tegra_nvjpg_route_tile_probe(&placed, dt.length, window, &held, 2, 4, &p, &st, &route);
        CHECK(why && strstr(why, "placed"), "probe: a placed picture: %s", why ? why : "written");
    }

    // The route once GM20B has answered for the destination: its rows when it holds no block-linear layout, its blocks
    // when it does, the copy engine into its blocks when NVJPG cannot place them there. Refused, never written as rows,
    // when GM20B holds it block-linear and describes no layout, holds a compressed one, or a copy would write part of it.
    {
        const uint64_t repack = 256ull << 20;
        TegraNVJPGSurface dt = surface(bgra, 256, 256);
        TegraNVJPGDecodeRequest rt = request(256, 256, TEGRA_NVJPG_SUB_420, 0);
        TegraNVJPGImage tile = image(256, 256, TEGRA_NVJPG_CHROMA_420);
        TegraNVJPGDestinationLayout none, held;
        memset(&none, 0, sizeof(none));
        memset(&held, 0, sizeof(held));
        held.held = held.described = true;
        CHECK(tgm_layout_tiled_within(256, 256, 4, dt.length, &held.layout), "route: 256 BGRA tiles");
        nvjpg_dec_drv_pic_setup_s st;
        TegraNVJPGExtent et;
        uint32_t route = TEGRA_NVJPG_ROUTE_COUNT;
#define PLANNED(req, img)                                                                 \
    do {                                                                                  \
        expect("route", req, img, &dt, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);             \
        memset(&st, 0, sizeof(st));                                                       \
        CHECK(!tegra_nvjpg_setup_output(img, &p.target, true, &st, &et), "route: setup"); \
        route = TEGRA_NVJPG_ROUTE_COUNT;                                                  \
    } while (0)
        PLANNED(&rt, &tile);
        const char *why = tegra_nvjpg_route(&rt, &dt, repack, true, &none, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_ROWS && p.repack == TEGRA_NVJPG_REPACK_NONE && st.tile_mode == 0,
              "route: rows when GM20B holds no layout (%s)", why ? why : "ok");

        PLANNED(&rt, &tile);
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &held, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_COPY_BLOCKS && p.repack == TEGRA_NVJPG_REPACK_CE &&
                  st.tile_mode == TEGRA_NVJPG_TILE_PITCH && p.copyDstStart[0] == 0 && p.copyRowBytes[0] == 1024 &&
                  p.copyRows[0] == 256,
              "route: blocks through the copy engine (%s, tile_mode %d)", why ? why : "ok", st.tile_mode);

        TegraNVJPGDestinationLayout undescribed = held;
        undescribed.described = false;
        PLANNED(&rt, &tile);
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &undescribed, &p, &st, &route);
        CHECK(why && strstr(why, "describes no layout") && st.tile_mode == 0, "route: undescribed: %s",
              why ? why : "written");

        TegraNVJPGDestinationLayout compressed = held;
        compressed.compressed = true;
        PLANNED(&rt, &tile);
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &compressed, &p, &st, &route);
        CHECK(why && strstr(why, "compressed"), "route: compressed: %s", why ? why : "written");

        PLANNED(&rt, &tile);
        why = tegra_nvjpg_route(&rt, &dt, repack, false, &held, &p, &st, &route);
        CHECK(why && strstr(why, "cannot reach"), "route: no copy engine: %s", why ? why : "written");

        TegraNVJPGDecodeRequest placed = request(64, 64, TEGRA_NVJPG_SUB_420, 0);
        TegraNVJPGImage small = image(64, 64, TEGRA_NVJPG_CHROMA_420);
        placed.x_offset = 64, placed.y_offset = 128;
        PLANNED(&placed, &small);
        why = tegra_nvjpg_route(&placed, &dt, repack, true, &held, &p, &st, &route);
        CHECK(why && strstr(why, "not the whole"), "route: a picture placed inside the blocks: %s",
              why ? why : "written");

        PLANNED(&rt, &tile);
        p.repack = TEGRA_NVJPG_REPACK_CE;
        p.copyDstStart[0] = 0, p.copyRowBytes[0] = 1024, p.copyRows[0] = 256;
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &held, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_COPY_BLOCKS, "route: a staged copy of the whole surface: %s",
              why ? why : "ok");
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &none, &p, &st, &route);
        CHECK(!why && route == TEGRA_NVJPG_ROUTE_COPY_ROWS, "route: a staged copy into rows: %s", why ? why : "ok");
        p.copyDstStart[0] = 64;
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &held, &p, &st, &route);
        CHECK(why && strstr(why, "part of a block-linear"), "route: a staged copy into part: %s",
              why ? why : "written");
        p.repack = TEGRA_NVJPG_REPACK_VIC;
        why = tegra_nvjpg_route(&rt, &dt, repack, true, &none, &p, &st, &route);
        CHECK(why != NULL, "route: VIC's re-pack is not routed");
#undef PLANNED
    }

    if (failures) {
        printf("FAIL request: %d failures\n", failures);
        return 1;
    }
    printf("PASS request\n");
    return 0;
}
