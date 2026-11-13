// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of HEVCSyntax.c and HEVCNVDEC.c against FFmpeg (test/HEVCTrace.h, test/HEVCClip.h from test/hevcgen):
//   * every slice segment of clips A, V, T, K parses to FFmpeg's NAL type, slice type, POC LSBs and sw_hdr_skip_length;
//   * the scaling list defaults equal FFmpeg's raster tables;
//   * HEVCNVDEC decodes every clip over a fake engine: each submit passes the kernel's own checker
//     (TegraNVDECCheck.h), pictures come out in FFmpeg's output order, every reference slot names a picture still
//     in the DPB, and refused submits (encryption, histogram, ecdma, v1-v3, sizes, output modes) are refused.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/HEVCNVDEC.h"
#include "../../AVD/HEVCSyntax.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "fake_engine.h"
#include "../HEVCClip.h"
#include "../HEVCTrace.h"

static int gFailures;
#define CHECK(cond, ...)                               \
    do {                                               \
        if (!(cond)) {                                 \
            printf("FAIL %s:%d ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
            gFailures++;                               \
        }                                              \
    } while (0)

typedef struct {
    const uint8_t *data;
    size_t size;
} Span;

// Annex-B NAL units of a clip, in order.
static int nalUnits(const uint8_t *data, size_t size, Span *out, int max)
{
    int n = 0;
    for (size_t i = 0; i + 3 <= size;) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) {
            i++;
            continue;
        }
        if (n) {
            size_t e = i;
            while (e && !data[e - 1])
                e--;
            out[n - 1].size = (size_t)(data + e - out[n - 1].data);
        }
        if (n == max)
            return -1;
        out[n++].data = data + i + 3;
        i += 3;
    }
    if (n)
        out[n - 1].size = (size_t)(data + size - out[n - 1].data);
    return n;
}

static void testSyntax(void)
{
    const uint8_t *data[4] = {kHEVCClips[0].data, kHEVCClips[1].data, kHEVCClips[2].data, kHEVCClipKData};
    const size_t sizes[4] = {kHEVCClips[0].size, kHEVCClips[1].size, kHEVCClips[2].size, sizeof(kHEVCClipKData)};
    static Span nal[512];
    static HEVCParams params;
    size_t t = 0, explicitRps = 0, nonzero = 0;
    for (int c = 0; c < 4; c++) {
        memset(&params, 0, sizeof(params));
        const int n = nalUnits(data[c], sizes[c], nal, 512);
        int au = -1;
        for (int i = 0; i < n; i++) {
            const int type = (nal[i].data[0] >> 1) & 0x3f;
            if (type == HEVC_NAL_SPS)
                CHECK(!hevc_parse_sps(&params, nal[i].data, nal[i].size), "clip %d SPS", c);
            if (type == HEVC_NAL_PPS)
                CHECK(!hevc_parse_pps(&params, nal[i].data, nal[i].size), "clip %d PPS", c);
            if (type >= 32)
                continue;
            HEVCSliceHeader h;
            const int ret = hevc_parse_slice_header(&params, nal[i].data, nal[i].size, &h);
            if (h.first_slice)
                au++;
            CHECK(t < sizeof(kHEVCSlices) / sizeof(kHEVCSlices[0]), "more slices than FFmpeg saw");
            if (t >= sizeof(kHEVCSlices) / sizeof(kHEVCSlices[0]))
                return;
            const HEVCSliceTruth *e = &kHEVCSlices[t++];
            CHECK(!ret, "clip %d au %d slice parse", c, au);
            CHECK(e->clip == c && e->au == au && e->nal_type == h.nal_type && e->first == h.first_slice &&
                      e->dependent == h.dependent,
                  "clip %d au %d: nal %d/%d first %d/%d", c, au, h.nal_type, e->nal_type, h.first_slice, e->first);
            if (h.dependent)
                continue;
            CHECK(e->slice_type == h.slice_type && e->poc_lsb == h.poc_lsb && e->skip == h.skip_bits,
                  "clip %d au %d: slice_type %d/%d poc_lsb %d/%d skip %d/%d (FFmpeg's second)", c, au, h.slice_type,
                  e->slice_type, h.poc_lsb, e->poc_lsb, h.skip_bits, e->skip);
            explicitRps += !h.st_rps_from_sps && h.st_rps_bits;
            nonzero += h.skip_bits != 0;
        }
    }
    CHECK(t == sizeof(kHEVCSlices) / sizeof(kHEVCSlices[0]), "%zu of %zu slices parsed", t,
          sizeof(kHEVCSlices) / sizeof(kHEVCSlices[0]));
    printf("syntax: %zu slice segments match FFmpeg (%zu with their own RPS, %zu with a non-zero skip length)\n", t,
           explicitRps, nonzero);
    HEVCScalingList sl;
    hevc_default_scaling_list(&sl);
    static const uint8_t kRow0Intra[8] = {16, 16, 16, 16, 17, 18, 21, 24}, kLastInter = 91;
    CHECK(!memcmp(sl.sl[1][0], kRow0Intra, 8) && sl.sl[3][3][63] == kLastInter && sl.dc[1][3] == 16,
          "default scaling lists");
}

enum {
    MAX_BUFFERS = 64
};

typedef struct {
    uint8_t *va[MAX_BUFFERS + 1];
    uint64_t bytes[MAX_BUFFERS + 1];
    int out[64], outCount, discarded, jobs;
    unsigned recycled;
    uint32_t pitch;  // the last picture handed out: bytes a row, as present and the CPU copy read it
    TegraNVDECSubmit last;
    FakeEngine engine;
} Fake;

static int fakeAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    Fake *f = ctx;
    for (uint32_t h = 1; h <= MAX_BUFFERS; h++) {
        if (f->va[h])
            continue;
        f->va[h] = calloc(1, size);
        f->bytes[h] = size;
        fake_engine_allocated(&f->engine, h);
        *handle = h;
        *va = f->va[h];
        return f->va[h] ? 0 : -1;
    }
    return -1;
}

static int switch_fake_recycle(void *ctx, uint32_t handle, uint64_t bytes, void **address)
{
    Fake *f = ctx;
    if (!handle || handle >= MAX_BUFFERS + 1 || !f->va[handle] || bytes != f->bytes[handle])
        return -1;
    void *next = calloc(1, bytes);
    if (!next)
        return -1;
    free(f->va[handle]);
    f->va[handle] = next;
    *address = next;
    ++f->recycled;
    return 0;
}

static void fakeFree(void *ctx, uint32_t handle)
{
    Fake *f = ctx;
    free(f->va[handle]);
    f->va[handle] = NULL;
    f->bytes[handle] = 0;
}

static const char *fakeCheck(Fake *f, const TegraNVDECSubmit *s)
{
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (e->value > MAX_BUFFERS || !f->va[e->value] || e->offset > f->bytes[e->value])
            return "bad relocation";
        avail[i] = f->bytes[e->value] - e->offset;
    }
    return tegra_nvdec_check(s, avail);
}

static int fakeSubmit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    Fake *f = ctx;
    const char *why = fakeCheck(f, s);
    CHECK(!why, "job %d refused by the kernel checker: %s", f->jobs, why ? why : "");
    f->last = *s;
    f->jobs++;
    *fence = fake_engine_submit(&f->engine, s);
    return why ? -1 : 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *w)
{
    fake_engine_wait(&((Fake *)ctx)->engine, w->fence);
    return 0;
}

static void *fakeDecoded(void *ctx, const H264Picture *p)
{
    Fake *f = ctx;
    CHECK(p->luma == f->va[p->handle], "recycled picture retains old CPU address");
    f->pitch = p->pitch;
    return (void *)(intptr_t)((intptr_t)p->token + 1);
}

static void fakeEmit(void *ctx, void *item)
{
    Fake *f = ctx;
    if (f->outCount < 64)
        f->out[f->outCount++] = (int)(intptr_t)item - 1;
}

static void fakeDiscard(void *ctx, void *item)
{
    (void)item;
    ((Fake *)ctx)->discarded++;
}

// An Annex-B clip's access units as 4-byte length-prefixed samples.
static int samples(const uint8_t *data, size_t size, uint8_t *buffer, uint8_t **sample, size_t *sizes, int max)
{
    static Span nal[1024];
    const int n = nalUnits(data, size, nal, 1024);
    int count = 0;
    size_t used = 0;
    bool pendingStart = true;
    for (int i = 0; i < n; i++) {
        const int type = (nal[i].data[0] >> 1) & 0x3f;
        const bool vcl = type < 32, starts = vcl && (nal[i].data[2] & 0x80);
        if ((starts && !pendingStart) || !count) {
            if (count == max)
                return -1;
            sample[count] = buffer + used;
            sizes[count++] = 0;
        }
        pendingStart = !vcl && (pendingStart || starts);
        if (vcl)
            pendingStart = false;
        uint8_t *p = buffer + used;
        p[0] = (uint8_t)(nal[i].size >> 24), p[1] = (uint8_t)(nal[i].size >> 16), p[2] = (uint8_t)(nal[i].size >> 8);
        p[3] = (uint8_t)nal[i].size;
        memcpy(p + 4, nal[i].data, nal[i].size);
        used += nal[i].size + 4;
        sizes[count - 1] += nal[i].size + 4;
    }
    return count;
}

static HEVCDecoder *decodeClip(Fake *f, const uint8_t *data, size_t size, int *aus, bool tenBit)
{
    static uint8_t buffer[64 * 1024];
    uint8_t *sample[64];
    size_t sizes[64];
    const H264Backend backend = {.ctx = f,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeDiscard,
                                 .recycle = switch_fake_recycle,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    HEVCDecoder *d = hevc_decoder_create(&backend);
    hevc_decoder_allow_10bit(d, tenBit);
    *aus = samples(data, size, buffer, sample, sizes, 64);
    for (int i = 0; i < *aus; i++) {
        const int ret = hevc_decoder_decode(d, sample[i], sizes[i], 4, (void *)(intptr_t)i, true);
        CHECK(ret == 1, "access unit %d: hevc_decoder_decode %d", i, ret);
    }
    hevc_decoder_flush(d);
    CHECK(f->recycled != 0, "output allocations never recycled");
    return d;
}

static void testDecode(void)
{
    for (int c = 0; c < 3; c++) {
        const HEVCClip *clip = &kHEVCClips[c];
        Fake f;
        memset(&f, 0, sizeof(f));
        int aus = 0;
        HEVCDecoder *d = decodeClip(&f, clip->data, clip->size, &aus, clip->depth == 10);
        CHECK(aus == clip->aus && f.jobs == clip->aus, "clip %c: %d access units, %d jobs", clip->name, aus, f.jobs);
        CHECK(f.outCount == clip->shown && !f.discarded, "clip %c: %d out, %d discarded", clip->name, f.outCount,
              f.discarded);
        bool order = true;
        for (int k = 0; k < f.outCount; k++)
            order &= clip->display[f.out[k]] == k;
        CHECK(order, "clip %c: output order differs from FFmpeg's", clip->name);
        CHECK(!hevc_decoder_missing_refs(d), "clip %c: %u references missing", clip->name,
              hevc_decoder_missing_refs(d));
        CHECK(!f.engine.waits && !f.engine.busyWrites, "clip %c: %u waits, %u CPU writes into a buffer a job uses",
              clip->name, f.engine.waits, f.engine.busyWrites);
        // framestride counts samples and mode 1 writes two bytes each (averne's nvtegra_hevc.c): the engine's rows
        // must be the rows the picture is read with.
        const nvdec_hevc_pic_s *setup = (const nvdec_hevc_pic_s *)f.last.setup;
        const uint32_t engineRow = setup->framestride[0] * (setup->disp_output_mode == 1 ? 2u : 1u);
        CHECK(engineRow == f.pitch, "clip %c: the engine writes %u-byte rows, the picture is read at %u", clip->name,
              engineRow, f.pitch);
        // A Main 10 clip is refused while Main 10 is not proven.
        if (clip->depth == 10) {
            Fake g;
            memset(&g, 0, sizeof(g));
            const H264Backend backend = {.ctx = &g,
                                         .alloc = fakeAlloc,
                                         .free = fakeFree,
                                         .submit = fakeSubmit,
                                         .wait = fakeWait,
                                         .decoded = fakeDecoded,
                                         .emit = fakeEmit,
                                         .discard = fakeDiscard,
                                         .recycle = switch_fake_recycle,
                                         .inputs = FAKE_ENGINE_DEPTH + 1};
            HEVCDecoder *e = hevc_decoder_create(&backend);
            static uint8_t buffer[64 * 1024];
            uint8_t *sample[64];
            size_t sizes[64];
            samples(clip->data, clip->size, buffer, sample, sizes, 64);
            CHECK(hevc_decoder_decode(e, sample[0], sizes[0], 4, NULL, true) == -1 && !g.jobs,
                  "Main 10 taken while off");
            hevc_decoder_destroy(e);
        }
        printf("decode: clip %c %dx%d %d-bit, %d access units, every job accepted by the kernel checker, %d pictures "
               "out in FFmpeg's order\n",
               clip->name, clip->width, clip->height, clip->depth, aus, f.outCount);
        if (c == 0) {  // the kernel checker refuses each kind of bad HEVC submit
            const TegraNVDECSubmit good = f.last;
            CHECK(!fakeCheck(&f, &good), "the last submit");
            static const char *const kBad[] = {"encryption", "histogram", "ecdma",  "v3",     "16-bit output",
                                               "pitch",      "rows",      "motion", "filter", "stream",
                                               "slot",       "timer",     "size",   "surface"};
            const size_t kinds = sizeof(kBad) / sizeof(kBad[0]);
            for (size_t i = 0; i < kinds; i++) {
                TegraNVDECSubmit s = good;
                nvdec_hevc_pic_s *p = (nvdec_hevc_pic_s *)s.setup;
                switch (i) {
                case 0:
                    p->enable_encryption = 1;
                    break;
                case 1:
                    p->enable_histogram = 1;
                    break;
                case 2:
                    p->ecdma_cfg.ecdma_enable = 1;
                    break;
                case 3:
                    p->v3.slice_decoding_enable = 1;
                    break;
                case 4:
                    p->disp_output_mode = 1;
                    break;
                case 5:
                    p->framestride[0] = p->framestride[1] = 256;
                    break;
                case 6:
                    p->pic_height_in_luma_samples = 4000;
                    break;
                case 7:
                    p->colMvBuffersize /= 2;
                    break;
                case 8:
                    p->HevcBsdCtrlOffset *= 4;
                    break;
                case 9:
                    p->stream_len = 0x7fffffff;
                    break;
                case 10:
                    p->curr_pic_idx = 16;
                    break;
                case 11:
                    p->gptimer_timeout_value = 1u << 28;
                    break;
                case 12:
                    p->pic_width_in_luma_samples = 64;
                    break;
                default:  // slot 3's chroma 256 bytes before its surface's end
                    for (uint32_t k = 0; k < s.entry_count; k++)
                        if (s.entries[k].method == NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 12)
                            s.entries[k].offset = (uint32_t)(f.bytes[s.entries[k].value] - 256);
                }
                CHECK(fakeCheck(&f, &s), "the checker took a bad HEVC submit (%s)", kBad[i]);
            }
            printf("checker: %zu kinds of bad HEVC submit refused\n", kinds);
        }
        if (clip->depth == 10) {  // Main 10: 16-bit samples, so 8-bit-sized surfaces and the 10-to-8 modes are refused
            const TegraNVDECSubmit good = f.last;
            const nvdec_hevc_pic_s *q = (const nvdec_hevc_pic_s *)good.setup;
            const uint32_t columns = (clip->width + 63u) & ~63u, rows = (clip->height + 63u) & ~63u;
            CHECK(q->disp_output_mode == 1 && q->framestride[0] == columns && f.pitch == 2 * columns,
                  "clip %c: mode %u, framestride %u, picture pitch %u", clip->name, q->disp_output_mode,
                  q->framestride[0], f.pitch);
            CHECK(!fakeCheck(&f, &good), "clip %c: the last submit", clip->name);
            TegraNVDECSubmit narrow = good, bytes = good, small = good, eight = good, deblock = good;
            nvdec_hevc_pic_s *p = (nvdec_hevc_pic_s *)narrow.setup;
            p->framestride[0] = p->framestride[1] = columns / 2;
            p = (nvdec_hevc_pic_s *)bytes.setup;  // a byte pitch: the engine would write rows twice the surface's
            p->framestride[0] = p->framestride[1] = 2 * columns;
            for (uint32_t k = 0; k < small.entry_count; k++)  // the current slot's chroma plane sized for 8 bits
                if (small.entries[k].method == NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * q->curr_pic_idx)
                    small.entries[k].offset = (uint32_t)(f.bytes[small.entries[k].value] - columns * rows / 2);
            ((nvdec_hevc_pic_s *)eight.setup)->disp_output_mode = 2;
            ((nvdec_hevc_pic_s *)deblock.setup)->HevcSaoBufferOffset = 480 * rows / 256;
            CHECK(fakeCheck(&f, &narrow) && fakeCheck(&f, &bytes) && fakeCheck(&f, &small) && fakeCheck(&f, &deblock),
                  "clip %c: the checker took a Main 10 submit the engine would write past", clip->name);
            CHECK(!fakeCheck(&f, &eight), "clip %c: mode 2 into 16-bit-sized pictures refused", clip->name);
            printf("checker: Main 10 at %u samples a row (%u bytes); byte strides, 8-bit-sized planes and filter rows "
                   "refused\n",
                   columns, 2 * columns);
        }
        hevc_decoder_destroy(d);
        for (int h = 1; h <= MAX_BUFFERS; h++)
            CHECK(!f.va[h], "clip %c: buffer %d not freed", clip->name, h);
    }
}

static size_t gobOffset(uint32_t x, uint32_t y, uint32_t pitch)
{
    return (size_t)(y / 16) * (pitch / 64) * 1024 + (x / 64) * 1024 + ((y % 16) / 8) * 512 + ((x % 64) / 32) * 256 +
           ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

// Without reordering every picture goes out before its decode call returns, as Apple's HEVC decoders do on a clip with
// B pictures: none is held for display order.
static void testDecodeOrder(void)
{
    const HEVCClip *clip = &kHEVCClips[0];
    Fake f;
    memset(&f, 0, sizeof(f));
    const H264Backend backend = {.ctx = &f,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeDiscard,
                                 .recycle = switch_fake_recycle,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    HEVCDecoder *d = hevc_decoder_create(&backend);
    static uint8_t buffer[64 * 1024];
    uint8_t *sample[64];
    size_t sizes[64];
    const int aus = samples(clip->data, clip->size, buffer, sample, sizes, 64);
    bool held = false, reordered = false;
    for (int i = 0; i < aus; i++) {
        CHECK(hevc_decoder_decode(d, sample[i], sizes[i], 4, (void *)(intptr_t)i, false) == 1,
              "decode order: access unit %d", i);
        held |= f.outCount != i + 1;
        reordered |= clip->display[i] != i;
    }
    hevc_decoder_flush(d);
    bool order = f.outCount == aus;
    for (int k = 0; order && k < f.outCount; k++)
        order = f.out[k] == k;
    CHECK(reordered, "decode order: clip %c has no B pictures to hold", clip->name);
    CHECK(!held && order, "decode order: clip %c held a picture past its decode (%d out of %d)", clip->name, f.outCount,
          aus);
    CHECK(!hevc_decoder_missing_refs(d), "decode order: %u references missing", hevc_decoder_missing_refs(d));
    hevc_decoder_destroy(d);
    printf("decode order: clip %c, %d pictures each out before its decode call returned\n", clip->name, aus);
}

// HEVCDisplayProbe's reading: a display buffer written as pitch-linear 8-bit truncation of a block-linear P010 picture
// is found as that layout, exact; a buffer left poisoned is "not written".
static void testDisplayProbeCompare(void)
{
    enum {
        W = 64,
        H = 32,
        PITCH16 = 128
    };

    static uint8_t ref[PITCH16 * H], display[PITCH16 * H * 3 / 2];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const uint16_t sample = (uint16_t)(((x * 7 + y * 13) % 1024) << 6);
            ref[gobOffset(2 * x, y, PITCH16)] = (uint8_t)sample;
            ref[gobOffset(2 * x + 1, y, PITCH16)] = (uint8_t)(sample >> 8);
        }
    HEVCDisplayProbeResult r = {0};
    memset(display, HEVC_DISPLAY_POISON, sizeof(display));
    hevc_display_probe_compare(display, sizeof(display), ref, PITCH16, H, W, H, &r);
    CHECK(r.layout == HEVC_DISPLAY_NOT_WRITTEN, "display probe: a poisoned buffer read as layout %u", r.layout);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            display[y * (PITCH16 / 2) + x] = (uint8_t)(((x * 7 + y * 13) % 1024) >> 2);
    hevc_display_probe_compare(display, sizeof(display), ref, PITCH16, H, W, H, &r);
    CHECK(r.layout == HEVC_DISPLAY_PITCH_8 && r.max_diff == 0 && r.trunc_milli == 1000,
          "display probe: layout %u max %u trunc %u", r.layout, r.max_diff, r.trunc_milli);
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    testSyntax();
    testDecode();
    testDecodeOrder();
    testDisplayProbeCompare();
    if (gFailures) {
        printf("FAIL hevc: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS hevc\n");
    return 0;
}
