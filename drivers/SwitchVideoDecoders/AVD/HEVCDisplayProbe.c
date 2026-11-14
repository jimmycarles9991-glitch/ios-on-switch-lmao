// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCDisplayProbe.h.
#include "HEVCDisplayProbe.h"
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "BlockLinear.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"

typedef struct {
    uint64_t sum, trunc, round;
    uint32_t max;
} Score;

// One layout's reading of the display luma into `out` (width x height bytes); false when the buffer cannot hold it.
static bool readLayout(uint32_t layout, const uint8_t *display, uint64_t bytes, uint32_t pitch16, uint32_t rows,
                       uint32_t width, uint32_t height, uint8_t *out)
{
    const uint32_t pitch = layout == HEVC_DISPLAY_BLOCK_8 || layout == HEVC_DISPLAY_PITCH_8 ? pitch16 / 2 : pitch16;
    if ((uint64_t)pitch * rows > bytes || width > pitch)
        return false;
    if (layout == HEVC_DISPLAY_BLOCK_8 || layout == HEVC_DISPLAY_BLOCK_16) {
        if (pitch % 64)
            return false;
        block_linear_copy(out, width, display, pitch, 0, 0, width, height);
        return true;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(out + (size_t)y * width, display + (size_t)y * pitch, width);
    return true;
}

static Score score(const uint8_t *eight, const uint16_t *ten, size_t samples)
{
    Score s = {0, 0, 0, 0};
    for (size_t i = 0; i < samples; i++) {
        const uint32_t ref10 = ten[i] >> 6;
        const uint32_t trunc = ref10 >> 2;
        const uint32_t round = ref10 + 2 > 1023 ? 255 : (ref10 + 2) >> 2;
        const uint32_t diff = eight[i] > trunc ? eight[i] - trunc : trunc - eight[i];
        s.sum += diff;
        s.max = diff > s.max ? diff : s.max;
        s.trunc += eight[i] == trunc;
        s.round += eight[i] == round;
    }
    return s;
}

// ITU-R BT.2087's Rec.2020 -> Rec.709 primaries matrix (rows R, G, B).
static const double kToRec709[3][3] = {
    {1.6605, -0.5876, -0.0728},
    {-0.1246, 1.1329, -0.0083},
    {-0.0182, -0.1006, 1.1187},
};

static double clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

uint8_t hevc_display_model_luma(uint32_t model, uint32_t y10, uint32_t cb10, uint32_t cr10)
{
    const double kr = 0.2627, kb = 0.0593;  // Rec.2020 in
    const double y = (y10 - 64.0) / 876.0, pb = (cb10 - 512.0) / 896.0, pr = (cr10 - 512.0) / 896.0;
    double rgb[3] = {y + 2 * (1 - kr) * pr, 0, y + 2 * (1 - kb) * pb};
    rgb[1] = (y - kr * rgb[0] - kb * rgb[2]) / (1 - kr - kb);
    double out[3] = {rgb[0], rgb[1], rgb[2]};
    if (model != HEVC_DISPLAY_MODEL_MATRIX) {
        double in[3];
        for (int c = 0; c < 3; c++)
            in[c] = model == HEVC_DISPLAY_MODEL_LINEAR ? pow(clamp01(rgb[c]), 2.4) : rgb[c];
        for (int c = 0; c < 3; c++) {
            const double v = kToRec709[c][0] * in[0] + kToRec709[c][1] * in[1] + kToRec709[c][2] * in[2];
            out[c] = model == HEVC_DISPLAY_MODEL_LINEAR ? pow(clamp01(v), 1 / 2.4) : v;
        }
    }
    const double luma = 0.2126 * clamp01(out[0]) + 0.7152 * clamp01(out[1]) + 0.0722 * clamp01(out[2]);  // Rec.709
    return (uint8_t)floor(16 + 219 * luma + 0.5);
}

// The best reading against each conversion model, the chroma taken at each luma sample's 4:2:0 site.
static void scoreModels(const uint8_t *eight, const uint16_t *ten, const uint16_t *chroma, uint32_t width,
                        uint32_t height, HEVCDisplayProbeResult *out)
{
    const size_t samples = (size_t)width * height;
    for (uint32_t m = 0; m < HEVC_DISPLAY_MODELS; m++) {
        uint64_t sum = 0;
        uint32_t max = 0;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                const size_t c = (size_t)(y / 2) * width + (x & ~1u);
                const uint32_t want =
                    hevc_display_model_luma(m, ten[(size_t)y * width + x] >> 6, chroma[c] >> 6, chroma[c + 1] >> 6);
                const uint8_t got = eight[(size_t)y * width + x];
                const uint32_t diff = got > want ? got - want : want - got;
                sum += diff;
                max = diff > max ? diff : max;
            }
        out->model_max[m] = max;
        out->model_mean_milli[m] = (uint32_t)(sum * 1000 / samples);
    }
}

void hevc_display_probe_compare(const uint8_t *display, uint64_t displayBytes, const uint8_t *ref16,
                                const uint8_t *chroma16, uint32_t pitch16, uint32_t rows, uint32_t width,
                                uint32_t height, HEVCDisplayProbeResult *out)
{
    const int status = out->status;
    memset(out, 0, sizeof(*out));
    out->status = status;
    const uint64_t lumaBytes = (uint64_t)pitch16 * rows;
    bool written = false;
    for (uint64_t i = 0; i < lumaBytes && i < displayBytes && !written; i++)
        written = display[i] != HEVC_DISPLAY_POISON;
    const size_t samples = (size_t)width * height;
    const uint32_t chromaRows = (height + 1) / 2;
    uint16_t *ten = written ? malloc(samples * 2) : NULL;
    uint16_t *chroma = written ? malloc((size_t)width * chromaRows * 2) : NULL;
    uint8_t *eight = written ? malloc(samples) : NULL;
    uint8_t *best8 = written ? malloc(samples) : NULL;
    if (ten && chroma && eight && best8) {
        block_linear_copy((uint8_t *)ten, (size_t)width * 2, ref16, pitch16, 0, 0, width * 2, height);
        block_linear_copy((uint8_t *)chroma, (size_t)width * 2, chroma16, pitch16, 0, 0, width * 2, chromaRows);
        uint64_t best = UINT64_MAX;
        for (uint32_t layout = HEVC_DISPLAY_BLOCK_8; layout < HEVC_DISPLAY_LAYOUTS; layout++) {
            if (!readLayout(layout, display, displayBytes, pitch16, rows, width, height, eight))
                continue;
            const Score s = score(eight, ten, samples);
            if (s.sum >= best)
                continue;
            best = s.sum;
            out->layout = layout;
            out->max_diff = s.max;
            out->mean_milli = (uint32_t)(s.sum * 1000 / samples);
            out->trunc_milli = (uint32_t)(s.trunc * 1000 / samples);
            out->round_milli = (uint32_t)(s.round * 1000 / samples);
            memcpy(best8, eight, samples);
        }
        if (out->layout)
            scoreModels(best8, ten, chroma, width, height, out);
    }
    free(ten);
    free(chroma);
    free(eight);
    free(best8);
}

static void add(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    s->entries[s->entry_count++] = (TegraNVDECEntry){method, kind, value, offset};
}

void hevc_display_probe_run(const H264Backend *b, const TegraNVDECSubmit *job, const TegraNVDECWait *wait,
                            uint32_t surfaceBytes, uint32_t lumaBytes, const uint8_t *ref16, uint32_t pitch16,
                            uint32_t rows, uint32_t width, uint32_t height, uint32_t mode, HEVCDisplayProbeResult *out)
{
    memset(out, 0, sizeof(*out));
    if (job->entry_count + 2 > TEGRA_NVDEC_MAX_ENTRIES || job->clean_count >= TEGRA_NVDEC_MAX_RANGES ||
        wait->invalidate_count >= TEGRA_NVDEC_MAX_RANGES) {
        out->status = -1;
        return;
    }
    uint32_t handle = 0;
    void *va = NULL;
    const uint64_t bytes = 2ull * surfaceBytes;  // the scratch slot, then the display buffer
    if (b->alloc(b->ctx, bytes, &handle, &va)) {
        out->status = -3;
        return;
    }
    memset(va, HEVC_DISPLAY_POISON, bytes);
    TegraNVDECSubmit s = *job;
    nvdec_hevc_pic_s *p = (nvdec_hevc_pic_s *)s.setup;
    p->disp_output_mode = (unsigned char)mode;
    const uint16_t luma = (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * p->curr_pic_idx);
    const uint16_t chroma = (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * p->curr_pic_idx);
    for (uint32_t i = 0; i < s.entry_count; i++) {
        if (s.entries[i].method == luma || s.entries[i].method == chroma) {
            s.entries[i].value = handle;
            s.entries[i].offset = s.entries[i].method == luma ? 0 : lumaBytes;
        }
    }
    const TegraNVDECEntry execute = s.entries[--s.entry_count];
    add(&s, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, handle, surfaceBytes);
    add(&s, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, handle, surfaceBytes + lumaBytes);
    s.entries[s.entry_count++] = execute;
    s.clean[s.clean_count++] = (TegraNVDECRange){handle, 0, (uint32_t)bytes, 0};
    TegraNVDECWait w = *wait;
    w.invalidate[w.invalidate_count++] = (TegraNVDECRange){handle, 0, (uint32_t)bytes, 0};
    out->status = b->submit(b->ctx, &s, &w.fence) ? -2 : b->wait(b->ctx, &w) ? -2 : 0;
    if (!out->status)
        hevc_display_probe_compare((const uint8_t *)va + surfaceBytes, surfaceBytes, ref16, ref16 + lumaBytes, pitch16,
                                   rows, width, height, out);
    b->free(b->ctx, handle);
}
#endif
