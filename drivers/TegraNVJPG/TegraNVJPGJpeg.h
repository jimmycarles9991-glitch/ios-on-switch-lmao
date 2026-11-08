// SPDX-License-Identifier: BSD-2-Clause-Patent
// JPEG headers to NVJPG's decoder setup. Plain C, header only: the kernel (TegraNVJPG.cpp, TegraNVJPGUserClient.cpp)
// and the host test (test/nvjpg_host.c, which checks it against libjpeg-turbo's own parse) compile the same code.
//
// What NVJPG 1.0 decodes (nvidia/nvjpg_drv.h "baseline sequential profile"; averne's oss-nvjpg and FFmpeg nvtegra
// MJPEG on the Switch): Huffman-coded sequential DCT with 8-bit samples (SOF0, and SOF1 within the same limits), one
// interleaved scan holding every component, one component (grey, 1x1) or three (YCbCr) with the luma sampled 1 or 2
// times the 1x1 chroma in each direction (4:4:4, 4:2:2, 4:4:0, 4:2:0), up to 4 quantisation tables of 8-bit values and
// 4 DC + 4 AC Huffman tables, restart intervals. Everything else is refused here, before any engine work, so the caller
// falls back to the CPU decoder: progressive, lossless, hierarchical and arithmetic-coded files, 12-bit samples, 16-bit
// quantisation tables, 2 or 4 components (CMYK, YCCK), Adobe RGB (transform 0) and 'R','G','B' component IDs, other
// sampling (4:1:1, subsampled 4:4:4, chroma sampled above 1x1, grey above 1x1), non-interleaved scans, more than one
// scan, DNL (height 0), sizes above 16384, tables a component uses that the file never defines, and Huffman tables
// whose code lengths do not fit (libjpeg's JERR_BAD_HUFF_TABLE).
//
// The parser reads only the bytes it is given and never writes outside `out`; every length is checked against the end
// before use, so any byte sequence is safe to hand it (the host test fuzzes it).
#ifndef TEGRA_NVJPG_JPEG_H
#define TEGRA_NVJPG_JPEG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nvidia/nvjpg_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEGRA_NVJPG_MAX_SIDE 16384u

// NVJPG's stream_chroma_mode / output_chroma_mode values (nvjpg_drv.h).
enum {
    TEGRA_NVJPG_CHROMA_MONO = 0,
    TEGRA_NVJPG_CHROMA_420 = 1,
    TEGRA_NVJPG_CHROMA_422H = 2,  // luma 2x1: 4:2:2
    TEGRA_NVJPG_CHROMA_422V = 3,  // luma 1x2: 4:4:0
    TEGRA_NVJPG_CHROMA_444 = 4,
};

typedef struct {
    uint8_t id, h, v, tq;  // frame header (SOF): component ID, sampling factors, quantisation table
    uint8_t td, ta;        // scan header (SOS): DC and AC Huffman tables
    uint8_t pad[2];
} TegraNVJPGComponent;

typedef struct {
    uint32_t width, height;       // pixels (SOF)
    uint32_t components;          // 1 or 3
    uint32_t chroma;              // TEGRA_NVJPG_CHROMA_*
    uint32_t mcuW, mcuH;          // MCU size in pixels: 8 x (largest sampling factor), 8x8 for grey
    uint32_t mcusX, mcusY;        // MCUs across and down
    uint32_t restart;             // DRI: MCUs per restart interval, 0 for none
    TegraNVJPGComponent comp[3];  // frame order
    uint8_t quant[4][64];         // as stored in DQT: zig-zag order
    uint8_t bits[2][4][16];       // [0] DC tables (Tc 0), [1] AC tables (Tc 1): codes of length 1..16
    uint8_t vals[2][4][162];      // their symbols (HUFFVAL), zero past count
    uint32_t count[2][4];         // symbols in each table
    uint32_t quantMask;           // tables defined (bit n: table n)
    uint32_t huffMask[2];
    uint32_t scanOffset;  // the scan's first entropy-coded byte (just after the SOS header)
    uint32_t scanEnd;     // one past its last byte: just after EOI, or the end of the data (eoi 0)
    uint32_t eoi;         // the scan ends in EOI; 0: the data ended first (a copy appends one)
    uint32_t jfif, adobe, adobeTransform;
    uint32_t stdTables;  // bit tc * 4 + n: table n of class tc is Annex K's (the file had none)
} TegraNVJPGImage;

static inline uint32_t tegra_nvjpg_be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

// Canonical Huffman codes (ITU T.81 C.2, libjpeg jdhuff.c jpeg_make_d_derived_tbl): every code of length l must be
// below 2^l. A table that breaks this is corrupt and libjpeg refuses it too.
static inline int tegra_nvjpg_huffman_fits(const uint8_t bits[16])
{
    uint32_t code = 0;
    for (uint32_t l = 1; l <= 16; l++) {
        code += bits[l - 1];
        if (code > (1u << l))
            return 0;
        code <<= 1;
    }
    return 1;
}

static inline const char *tegra_nvjpg_sof(const uint8_t *p, uint32_t length, TegraNVJPGImage *img)
{
    if (length < 6)
        return "SOF too short";
    if (p[0] != 8)
        return "not 8-bit samples";
    img->height = tegra_nvjpg_be16(p + 1);
    img->width = tegra_nvjpg_be16(p + 3);
    img->components = p[5];
    if (!img->height)
        return "height 0 (DNL)";
    if (!img->width || img->width > TEGRA_NVJPG_MAX_SIDE || img->height > TEGRA_NVJPG_MAX_SIDE)
        return "size outside 1..16384";
    if (img->components != 1 && img->components != 3)
        return "not 1 or 3 components";
    if (length != 6 + 3 * img->components)
        return "SOF length";
    for (uint32_t i = 0; i < img->components; i++) {
        TegraNVJPGComponent *c = &img->comp[i];
        c->id = p[6 + 3 * i];
        c->h = p[7 + 3 * i] >> 4;
        c->v = p[7 + 3 * i] & 15;
        c->tq = p[8 + 3 * i];
        if (c->tq > 3)
            return "quantisation table above 3";
        for (uint32_t j = 0; j < i; j++) {
            if (img->comp[j].id == c->id)
                return "duplicate component ID";
        }
    }
    if (img->components == 1) {
        // A grey scan is non-interleaved: its MCU is one block whatever the factors say; NVJPG is trusted with 1x1.
        if (img->comp[0].h != 1 || img->comp[0].v != 1)
            return "grey with sampling factors above 1x1";
        img->chroma = TEGRA_NVJPG_CHROMA_MONO;
        img->mcuW = img->mcuH = 8;
    } else {
        const TegraNVJPGComponent *y = &img->comp[0];
        if (img->comp[1].h != 1 || img->comp[1].v != 1 || img->comp[2].h != 1 || img->comp[2].v != 1)
            return "chroma sampled above 1x1";
        if (y->h == 2 && y->v == 2) {
            img->chroma = TEGRA_NVJPG_CHROMA_420;
        } else if (y->h == 2 && y->v == 1) {
            img->chroma = TEGRA_NVJPG_CHROMA_422H;
        } else if (y->h == 1 && y->v == 2) {
            img->chroma = TEGRA_NVJPG_CHROMA_422V;
        } else if (y->h == 1 && y->v == 1) {
            img->chroma = TEGRA_NVJPG_CHROMA_444;
        } else {
            return "luma sampling other than 1 or 2 (4:1:1)";
        }
        if (img->comp[0].id == 'R' && img->comp[1].id == 'G' && img->comp[2].id == 'B')
            return "RGB components";
        img->mcuW = 8u * y->h;
        img->mcuH = 8u * y->v;
    }
    img->mcusX = (img->width + img->mcuW - 1) / img->mcuW;
    img->mcusY = (img->height + img->mcuH - 1) / img->mcuH;
    return NULL;
}

static inline const char *tegra_nvjpg_dqt(const uint8_t *p, uint32_t length, TegraNVJPGImage *img)
{
    for (uint32_t at = 0; at < length;) {
        const uint32_t pq = p[at] >> 4;
        const uint32_t tq = p[at] & 15;
        if (pq != 0)
            return "16-bit quantisation table";
        if (tq > 3)
            return "quantisation table above 3";
        if (length - at < 65)
            return "DQT too short";
        for (uint32_t i = 0; i < 64; i++) {
            if (!p[at + 1 + i])
                return "quantisation value 0";
            img->quant[tq][i] = p[at + 1 + i];
        }
        img->quantMask |= 1u << tq;
        at += 65;
    }
    return NULL;
}

static inline const char *tegra_nvjpg_dht(const uint8_t *p, uint32_t length, TegraNVJPGImage *img)
{
    for (uint32_t at = 0; at < length;) {
        if (length - at < 17)
            return "DHT too short";
        const uint32_t tc = p[at] >> 4;
        const uint32_t th = p[at] & 15;
        if (tc > 1 || th > 3)
            return "Huffman table class or index";
        uint32_t n = 0;
        for (uint32_t i = 0; i < 16; i++)
            n += p[at + 1 + i];
        if (n > (tc ? 162u : 12u))
            return "Huffman table too long";
        if (length - at - 17 < n)
            return "DHT too short";
        if (!tegra_nvjpg_huffman_fits(p + at + 1))
            return "Huffman code lengths do not fit";
        memcpy(img->bits[tc][th], p + at + 1, 16);
        memset(img->vals[tc][th], 0, sizeof(img->vals[tc][th]));
        memcpy(img->vals[tc][th], p + at + 17, n);
        if (!tc) {
            for (uint32_t i = 0; i < n; i++) {
                if (img->vals[0][th][i] > 11)
                    return "DC symbol above 11";
            }
        }
        img->count[tc][th] = n;
        img->huffMask[tc] |= 1u << th;
        at += 17 + n;
    }
    return NULL;
}

// ITU T.81 Annex K.3's typical tables (luminance and chrominance DC and AC), which libjpeg (jstdhuff.c) supplies for
// table 0 and 1 when a file defines none: Motion JPEG frames (AVI MJPEG, many cameras) leave DHT out.
static const uint8_t kTegraNVJPGStdBits[2][2][16] = {
    {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}},
    {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d}, {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77}},
};
static const uint8_t kTegraNVJPGStdAcVals[2][162] = {
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
     0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
     0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
     0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
     0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
     0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
     0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
     0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
     0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
     0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
     0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
};

// Supplies table `th` (0 or 1) of class `tc` from Annex K when the file never defined it.
static inline void tegra_nvjpg_std_table(TegraNVJPGImage *img, uint32_t tc, uint32_t th)
{
    uint32_t n = 0;
    memcpy(img->bits[tc][th], kTegraNVJPGStdBits[tc][th], 16);
    for (uint32_t i = 0; i < 16; i++)
        n += img->bits[tc][th][i];
    memset(img->vals[tc][th], 0, sizeof(img->vals[tc][th]));
    if (tc) {
        memcpy(img->vals[1][th], kTegraNVJPGStdAcVals[th], n);
    } else {
        for (uint32_t i = 0; i < n; i++)
            img->vals[0][th][i] = (uint8_t)i;  // DC symbols 0..11 in order
    }
    img->count[tc][th] = n;
    img->huffMask[tc] |= 1u << th;
    img->stdTables |= 1u << (tc * 4 + th);
}

static inline const char *tegra_nvjpg_sos(const uint8_t *p, uint32_t length, TegraNVJPGImage *img)
{
    if (!img->components)
        return "SOS before SOF";
    if (length < 1 || p[0] != img->components)
        return "scan does not hold every component (non-interleaved)";
    if (length != 4 + 2 * img->components)
        return "SOS length";
    for (uint32_t i = 0; i < img->components; i++) {
        TegraNVJPGComponent *c = &img->comp[i];
        if (p[1 + 2 * i] != c->id)
            return "scan component order";
        c->td = p[2 + 2 * i] >> 4;
        c->ta = p[2 + 2 * i] & 15;
        if (c->td > 3 || c->ta > 3)
            return "Huffman table above 3";
        if (!(img->huffMask[0] & (1u << c->td)) && c->td < 2)
            tegra_nvjpg_std_table(img, 0, c->td);
        if (!(img->huffMask[1] & (1u << c->ta)) && c->ta < 2)
            tegra_nvjpg_std_table(img, 1, c->ta);
        if (!(img->huffMask[0] & (1u << c->td)) || !(img->huffMask[1] & (1u << c->ta)))
            return "scan uses an undefined Huffman table";
        if (!(img->quantMask & (1u << c->tq)))
            return "component uses an undefined quantisation table";
    }
    const uint8_t *s = p + 1 + 2 * (size_t)img->components;
    if (s[0] != 0 || s[1] != 63 || s[2] != 0)
        return "spectral selection or approximation (not sequential)";
    return NULL;
}

// The entropy-coded data runs until a marker other than a restart marker (0xFF then 0x00 is a stuffed byte, 0xFF fill
// bytes may precede a marker). Ending in EOI is the normal case; ending with the data (a truncated file) is passed on
// and the copy appends EOI; any other marker means another scan or a table change between scans (refused).
static inline const char *tegra_nvjpg_scan_end(const uint8_t *data, size_t length, TegraNVJPGImage *img)
{
    size_t at = img->scanOffset;
    for (;;) {
        const uint8_t *ff = (const uint8_t *)memchr(data + at, 0xff, length - at);
        if (!ff) {
            img->scanEnd = (uint32_t)length;
            img->eoi = 0;
            return NULL;
        }
        at = (size_t)(ff - data) + 1;
        while (at < length && data[at] == 0xff)
            at++;
        if (at == length) {
            img->scanEnd = (uint32_t)length;
            img->eoi = 0;
            return NULL;
        }
        const uint8_t marker = data[at++];
        if (marker == 0x00 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (marker == 0xd9) {
            img->scanEnd = (uint32_t)at;
            img->eoi = 1;
            return NULL;
        }
        return marker == 0xdc ? "DNL marker" : "more than one scan";
    }
}

// Parses the headers up to the first scan (img->scanOffset: the scan's first byte). NULL on success, else why NVJPG
// cannot take it. `length` may stop anywhere after the SOS header (the kernel parses a first chunk of the file).
static inline const char *tegra_nvjpg_parse_headers(const uint8_t *data, size_t length, TegraNVJPGImage *img)
{
    memset(img, 0, sizeof(*img));
    if (length < 4 || length > 0xffffffffu)
        return "length";
    if (data[0] != 0xff || data[1] != 0xd8)
        return "no SOI";
    size_t at = 2;
    int sof = 0;
    for (;;) {
        if (at >= length || data[at] != 0xff)
            return "marker expected";
        while (at < length && data[at] == 0xff)
            at++;
        if (at >= length)
            return "data ends in a marker";
        const uint8_t marker = data[at++];
        if (marker == 0xd8)
            return "second SOI";
        if (marker == 0xd9)
            return "EOI before any scan";
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;  // TEM, stray RSTn: no length
        if (length - at < 2)
            return "segment length missing";
        const uint32_t segment = tegra_nvjpg_be16(data + at);
        if (segment < 2 || segment > length - at)
            return "segment length";
        const uint8_t *p = data + at + 2;
        const uint32_t n = segment - 2;
        const char *why = NULL;
        switch (marker) {
        case 0xc0:
        case 0xc1:
            if (sof)
                return "second SOF";
            sof = 1;
            why = tegra_nvjpg_sof(p, n, img);
            break;
        case 0xc2:
        case 0xc6:
        case 0xca:
        case 0xce:
            return "progressive";
        case 0xc3:
        case 0xc7:
        case 0xcb:
        case 0xcf:
            return "lossless";
        case 0xc5:
        case 0xcd:
        case 0xde:
        case 0xdf:
            return "hierarchical";
        case 0xc9:
        case 0xcc:
            return "arithmetic coding";
        case 0xf7:
            return "JPEG-LS";
        case 0xdc:
            return "DNL marker";
        case 0xc4:
            why = tegra_nvjpg_dht(p, n, img);
            break;
        case 0xdb:
            why = tegra_nvjpg_dqt(p, n, img);
            break;
        case 0xdd:
            if (n != 2)
                return "DRI length";
            img->restart = tegra_nvjpg_be16(p);
            break;
        case 0xe0:
            if (n >= 5 && !memcmp(p, "JFIF\0", 5))
                img->jfif = 1;
            break;
        case 0xee:
            if (n >= 12 && !memcmp(p, "Adobe", 5)) {
                img->adobe = 1;
                img->adobeTransform = p[11];
            }
            break;
        case 0xda:
            if (!sof)
                return "SOS before SOF";
            why = tegra_nvjpg_sos(p, n, img);
            if (why)
                return why;
            // Three components without JFIF are YCbCr unless an Adobe marker says transform 0 (libjpeg's guess).
            if (img->components == 3 && img->adobe && img->adobeTransform == 0 && !img->jfif)
                return "Adobe RGB";
            img->scanOffset = (uint32_t)(at + segment);
            return NULL;
        default:
            break;  // APPn, COM, JPGn: skipped
        }
        if (why)
            return why;
        at += segment;
    }
}

// Parses the headers and finds the scan's end in the whole file. NULL on success, else why NVJPG cannot take it.
static inline const char *tegra_nvjpg_parse(const uint8_t *data, size_t length, TegraNVJPGImage *img)
{
    const char *why = tegra_nvjpg_parse_headers(data, length, img);
    return why ? why : tegra_nvjpg_scan_end(data, length, img);
}

// The image half of NVJPG's decoder setup (nvjpg_dec_drv_pic_setup_s); the output half is filled by the caller (the
// target's format, strides, scale, colour). Tables as the firmware takes them (averne's FFmpeg nvtegra MJPEG and
// oss-nvjpg, both decoding on the Switch): huffTab[0] the DC tables and huffTab[1] the AC tables by index, codeNum[i]
// the number of codes of length i + 1, symbol[] HUFFVAL, minCodeIdx/minCode left zero (the firmware derives them);
// quant[] as DQT stores them (zig-zag); blkPar by frame component; MCU counts, not pixels; bitstream_size the scan's
// bytes from the SOS header's end.
static inline void tegra_nvjpg_setup_image(const TegraNVJPGImage *img, uint32_t scanBytes, nvjpg_dec_drv_pic_setup_s *s)
{
    memset(s, 0, sizeof(*s));
    for (uint32_t tc = 0; tc < 2; tc++) {
        for (uint32_t t = 0; t < 4; t++) {
            for (uint32_t i = 0; i < 16; i++)
                s->huffTab[tc][t].codeNum[i] = img->bits[tc][t][i];
            memcpy(s->huffTab[tc][t].symbol, img->vals[tc][t], 162);
        }
    }
    for (uint32_t i = 0; i < img->components; i++) {
        s->blkPar[i].hblock = img->comp[i].h;
        s->blkPar[i].vblock = img->comp[i].v;
        s->blkPar[i].quant = img->comp[i].tq;
        s->blkPar[i].ac = img->comp[i].ta;
        s->blkPar[i].dc = img->comp[i].td;
    }
    memcpy(s->quant, img->quant, sizeof(s->quant));
    s->restart_interval = (int)img->restart;
    s->frame_width = (int)img->width;
    s->frame_height = (int)img->height;
    s->mcu_width = (int)img->mcusX;
    s->mcu_height = (int)img->mcusY;
    s->comp = (int)img->components;
    s->bitstream_offset = 0;
    s->bitstream_size = (int)scanBytes;
    s->stream_chroma_mode = (int)img->chroma;
}

// Targets NVJPG writes (output_pixel_format, output_chroma_mode, memory_mode): semi-planar YUV (a luma plane and an
// interleaved CbCr plane) at 4:2:0, 4:2:2 or 4:4:4 whatever the file's sampling, planar YUV 4:2:0 (three planes),
// packed 4:2:2 (YUY2: Y0 Cb Y1 Cr, CoreVideo's 'yuvf' order), and 32-bit RGB in its byte orders (planar memory mode,
// as oss-nvjpg drives RGB surfaces), each optionally downscaled by 2, 4 or 8 (DCT scaling, power2_downscale). Pitch linear only (the T210's NVJPG has no tiled output, averne's FFmpeg note). Strides are
// 64-byte multiples and plane addresses 256-byte multiples (the methods take address >> 8).
enum {
    TEGRA_NVJPG_OUT_NV12 = 0,  // '420f': Y plane, CbCr plane (Cb first)
    TEGRA_NVJPG_OUT_I420 = 1,  // 'y420': Y, Cb, Cr planes
    TEGRA_NVJPG_OUT_BGRA = 2,  // bytes B, G, R, A ('BGRA')
    TEGRA_NVJPG_OUT_RGBA = 3,  // bytes R, G, B, A ('RGBA')
    TEGRA_NVJPG_OUT_ARGB = 4,  // bytes A, R, G, B ('ARGB')
    TEGRA_NVJPG_OUT_GREY = 5,  // Y plane only ('L008'), grey files only
    TEGRA_NVJPG_OUT_NV16 = 6,  // '422f': Y plane, CbCr plane at half width, full height
    TEGRA_NVJPG_OUT_NV24 = 7,  // '444f': Y plane, CbCr plane at full width and height
    TEGRA_NVJPG_OUT_YUY2 = 8,  // 'yuvf': one plane, Y0 Cb Y1 Cr
    TEGRA_NVJPG_OUT_COUNT = 9,
};

// nvjpg_drv.h output_pixel_format and memory_mode values.
enum {
    TEGRA_NVJPG_PIXEL_YUV = 0,
    TEGRA_NVJPG_PIXEL_RGBA = 3,
    TEGRA_NVJPG_PIXEL_BGRA = 4,
    TEGRA_NVJPG_PIXEL_ARGB = 6
};

enum {
    TEGRA_NVJPG_MEMORY_NV12 = 0,
    TEGRA_NVJPG_MEMORY_YUY2 = 2,
    TEGRA_NVJPG_MEMORY_PLANAR = 3
};

typedef struct {
    uint32_t format;    // TEGRA_NVJPG_OUT_*
    uint32_t scale;     // log2 of the downscale: 0..3
    uint32_t pitch[3];  // bytes per row of each plane (one plane for RGB and grey; NV12 two; I420 three)
    uint32_t alpha;     // the A byte of RGB outputs
} TegraNVJPGTarget;

// What a decode writes: the visible picture, and per plane the rows and bytes per row the engine may touch. `padded`:
// the engine writes whole MCUs (the extent then covers the MCU grid, scaled); 0 for an engine that clips to the
// picture (TegraNVJPGTraits).
typedef struct {
    uint32_t width, height;  // the picture after scaling (libjpeg's jdiv_round_up(size, 2^scale))
    uint32_t planes;
    uint32_t rows[3], rowBytes[3];
} TegraNVJPGExtent;

static inline uint32_t tegra_nvjpg_planes(uint32_t format)
{
    switch (format) {
    case TEGRA_NVJPG_OUT_NV12:
    case TEGRA_NVJPG_OUT_NV16:
    case TEGRA_NVJPG_OUT_NV24:
        return 2;
    case TEGRA_NVJPG_OUT_I420:
        return 3;
    default:
        return 1;
    }
}

static inline bool tegra_nvjpg_is_yuv(uint32_t format)
{
    return format == TEGRA_NVJPG_OUT_NV12 || format == TEGRA_NVJPG_OUT_I420 || format == TEGRA_NVJPG_OUT_NV16 ||
           format == TEGRA_NVJPG_OUT_NV24 || format == TEGRA_NVJPG_OUT_YUY2;
}

static inline uint32_t tegra_nvjpg_shr_up(uint32_t value, uint32_t shift)
{
    return (value + (1u << shift) - 1) >> shift;
}

// JFIF's YCbCr to RGB (ITU-T T.871, full range: R = Y + 1.402 Cr', G = Y - 0.344136 Cb' - 0.714136 Cr',
// B = Y + 1.772 Cb') in NVJPG's yuv2rgb_param order {Y gain, Cr->R, Cb->G, Cr->G, Cb->B, Y offset}, 16.16 fixed point
// rounded to nearest (oss-nvjpg's BT.601 "extended range" kernel holds the same coefficients).
static const int32_t kTegraNVJPGJfif[6] = {65536, 91881, -22554, -46802, 116130, 0};

// The output half of the setup, and what the decode will write. NULL on success, else why the target is refused.
static inline const char *tegra_nvjpg_setup_output(const TegraNVJPGImage *img, const TegraNVJPGTarget *t, bool padded,
                                                   nvjpg_dec_drv_pic_setup_s *s, TegraNVJPGExtent *e)
{
    memset(e, 0, sizeof(*e));
    if (t->format >= TEGRA_NVJPG_OUT_COUNT)
        return "output format";
    if (t->scale > 3)
        return "scale";
    if (t->format == TEGRA_NVJPG_OUT_GREY && img->components != 1)
        return "grey output from a colour file";
    if (img->components == 1 && tegra_nvjpg_is_yuv(t->format))
        return "YUV output from a grey file";  // the service writes grey, then fills the chroma itself
    const uint32_t gridW = img->mcusX * img->mcuW;
    const uint32_t gridH = img->mcusY * img->mcuH;
    e->width = tegra_nvjpg_shr_up(img->width, t->scale);
    e->height = tegra_nvjpg_shr_up(img->height, t->scale);
    const uint32_t w = padded ? gridW >> t->scale : e->width;
    const uint32_t h = padded ? gridH >> t->scale : e->height;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    e->planes = tegra_nvjpg_planes(t->format);
    switch (t->format) {
    case TEGRA_NVJPG_OUT_NV12:
        e->rows[0] = h, e->rowBytes[0] = w;
        e->rows[1] = ch, e->rowBytes[1] = 2 * cw;
        break;
    case TEGRA_NVJPG_OUT_I420:
        e->rows[0] = h, e->rowBytes[0] = w;
        e->rows[1] = e->rows[2] = ch;
        e->rowBytes[1] = e->rowBytes[2] = cw;
        break;
    case TEGRA_NVJPG_OUT_NV16:
        e->rows[0] = e->rows[1] = h;
        e->rowBytes[0] = w, e->rowBytes[1] = 2 * cw;
        break;
    case TEGRA_NVJPG_OUT_NV24:
        e->rows[0] = e->rows[1] = h;
        e->rowBytes[0] = w, e->rowBytes[1] = 2 * w;
        break;
    case TEGRA_NVJPG_OUT_YUY2:
        e->rows[0] = h, e->rowBytes[0] = 4 * cw;
        break;
    case TEGRA_NVJPG_OUT_GREY:
        e->rows[0] = h, e->rowBytes[0] = w;
        break;
    default:
        e->rows[0] = h, e->rowBytes[0] = 4 * w;
        break;
    }
    // The engine may write a row in 64-byte bursts (its strides are 64-byte multiples): the extent covers them.
    for (uint32_t p = 0; p < e->planes; p++) {
        e->rowBytes[p] = (e->rowBytes[p] + 63) & ~63u;
        if (!t->pitch[p] || (t->pitch[p] & 63))
            return "stride not a multiple of 64";
        if (t->pitch[p] < e->rowBytes[p])
            return "stride below the row";
    }
    if (e->planes == 3 && t->pitch[1] != t->pitch[2])
        return "chroma strides differ";
    s->output_stride_luma = (int)t->pitch[0];
    s->output_stride_chroma = e->planes > 1 ? (int)t->pitch[1] : 0;
    s->power2_downscale = (int)t->scale;
    s->tile_mode = 0;
    s->motion_jpeg_type = 0;
    switch (t->format) {
    case TEGRA_NVJPG_OUT_NV12:
    case TEGRA_NVJPG_OUT_I420:
        s->output_pixel_format = TEGRA_NVJPG_PIXEL_YUV;
        s->output_chroma_mode = TEGRA_NVJPG_CHROMA_420;
        s->memory_mode = t->format == TEGRA_NVJPG_OUT_NV12 ? TEGRA_NVJPG_MEMORY_NV12 : TEGRA_NVJPG_MEMORY_PLANAR;
        break;
    case TEGRA_NVJPG_OUT_NV16:
    case TEGRA_NVJPG_OUT_YUY2:
        s->output_pixel_format = TEGRA_NVJPG_PIXEL_YUV;
        s->output_chroma_mode = TEGRA_NVJPG_CHROMA_422H;
        s->memory_mode = t->format == TEGRA_NVJPG_OUT_NV16 ? TEGRA_NVJPG_MEMORY_NV12 : TEGRA_NVJPG_MEMORY_YUY2;
        break;
    case TEGRA_NVJPG_OUT_NV24:
        s->output_pixel_format = TEGRA_NVJPG_PIXEL_YUV;
        s->output_chroma_mode = TEGRA_NVJPG_CHROMA_444;
        s->memory_mode = TEGRA_NVJPG_MEMORY_NV12;
        break;
    case TEGRA_NVJPG_OUT_GREY:
        s->output_pixel_format = TEGRA_NVJPG_PIXEL_YUV;
        s->output_chroma_mode = TEGRA_NVJPG_CHROMA_MONO;
        s->memory_mode = TEGRA_NVJPG_MEMORY_PLANAR;  // NV12 fails for grey (averne's FFmpeg note)
        break;
    default:
        if (t->format == TEGRA_NVJPG_OUT_BGRA) {
            s->output_pixel_format = TEGRA_NVJPG_PIXEL_BGRA;
        } else if (t->format == TEGRA_NVJPG_OUT_RGBA) {
            s->output_pixel_format = TEGRA_NVJPG_PIXEL_RGBA;
        } else {
            s->output_pixel_format = TEGRA_NVJPG_PIXEL_ARGB;
        }
        s->output_chroma_mode = (int)img->chroma;
        s->memory_mode = TEGRA_NVJPG_MEMORY_PLANAR;
        s->alpha_value = (int)(t->alpha & 0xff);
        for (uint32_t i = 0; i < 6; i++)
            s->yuv2rgb_param[i] = kTegraNVJPGJfif[i];
        break;
    }
    return NULL;
}

#ifdef __cplusplus
static_assert(sizeof(nvjpg_dec_drv_pic_setup_s) == 0xb2c, "NVJPG decoder setup (oss-nvjpg: 0xb2c)");
static_assert(sizeof(nvjpg_dec_status) == 0x20, "NVJPG decoder status");
static_assert(offsetof(nvjpg_dec_drv_pic_setup_s, quant) == 0x9c0 &&
                  offsetof(nvjpg_dec_drv_pic_setup_s, restart_interval) == 0xac0 &&
                  offsetof(nvjpg_dec_drv_pic_setup_s, yuv2rgb_param) == 0xaf8,
              "NVJPG decoder setup layout");
}
#else
_Static_assert(sizeof(nvjpg_dec_drv_pic_setup_s) == 0xb2c, "NVJPG decoder setup (oss-nvjpg: 0xb2c)");
_Static_assert(sizeof(nvjpg_dec_status) == 0x20, "NVJPG decoder status");
_Static_assert(offsetof(nvjpg_dec_drv_pic_setup_s, quant) == 0x9c0 &&
                   offsetof(nvjpg_dec_drv_pic_setup_s, restart_interval) == 0xac0 &&
                   offsetof(nvjpg_dec_drv_pic_setup_s, yuv2rgb_param) == 0xaf8,
               "NVJPG decoder setup layout");
#endif

#endif
