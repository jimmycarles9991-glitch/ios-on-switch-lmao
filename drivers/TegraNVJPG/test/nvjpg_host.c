// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of TegraNVJPG's JPEG side (test:nvjpg_host_test runs it):
// - TegraNVJPGJpeg.h's parser against libjpeg-turbo's own parse of the same files, over a matrix of files libjpeg-turbo
//   writes here (sizes, 4:2:0/4:2:2/4:4:0/4:4:4/grey, restart intervals, quality, optimised tables): dimensions,
//   sampling, every quantisation and Huffman table, the scan's table choices, the restart interval, where the scan
//   starts and that it ends at EOI; then the decoder setup made from it, field by field;
// - every refusal: progressive, arithmetic, lossless-style markers, 12-bit, 16-bit tables, CMYK, Adobe RGB, 4:1:1,
//   subsampled chroma, grey above 1x1, non-interleaved and multiple scans, DNL, oversize, undefined and corrupt tables,
//   truncated headers; a truncated scan is accepted without EOI;
// - the target translation: strides, plane extents (padded and clipped), formats, the JFIF matrix against the float
//   formula;
// - mutation fuzzing of the parser under ASan/UBSan.
// Without libjpeg-turbo (no HAVE_LIBJPEG) only the checks that need no encoder run.
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../TegraNVJPGJpeg.h"
#ifdef HAVE_LIBJPEG
#include <jpeglib.h>
#define STR2(x)                      #x
#define STR(x)                       STR2(x)
#define LIBJPEG_TURBO_VERSION_STRING STR(LIBJPEG_TURBO_VERSION)
#endif

// The JFIF conversion in integer arithmetic with the coefficients the jobs program (kTegraNVJPGJfif).
static uint8_t clamp8(int32_t v)
{
    if (v < 0)
        return 0;
    return (uint8_t)(v > 255 ? 255 : v);
}

static void jfif_rgb(uint32_t y, uint32_t cb, uint32_t cr, uint8_t rgb[3])
{
    const int32_t Y = (int32_t)y * kTegraNVJPGJfif[0];
    const int32_t u = (int32_t)cb - 128;
    const int32_t v = (int32_t)cr - 128;
    rgb[0] = clamp8((Y + kTegraNVJPGJfif[1] * v + 32768) / 65536);
    rgb[1] = clamp8((Y + kTegraNVJPGJfif[2] * u + kTegraNVJPGJfif[3] * v + 32768) / 65536);
    rgb[2] = clamp8((Y + kTegraNVJPGJfif[4] * u + 32768) / 65536);
}

static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond))           \
            fail(__VA_ARGS__); \
    } while (0)

// The offset of the first marker `m` (its 0xFF), or -1.
static long find_marker(const uint8_t *d, size_t n, uint8_t m)
{
    for (size_t i = 2; i + 3 < n;) {
        if (d[i] != 0xff)
            return -1;
        if (d[i + 1] == m)
            return (long)i;
        if (d[i + 1] == 0xda)
            return -1;
        i += 2 + tegra_nvjpg_be16(d + i + 2);
    }
    return -1;
}

static const char *parse_copy(const uint8_t *d, size_t n, TegraNVJPGImage *img)
{
    // An exact-size heap copy, so ASan sees any read past the end.
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, d, n);
    const char *why = tegra_nvjpg_parse(copy, n, img);
    free(copy);
    return why;
}

static void expect_refusal(const char *name, const uint8_t *d, size_t n, const char *reason)
{
    TegraNVJPGImage img;
    const char *why = parse_copy(d, n, &img);
    if (!why)
        fail("%s: accepted", name);
    else if (reason && !strstr(why, reason))
        fail("%s: refused as '%s', want '%s'", name, why, reason);
}

// Edits of a valid baseline file that must each be refused (or, for a truncated scan, accepted without EOI).
static void edits(const char *name, const uint8_t *orig, size_t n)
{
    uint8_t *d = malloc(n + 64);
    char label[160];
    TegraNVJPGImage img;
    const long sof = find_marker(orig, n, 0xc0), dqt = find_marker(orig, n, 0xdb), dht = find_marker(orig, n, 0xc4);
    CHECK(sof > 0 && dqt > 0 && dht > 0, "%s: markers", name);
    if (sof <= 0 || dqt <= 0 || dht <= 0) {
        free(d);
        return;
    }

    static const struct {
        uint8_t marker;
        const char *why;
    } sofs[] = {{0xc2, "progressive"}, {0xc3, "lossless"}, {0xc5, "hierarchical"}, {0xc9, "arithmetic"},
                {0xca, "progressive"}, {0xcb, "lossless"}, {0xf7, "JPEG-LS"}};

    for (size_t i = 0; i < sizeof(sofs) / sizeof(sofs[0]); i++) {
        memcpy(d, orig, n);
        d[sof + 1] = sofs[i].marker;
        snprintf(label, sizeof(label), "%s SOF %#x", name, sofs[i].marker);
        expect_refusal(label, d, n, sofs[i].why);
    }
    memcpy(d, orig, n);
    d[sof + 4] = 12;
    snprintf(label, sizeof(label), "%s 12-bit", name);
    expect_refusal(label, d, n, "8-bit");
    memcpy(d, orig, n);
    d[sof + 5] = d[sof + 6] = 0;
    snprintf(label, sizeof(label), "%s DNL", name);
    expect_refusal(label, d, n, "DNL");
    memcpy(d, orig, n);
    d[sof + 7] = 0x40, d[sof + 8] = 0x01;  // width 16385
    snprintf(label, sizeof(label), "%s 16385 wide", name);
    expect_refusal(label, d, n, "16384");
    memcpy(d, orig, n);
    d[dqt + 4] |= 0x10;
    snprintf(label, sizeof(label), "%s 16-bit DQT", name);
    expect_refusal(label, d, n, "16-bit");
    memcpy(d, orig, n);
    d[dqt + 5] = 0;
    snprintf(label, sizeof(label), "%s quant 0", name);
    expect_refusal(label, d, n, "quantisation value 0");
    memcpy(d, orig, n);
    d[dqt + 4] = (uint8_t)((d[dqt + 4] & 0xf0) | 2);  // table 0 becomes table 2: table 0 undefined
    snprintf(label, sizeof(label), "%s undefined quant", name);
    expect_refusal(label, d, n, "undefined");
    // Three 1-bit codes (only two exist), the table's symbol count kept: moved from its longest codes.
    memcpy(d, orig, n);
    {
        uint8_t *bits = d + dht + 5;
        int last = 15;
        while (last > 0 && !bits[last])
            last--;
        const int move = 3 - bits[0];
        if (move > 0 && last > 0 && bits[last] >= move) {
            bits[0] += (uint8_t)move, bits[last] -= (uint8_t)move;
            snprintf(label, sizeof(label), "%s bad Huffman", name);
            expect_refusal(label, d, n, "do not fit");
        }
    }
    memcpy(d, orig, n);
    d[dht + 4] = 0x21;
    snprintf(label, sizeof(label), "%s Huffman class 2", name);
    expect_refusal(label, d, n, "class");
    // A second SOS straight after the first scan's data instead of EOI: more than one scan.
    TegraNVJPGImage ok;
    if (!parse_copy(orig, n, &ok) && ok.eoi) {
        const long sos = (long)ok.scanOffset - 2 - (long)tegra_nvjpg_be16(orig + ok.scanOffset - 0) * 0;
        (void)sos;
        memcpy(d, orig, n);
        d[ok.scanEnd - 1] = 0xda;
        snprintf(label, sizeof(label), "%s second scan", name);
        expect_refusal(label, d, n, "more than one scan");
        memcpy(d, orig, n);
        d[ok.scanEnd - 1] = 0xdc;
        snprintf(label, sizeof(label), "%s DNL after scan", name);
        expect_refusal(label, d, n, "DNL");
        // Truncated inside the scan: accepted, no EOI, the scan runs to the end of the data.
        const size_t cut = ok.scanOffset + (ok.scanEnd - ok.scanOffset) / 2;
        const char *why = parse_copy(orig, cut, &img);
        snprintf(label, sizeof(label), "%s truncated scan", name);
        CHECK(!why && !img.eoi && img.scanEnd == cut, "%s: %s eoi %u end %u", label, why ? why : "ok", img.eoi,
              img.scanEnd);
        // Truncated anywhere in the headers: refused, never read past the end (ASan).
        for (size_t at = 0; at < ok.scanOffset; at++) {
            TegraNVJPGImage t;
            CHECK(parse_copy(orig, at, &t) != NULL, "%s: headers cut at %zu accepted", name, at);
        }
    }
    free(d);
}

#ifdef HAVE_LIBJPEG  // only the libjpeg-turbo matrix makes the files it strips
// A file without DHT (Motion JPEG style) gets Annex K's tables: the same ones libjpeg-turbo wrote into the original.
static void without_dht(const char *name, const uint8_t *orig, size_t n)
{
    TegraNVJPGImage full, bare;
    if (parse_copy(orig, n, &full))
        return;
    uint8_t *d = malloc(n);
    size_t m = 0;
    for (size_t i = 0; i < 2; i++)
        d[m++] = orig[i];
    for (size_t at = 2; at < full.scanOffset;) {
        const size_t seg = 2 + tegra_nvjpg_be16(orig + at + 2);
        if (orig[at + 1] != 0xc4)
            memcpy(d + m, orig + at, seg), m += seg;
        at += seg;
    }
    memcpy(d + m, orig + full.scanOffset, n - full.scanOffset);
    m += n - full.scanOffset;
    const char *why = parse_copy(d, m, &bare);
    CHECK(!why, "%s without DHT: %s", name, why ? why : "");
    if (!why) {
        CHECK(bare.stdTables == (full.components == 1 ? 0x11u : 0x33u), "%s without DHT: std tables %#x", name,
              bare.stdTables);
        CHECK(!memcmp(bare.bits, full.bits, sizeof(full.bits)) && !memcmp(bare.vals, full.vals, sizeof(full.vals)) &&
                  !memcmp(bare.count, full.count, sizeof(full.count)),
              "%s without DHT: Annex K tables differ from libjpeg-turbo's", name);
    }
    free(d);
}

#endif  // HAVE_LIBJPEG

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint32_t next_random(void)
{
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)rng;
}

static void fuzz(const uint8_t *orig, size_t n, uint32_t rounds)
{
    uint8_t *d = malloc(n);
    for (uint32_t r = 0; r < rounds; r++) {
        memcpy(d, orig, n);
        const uint32_t edits = 1 + next_random() % 8;
        for (uint32_t e = 0; e < edits; e++) {
            const size_t at = next_random() % (n < 700 ? n : 700);  // mostly the headers
            const uint32_t kind = next_random() % 4;
            d[at] = kind == 0 ? 0xff : kind == 1 ? (uint8_t)next_random() : kind == 2 ? 0 : (uint8_t)(d[at] ^ 1);
        }
        const size_t len = next_random() % 4 ? n : next_random() % n;
        TegraNVJPGImage img;
        if (parse_copy(d, len, &img))
            continue;
        // Whatever it accepts is self-consistent.
        CHECK(img.width && img.height && img.width <= 16384 && img.height <= 16384, "fuzz: size %ux%u", img.width,
              img.height);
        CHECK(img.components == 1 || img.components == 3, "fuzz: components");
        CHECK(img.scanOffset <= img.scanEnd && img.scanEnd <= len, "fuzz: scan %u..%u of %zu", img.scanOffset,
              img.scanEnd, len);
        CHECK(img.mcusX * img.mcuW >= img.width && img.mcusY * img.mcuH >= img.height, "fuzz: MCU grid");
        for (uint32_t i = 0; i < img.components; i++)
            CHECK((img.quantMask >> img.comp[i].tq & 1) && (img.huffMask[0] >> img.comp[i].td & 1) &&
                      (img.huffMask[1] >> img.comp[i].ta & 1),
                  "fuzz: undefined table used");
    }
    free(d);
}

static void targets(void)
{
    TegraNVJPGImage img;
    memset(&img, 0, sizeof(img));
    img.width = 100, img.height = 75, img.components = 3, img.chroma = TEGRA_NVJPG_CHROMA_420;
    img.mcuW = img.mcuH = 16, img.mcusX = 7, img.mcusY = 5;
    nvjpg_dec_drv_pic_setup_s s;
    TegraNVJPGExtent e;
    TegraNVJPGTarget t = {TEGRA_NVJPG_OUT_NV12, 0, {128, 128, 0}, 0};
    memset(&s, 0, sizeof(s));
    CHECK(!tegra_nvjpg_setup_output(&img, &t, 1, &s, &e), "NV12 refused");
    CHECK(e.width == 100 && e.height == 75 && e.planes == 2, "NV12 size");
    CHECK(e.rows[0] == 80 && e.rowBytes[0] == 128 && e.rows[1] == 40 && e.rowBytes[1] == 128, "NV12 padded extent");
    CHECK(s.output_pixel_format == 0 && s.output_chroma_mode == 1 && s.memory_mode == 0 && s.output_stride_luma == 128,
          "NV12 setup");
    CHECK(!tegra_nvjpg_setup_output(&img, &t, 0, &s, &e), "NV12 clipped refused");
    CHECK(e.rows[0] == 75 && e.rowBytes[0] == 128 && e.rows[1] == 38 && e.rowBytes[1] == 128, "NV12 clipped extent");
    t.pitch[0] = 96;
    CHECK(tegra_nvjpg_setup_output(&img, &t, 1, &s, &e) != NULL, "stride below a padded row accepted");
    t.pitch[0] = 120;
    CHECK(tegra_nvjpg_setup_output(&img, &t, 0, &s, &e) != NULL, "stride not a multiple of 64 accepted");
    t = (TegraNVJPGTarget){TEGRA_NVJPG_OUT_BGRA, 0, {448, 0, 0}, 0xff};
    CHECK(!tegra_nvjpg_setup_output(&img, &t, 1, &s, &e), "BGRA refused");
    CHECK(e.planes == 1 && e.rows[0] == 80 && e.rowBytes[0] == 448, "BGRA extent");  // 448: already 64-byte bursts
    CHECK(s.output_pixel_format == 4 && s.memory_mode == 3 && s.output_chroma_mode == 1 && s.alpha_value == 255,
          "BGRA setup");
    t.scale = 3;
    CHECK(!tegra_nvjpg_setup_output(&img, &t, 1, &s, &e), "BGRA 1/8 refused");
    CHECK(e.width == 13 && e.height == 10 && e.rows[0] == 10 && e.rowBytes[0] == 64 && s.power2_downscale == 3,
          "BGRA 1/8 extent %ux%u %u %u", e.width, e.height, e.rows[0], e.rowBytes[0]);
    t.scale = 4;
    CHECK(tegra_nvjpg_setup_output(&img, &t, 1, &s, &e) != NULL, "scale 4 accepted");
    t = (TegraNVJPGTarget){TEGRA_NVJPG_OUT_GREY, 0, {128, 0, 0}, 0};
    CHECK(tegra_nvjpg_setup_output(&img, &t, 1, &s, &e) != NULL, "colour to grey accepted");
    {
        TegraNVJPGImage grey = img;
        grey.components = 1, grey.chroma = TEGRA_NVJPG_CHROMA_MONO, grey.mcuW = grey.mcuH = 8;
        grey.mcusX = 13, grey.mcusY = 10;
        TegraNVJPGTarget g = {TEGRA_NVJPG_OUT_NV12, 0, {128, 128, 0}, 0};
        CHECK(tegra_nvjpg_setup_output(&grey, &g, 1, &s, &e) != NULL, "grey to NV12 accepted");
        g = (TegraNVJPGTarget){TEGRA_NVJPG_OUT_BGRA, 0, {448, 0, 0}, 0xff};
        CHECK(!tegra_nvjpg_setup_output(&grey, &g, 1, &s, &e) && s.output_chroma_mode == 0 && e.rows[0] == 80,
              "grey to BGRA refused");
        g = (TegraNVJPGTarget){TEGRA_NVJPG_OUT_GREY, 0, {128, 0, 0}, 0};
        CHECK(!tegra_nvjpg_setup_output(&grey, &g, 1, &s, &e) && s.memory_mode == 3 && e.planes == 1 &&
                  e.rowBytes[0] == 128,
              "grey to grey refused");
    }
    t = (TegraNVJPGTarget){TEGRA_NVJPG_OUT_I420, 0, {128, 64, 128}, 0};
    CHECK(tegra_nvjpg_setup_output(&img, &t, 1, &s, &e) != NULL, "I420 with different chroma strides accepted");
    // CMPhoto's interchange-format destinations ('422f', '444f', 'yuvf': 24B119 and 21H461 alike since the client's
    // format filter went): NVJPG writes them itself, semi-planar or packed, at 4:2:2 or 4:4:4.
    {
        TegraNVJPGImage four22 = img, four44 = img;
        four22.chroma = TEGRA_NVJPG_CHROMA_422H, four22.mcuW = 16, four22.mcuH = 8, four22.mcusY = 10;
        four44.chroma = TEGRA_NVJPG_CHROMA_444, four44.mcuW = four44.mcuH = 8, four44.mcusX = 13, four44.mcusY = 10;
        TegraNVJPGTarget n16 = {TEGRA_NVJPG_OUT_NV16, 0, {128, 128, 0}, 0};
        CHECK(!tegra_nvjpg_setup_output(&four22, &n16, 1, &s, &e), "422 to 422f refused");
        CHECK(s.output_chroma_mode == TEGRA_NVJPG_CHROMA_422H && s.memory_mode == TEGRA_NVJPG_MEMORY_NV12 &&
                  e.planes == 2 && e.rows[0] == 80 && e.rows[1] == 80 && e.rowBytes[1] == 128,
              "422f setup: chroma rows at full height");
        TegraNVJPGTarget yuy2 = {TEGRA_NVJPG_OUT_YUY2, 0, {256, 0, 0}, 0};
        CHECK(!tegra_nvjpg_setup_output(&four22, &yuy2, 1, &s, &e), "422 to yuvf refused");
        CHECK(s.output_chroma_mode == TEGRA_NVJPG_CHROMA_422H && s.memory_mode == TEGRA_NVJPG_MEMORY_YUY2 &&
                  e.planes == 1 && e.rowBytes[0] == 256,
              "yuvf setup: one packed plane, two bytes a sample");
        TegraNVJPGTarget n24 = {TEGRA_NVJPG_OUT_NV24, 0, {128, 256, 0}, 0};
        CHECK(!tegra_nvjpg_setup_output(&four44, &n24, 1, &s, &e), "444 to 444f refused");
        CHECK(s.output_chroma_mode == TEGRA_NVJPG_CHROMA_444 && s.memory_mode == TEGRA_NVJPG_MEMORY_NV12 &&
                  e.planes == 2 && e.rows[1] == 80 && e.rowBytes[1] == 256,
              "444f setup: chroma rows twice the luma's bytes");
        n24.pitch[1] = 128;
        CHECK(tegra_nvjpg_setup_output(&four44, &n24, 1, &s, &e) != NULL, "444f chroma stride below its row accepted");
    }
    // The JFIF matrix in 16.16 against the float formula over the whole cube (corners and a lattice).
    int worst = 0;
    for (int y = 0; y < 256; y += 5)
        for (int cb = 0; cb < 256; cb += 3)
            for (int cr = 0; cr < 256; cr += 7) {
                uint8_t rgb[3];
                jfif_rgb((uint32_t)y, (uint32_t)cb, (uint32_t)cr, rgb);
                const double f[3] = {y + 1.402 * (cr - 128), y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128),
                                     y + 1.772 * (cb - 128)};
                for (int c = 0; c < 3; c++) {
                    const double clamped = f[c] < 0 ? 0 : f[c] > 255 ? 255 : f[c];
                    const int d = abs((int)rgb[c] - (int)lround(clamped));
                    if (d > worst)
                        worst = d;
                }
            }
    CHECK(worst <= 1, "JFIF matrix off by %d", worst);
}

#ifdef HAVE_LIBJPEG
static const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} Error;

static void error_exit(j_common_ptr c)
{
    Error *e = (Error *)c->err;
    c->err->format_message(c, e->message);
    longjmp(e->jump, 1);
}

static void quiet(j_common_ptr c, int level)
{
    (void)c, (void)level;
}

enum {
    CS_YCC,
    CS_GREY,
    CS_RGB,
    CS_CMYK
};

typedef struct {
    const char *name;
    uint32_t w, h, cs;
    uint8_t hs, vs;    // luma sampling (chroma 1x1), or every component's when all is set
    uint8_t all;       // every component sampled hs x vs
    uint32_t restart;  // MCUs
    uint32_t rows;     // restart_in_rows
    int quality, optimize, progressive, arith, noninterleaved, baseline;
} Spec;

static uint8_t sample(uint32_t c, uint32_t x, uint32_t y)
{
    double v;
    if (c == 0)
        v = 64 + 2.0 * (x % 64) + 1.5 * (y % 64) + 24 * sin(0.9 * x) * cos(0.7 * y);
    else if (c == 1)
        v = 96 + 2.0 * (x % 64);
    else if (c == 2)
        v = 160 - 2.0 * (y % 64);
    else
        v = 40 + (x * 3 + y) % 170;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint8_t *encode(const Spec *sp, unsigned long *size)
{
    struct jpeg_compress_struct c;
    Error err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = error_exit;
    err.mgr.emit_message = quiet;
    uint8_t *out = NULL;
    unsigned long outSize = 0;
    uint8_t *row = NULL;
    if (setjmp(err.jump)) {
        fail("%s: encode: %s", sp->name, err.message);
        jpeg_destroy_compress(&c);
        free(row);
        free(out);
        return NULL;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &outSize);
    const int comps = sp->cs == CS_GREY ? 1 : sp->cs == CS_CMYK ? 4 : 3;
    c.image_width = sp->w, c.image_height = sp->h, c.input_components = comps;
    c.in_color_space = sp->cs == CS_GREY   ? JCS_GRAYSCALE
                       : sp->cs == CS_CMYK ? JCS_CMYK
                       : sp->cs == CS_RGB  ? JCS_RGB
                                           : JCS_YCbCr;
    jpeg_set_defaults(&c);
    if (sp->cs == CS_RGB)
        jpeg_set_colorspace(&c, JCS_RGB);
    jpeg_set_quality(&c, sp->quality, sp->baseline);
    for (int i = 0; i < c.num_components; i++) {
        const int luma = i == 0 || sp->all;
        c.comp_info[i].h_samp_factor = luma ? sp->hs : 1;
        c.comp_info[i].v_samp_factor = luma ? sp->vs : 1;
    }
    c.restart_interval = sp->restart;
    c.restart_in_rows = (int)sp->rows;
    c.optimize_coding = sp->optimize;
    c.arith_code = sp->arith;
    c.dct_method = JDCT_ISLOW;
    if (sp->progressive)
        jpeg_simple_progression(&c);
    static jpeg_scan_info scans[4];
    if (sp->noninterleaved) {
        for (int i = 0; i < comps; i++) {
            scans[i].comps_in_scan = 1, scans[i].component_index[0] = i;
            scans[i].Ss = 0, scans[i].Se = 63, scans[i].Ah = 0, scans[i].Al = 0;
        }
        c.scan_info = scans, c.num_scans = comps;
    }
    jpeg_start_compress(&c, TRUE);
    row = malloc((size_t)sp->w * comps);
    for (uint32_t y = 0; y < sp->h; y++) {
        for (uint32_t x = 0; x < sp->w; x++)
            for (int k = 0; k < comps; k++)
                row[x * comps + k] = sample(sp->cs == CS_GREY ? 0 : (uint32_t)k, x, y);
        JSAMPROW rows[1] = {row};
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    *size = outSize;
    return out;
}

// libjpeg-turbo's parse against ours, and the setup made from ours.
static void compare(const Spec *sp, const uint8_t *d, size_t n)
{
    TegraNVJPGImage img;
    const char *why = parse_copy(d, n, &img);
    if (why) {
        fail("%s: refused: %s", sp->name, why);
        return;
    }
    struct jpeg_decompress_struct c;
    Error err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = error_exit;
    err.mgr.emit_message = quiet;
    if (setjmp(err.jump)) {
        fail("%s: libjpeg: %s", sp->name, err.message);
        jpeg_destroy_decompress(&c);
        return;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, d, n);
    jpeg_read_header(&c, TRUE);
    CHECK(img.width == c.image_width && img.height == c.image_height, "%s: size", sp->name);
    CHECK((int)img.components == c.num_components, "%s: components", sp->name);
    CHECK(img.restart == c.restart_interval, "%s: restart %u vs %u", sp->name, img.restart, c.restart_interval);
    CHECK(img.scanOffset == n - c.src->bytes_in_buffer, "%s: scan at %u, libjpeg %zu", sp->name, img.scanOffset,
          n - c.src->bytes_in_buffer);
    CHECK(img.eoi && img.scanEnd == n, "%s: scan end %u of %zu eoi %u", sp->name, img.scanEnd, n, img.eoi);
    for (int i = 0; i < c.num_components; i++) {
        const jpeg_component_info *ci = &c.comp_info[i];
        const TegraNVJPGComponent *m = &img.comp[i];
        CHECK(m->id == ci->component_id && m->h == ci->h_samp_factor && m->v == ci->v_samp_factor &&
                  m->tq == ci->quant_tbl_no && m->td == ci->dc_tbl_no && m->ta == ci->ac_tbl_no,
              "%s: component %d", sp->name, i);
    }
    for (int t = 0; t < 4; t++) {
        const JQUANT_TBL *q = c.quant_tbl_ptrs[t];
        CHECK(!!q == !!(img.quantMask >> t & 1), "%s: quant %d presence", sp->name, t);
        if (q)
            for (int k = 0; k < 64; k++)
                CHECK(img.quant[t][k] == q->quantval[kZigzag[k]], "%s: quant %d[%d]", sp->name, t, k);
        for (int tc = 0; tc < 2; tc++) {
            const JHUFF_TBL *h = tc ? c.ac_huff_tbl_ptrs[t] : c.dc_huff_tbl_ptrs[t];
            CHECK(!!h == !!(img.huffMask[tc] >> t & 1), "%s: huff %d/%d presence", sp->name, tc, t);
            if (!h)
                continue;
            uint32_t count = 0;
            for (int l = 0; l < 16; l++) {
                CHECK(img.bits[tc][t][l] == h->bits[l + 1], "%s: huff %d/%d bits %d", sp->name, tc, t, l);
                count += h->bits[l + 1];
            }
            CHECK(count == img.count[tc][t], "%s: huff count", sp->name);
            for (uint32_t k = 0; k < count && k < 162; k++)
                CHECK(img.vals[tc][t][k] == h->huffval[k], "%s: huff %d/%d val %u", sp->name, tc, t, k);
        }
    }
    // The setup (image half).
    nvjpg_dec_drv_pic_setup_s s;
    tegra_nvjpg_setup_image(&img, img.scanEnd - img.scanOffset, &s);
    CHECK(s.frame_width == (int)c.image_width && s.frame_height == (int)c.image_height && s.comp == c.num_components,
          "%s: setup size", sp->name);
    CHECK(s.mcu_width == (int)img.mcusX && s.mcu_height == (int)img.mcusY && s.restart_interval == (int)img.restart,
          "%s: setup MCUs", sp->name);
    const int maxH = c.max_h_samp_factor, maxV = c.max_v_samp_factor;
    CHECK(img.mcusX == (c.image_width + 8 * maxH - 1) / (8 * maxH) &&
              img.mcusY == (c.image_height + 8 * maxV - 1) / (8 * maxV),
          "%s: MCU grid", sp->name);
    const int chroma = c.num_components == 1    ? 0
                       : maxH == 2 && maxV == 2 ? 1
                       : maxH == 2 && maxV == 1 ? 2
                       : maxH == 1 && maxV == 2 ? 3
                                                : 4;
    CHECK(s.stream_chroma_mode == chroma, "%s: chroma mode %d vs %d", sp->name, s.stream_chroma_mode, chroma);
    CHECK(s.bitstream_offset == 0 && s.bitstream_size == (int)(n - img.scanOffset), "%s: bitstream", sp->name);
    for (int i = 0; i < c.num_components; i++) {
        const jpeg_component_info *ci = &c.comp_info[i];
        CHECK(s.blkPar[i].hblock == ci->h_samp_factor && s.blkPar[i].vblock == ci->v_samp_factor &&
                  s.blkPar[i].quant == ci->quant_tbl_no && s.blkPar[i].dc == ci->dc_tbl_no &&
                  s.blkPar[i].ac == ci->ac_tbl_no,
              "%s: blkPar %d", sp->name, i);
    }
    for (int tc = 0; tc < 2; tc++)
        for (int t = 0; t < 4; t++) {
            const JHUFF_TBL *h = tc ? c.ac_huff_tbl_ptrs[t] : c.dc_huff_tbl_ptrs[t];
            if (!h)
                continue;
            for (int l = 0; l < 16; l++)
                CHECK(s.huffTab[tc][t].codeNum[l] == h->bits[l + 1], "%s: setup huff %d/%d", sp->name, tc, t);
            CHECK(!memcmp(s.huffTab[tc][t].symbol, img.vals[tc][t], 162), "%s: setup symbols", sp->name);
            for (int l = 0; l < 16; l++)
                CHECK(!s.huffTab[tc][t].minCode[l] && !s.huffTab[tc][t].minCodeIdx[l], "%s: min codes", sp->name);
        }
    for (int t = 0; t < 4; t++)
        if (c.quant_tbl_ptrs[t])
            for (int k = 0; k < 64; k++)
                CHECK(s.quant[t][k] == c.quant_tbl_ptrs[t]->quantval[kZigzag[k]], "%s: setup quant", sp->name);
    jpeg_destroy_decompress(&c);
}

static const Spec kAccepted[] = {
    {"420 640x480 q75", 640, 480, CS_YCC, 2, 2, 0, 0, 0, 75, 0, 0, 0, 0, 1},
    {"420 1x1", 1, 1, CS_YCC, 2, 2, 0, 0, 0, 90, 0, 0, 0, 0, 1},
    {"420 17x33 restart 1", 17, 33, CS_YCC, 2, 2, 0, 1, 0, 90, 0, 0, 0, 0, 1},
    {"420 101x77 restart 7 optimised", 101, 77, CS_YCC, 2, 2, 0, 7, 0, 60, 1, 0, 0, 0, 1},
    {"420 2000x3 restart rows", 2000, 3, CS_YCC, 2, 2, 0, 0, 1, 85, 0, 0, 0, 0, 1},
    {"420 3x1500", 3, 1500, CS_YCC, 2, 2, 0, 0, 0, 85, 1, 0, 0, 0, 1},
    {"420 16384x16", 16384, 16, CS_YCC, 2, 2, 0, 0, 0, 50, 0, 0, 0, 0, 1},
    {"422 333x111", 333, 111, CS_YCC, 2, 1, 0, 3, 0, 92, 1, 0, 0, 0, 1},
    {"440 120x90", 120, 90, CS_YCC, 1, 2, 0, 0, 0, 92, 0, 0, 0, 0, 1},
    {"444 77x66 q100", 77, 66, CS_YCC, 1, 1, 0, 0, 0, 100, 0, 0, 0, 0, 1},
    {"444 64x64 q10 optimised", 64, 64, CS_YCC, 1, 1, 0, 5, 0, 10, 1, 0, 0, 0, 1},
    {"grey 99x45", 99, 45, CS_GREY, 1, 1, 0, 0, 0, 80, 0, 0, 0, 0, 1},
    {"grey 8x8 restart 1 optimised", 8, 8, CS_GREY, 1, 1, 0, 1, 0, 80, 1, 0, 0, 0, 1},
};

static const struct {
    Spec spec;
    const char *why;
} kRefused[] = {
    {{"progressive", 64, 48, CS_YCC, 2, 2, 0, 0, 0, 80, 0, 1, 0, 0, 1}, "progressive"},
    {{"arithmetic", 64, 48, CS_YCC, 2, 2, 0, 0, 0, 80, 0, 0, 1, 0, 1}, "arithmetic"},
    {{"non-interleaved", 64, 48, CS_YCC, 2, 2, 0, 0, 0, 80, 0, 0, 0, 1, 1}, "non-interleaved"},
    {{"CMYK", 32, 32, CS_CMYK, 1, 1, 0, 0, 0, 80, 0, 0, 0, 0, 1}, "components"},
    {{"Adobe RGB", 32, 32, CS_RGB, 1, 1, 0, 0, 0, 80, 0, 0, 0, 0, 1}, "RGB"},
    {{"411", 64, 32, CS_YCC, 4, 1, 0, 0, 0, 80, 0, 0, 0, 0, 1}, "4:1:1"},
    {{"subsampled 444", 64, 32, CS_YCC, 2, 1, 1, 0, 0, 80, 0, 0, 0, 0, 1}, "chroma"},
    {{"grey 2x2", 64, 32, CS_GREY, 2, 2, 0, 0, 0, 80, 0, 0, 0, 0, 1}, "grey"},
    {{"16-bit tables", 64, 32, CS_YCC, 2, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0}, "16-bit"},
};

#endif  // HAVE_LIBJPEG

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    targets();
#ifdef HAVE_LIBJPEG
    uint32_t files = 0;
    for (size_t i = 0; i < sizeof(kAccepted) / sizeof(kAccepted[0]); i++) {
        unsigned long n = 0;
        uint8_t *d = encode(&kAccepted[i], &n);
        if (!d)
            continue;
        compare(&kAccepted[i], d, n);
        edits(kAccepted[i].name, d, n);
        if (!kAccepted[i].optimize)
            without_dht(kAccepted[i].name, d, n);
        fuzz(d, n, 2000);
        free(d);
        files++;
    }
    for (size_t i = 0; i < sizeof(kRefused) / sizeof(kRefused[0]); i++) {
        unsigned long n = 0;
        uint8_t *d = encode(&kRefused[i].spec, &n);
        if (!d)
            continue;
        expect_refusal(kRefused[i].spec.name, d, n, kRefused[i].why);
        free(d);
        files++;
    }
#endif
    if (failures) {
        printf("FAIL nvjpg: %d failures\n", failures);
        return 1;
    }
#ifdef HAVE_LIBJPEG
    printf("PASS nvjpg (%u libjpeg-turbo %s files)\n", files, LIBJPEG_TURBO_VERSION_STRING);
#else
    printf("PASS nvjpg (no libjpeg-turbo: parser and targets only)\n");
#endif
    return 0;
}
