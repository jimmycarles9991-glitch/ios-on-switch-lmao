// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of TegraNVJPGRequest.h (test:request_host_test): AppleJPEGDriver decode requests, as CMPhoto and
// JPEGH1.videodecoder build them, translated into NVJPG plans against made-up destination surfaces shaped as
// CoreVideo lays them out; every refusal, with its kind (UNSUPPORTED: the caller decodes on the CPU; BAD: the request
// contradicts the file or the surfaces).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../TegraNVJPGRequest.h"

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
    // CoreVideo's 16 KiB plane alignment leaves 3.7 rows after the luma: 8 padded rows would land in the chroma.
    d = surface(f420, 1920, 1080);
    expect("1080p 420f padded", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "another plane", &p);
    d.planeOffset[1] = 1920ull * 1080;  // chroma straight after the luma
    d.length = d.planeOffset[1] + 1920ull * 540;
    expect("1080p 420f packed planes", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "another plane", &p);
    TegraNVJPGAbility clipped = kAll;
    clipped.clipped = 1;
    expect("1080p 420f packed, clipped engine", &r, &img, &d, &clipped, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.extent.rows[0] == 1080 && p.extent.rows[1] == 540, "clipped extent");

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
    expect("ARGB unproven", &r, &img, &d, &noArgb, TEGRA_NVJPG_PLAN_UNSUPPORTED, "format not proven", &p);

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
    d = surface(TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'), 1600, 1200);
    expect("422 to 422f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_NV16 && p.planes == 2 && p.extent.rows[1] == 1200 &&
              p.extent.rowBytes[1] == 1600 && p.planeStart[1] == d.planeOffset[1],
          "422 to 422f: plan");
    img = image(1600, 1200, TEGRA_NVJPG_CHROMA_444);
    r = request(1600, 1200, TEGRA_NVJPG_SUB_444, 0);
    d = surface(TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'), 1600, 1200);
    expect("444 to 444f", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.target.format == TEGRA_NVJPG_OUT_NV24 && p.planes == 2 && p.extent.rows[1] == 1200 &&
              p.extent.rowBytes[1] == 3200 && p.target.pitch[1] == 3200,
          "444 to 444f: plan");
    r.x_offset = 256, r.y_offset = 2;
    d = surface(TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'), 2048, 1600);
    expect("444f offset", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_OK, NULL, &p);
    CHECK(p.planeStart[0] == 2 * 2048 + 256 && p.planeStart[1] == d.planeOffset[1] + 2 * 4096 + 512,
          "444f offset: starts");
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
    expect("offset 8", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "256 bytes", &p);
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
    expect("stride 2080", &r, &img, &d, &kAll, TEGRA_NVJPG_PLAN_UNSUPPORTED, "multiple of 64", &p);
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

    if (failures) {
        printf("FAIL request: %d failures\n", failures);
        return 1;
    }
    printf("PASS request\n");
    return 0;
}
