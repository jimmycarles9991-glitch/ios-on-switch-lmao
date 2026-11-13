// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the VP9 half of AVD.videodecoder, against libvpx (test/VP9Trace.h, VP9Clip.h, made by vp9gen from
// libvpx 1.16.0). VP9NVDEC runs over a fake NVDEC whose "engine" writes, for each frame, the symbol counts libvpx's own
// decoder counted for it (NVDEC's counter layout). After every frame the decoder's header fields, its frame context
// and its four stored contexts must equal libvpx's (CRC-32 of the 2039 probability bytes): clip B (frame-parallel, no
// adaptation) proves what NVDEC is given for every frame, clip A (adaptation on) the counter mapping and the backward
// adaptation. Every submit must pass the kernel's checker (TegraNVDECCheck.h), which must also refuse each kind of bad
// VP9 submit derived from the first one.
//   vp9_host             prints "PASS vp9 ..." (vp9_host_test)
// Build: cc -O1 -o vp9_host vp9_host.c ../../AVD/VP9NVDEC.c ../../AVD/VP9Syntax.c ../../AVD/VP9Probs.c -lz
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../../AVD/VP9NVDEC.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "fake_engine.h"
#include "../VP9Clip.h"
#include "../VP9Trace.h"

_Static_assert(sizeof(VP9FrameContext) == 2039, "libvpx FRAME_CONTEXT probability bytes");

typedef struct {
    uint8_t *va[64];
    uint64_t size[64];
    const uint8_t *counts;  // unpacked trace counts (NVDEC layout)
    const VP9TraceFrame *frame;
    int jobs, refused, shown;
    unsigned recycled;
    bool haveFirst;
    TegraNVDECSubmit first;
    uint64_t firstAvail[TEGRA_NVDEC_MAX_ENTRIES];
    uint32_t counterHandle, counterOff;
    FakeEngine engine;
    const VP9TraceFrame *jobFrame[64];  // the trace frame each job decoded, by fence
    bool decoding;                      // inside vp9_decoder_decode, which began after job callStart
    uint32_t callStart;
    unsigned ownWaits;  // waits inside a decode call for a job that call submitted
} Fake;

static int fakeAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    Fake *f = ctx;
    for (uint32_t h = 1; h < 64; h++) {
        if (f->va[h])
            continue;
        if (!(f->va[h] = calloc(1, size)))
            return -1;
        f->size[h] = size;
        fake_engine_allocated(&f->engine, h);
        *handle = h;
        *va = f->va[h];
        return 0;
    }
    return -1;
}

static int switch_fake_recycle(void *ctx, uint32_t handle, uint64_t bytes, void **address)
{
    Fake *f = ctx;
    if (!handle || handle >= 64 || !f->va[handle] || bytes != f->size[handle])
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
}

static int fakeSubmit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    Fake *f = ctx;
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (e->value >= 64 || !f->va[e->value] || e->offset > f->size[e->value])
            return -1;
        avail[i] = f->size[e->value] - e->offset;
        if (e->method == NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET)
            f->counterHandle = e->value, f->counterOff = e->offset;
    }
    const char *why = tegra_nvdec_check(s, avail);
    if (why) {
        fprintf(stderr, "kernel check refuses VP9 job %d: %s\n", f->jobs, why);
        f->refused++;
        return -1;
    }
    // The frame's filter as NVDEC numbers it: the header's literal (0 smooth, 1 regular, 2 sharp, 3 bilinear, 4
    // switchable). On hardware, clip A's frame 7 (regular) sent in libvpx's numbering (0) decoded as libvpx decodes it
    // with the literal rewritten to smooth (luma CRC 0xbde1bb6d).
    static const int kLiteralOf[5] = {1, 0, 2, 3, 4};  // libvpx EIGHTTAP, SMOOTH, SHARP, BILINEAR, SWITCHABLE
    const nvdec_vp9_pic_s *p = (const nvdec_vp9_pic_s *)s->setup;
    if (f->frame && !f->frame->key && !f->frame->intra_only &&
        p->mcomp_filter_type != kLiteralOf[f->frame->interp_filter]) {
        fprintf(stderr, "VP9 job %d: mcomp_filter_type %d for libvpx filter %d, NVDEC wants %d\n", f->jobs,
                p->mcomp_filter_type, f->frame->interp_filter, kLiteralOf[f->frame->interp_filter]);
        f->refused++;
    }
    if (!f->haveFirst) {
        f->first = *s;
        memcpy(f->firstAvail, avail, sizeof(avail));
        f->haveFirst = true;
    }
    f->jobs++;
    *fence = fake_engine_submit(&f->engine, s);
    f->jobFrame[*fence % 64] = f->frame;
    return 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *w)
{
    Fake *f = ctx;
    f->ownWaits += f->decoding && (int32_t)(w->fence - f->callStart) > 0;
    fake_engine_wait(&f->engine, w->fence);
    uint8_t *counters = f->va[f->counterHandle] + f->counterOff;
    const VP9TraceFrame *frame = f->jobFrame[w->fence % 64];
    if (frame && frame->adapted)
        memcpy(counters, f->counts + frame->counts_at, sizeof(nvdec_vp9EntropyCounts_t));
    else
        memset(counters, 0, sizeof(nvdec_vp9EntropyCounts_t));
    return 0;
}

static void *fakeDecoded(void *ctx, const H264Picture *pic)
{
    Fake *f = ctx;
    if (pic->luma != f->va[pic->handle])
        ++f->refused;
    f->shown++;
    return malloc(1);
}

static void fakeEmit(void *ctx, void *item)
{
    (void)ctx, free(item);
}

static uint32_t crcOf(const void *p, size_t n)
{
    return (uint32_t)crc32(crc32(0, NULL, 0), p, (uInt)n);
}

// Runs one clip's packets frame by frame and compares with its trace. Returns the number of mismatches.
// settled: each frame's adaptation done before its contexts are compared; else the decoder adapts each frame when the
// next one starts, and only the last frame's contexts are compared.
static int runClip(const char *name, const uint32_t *sizes, int packets, const uint8_t *data,
                   const VP9TraceFrame *trace, int traced, const uint8_t *packed, size_t packedBytes,
                   uint32_t countsBytes, bool settled, int *shownOut)
{
    uint8_t *counts = malloc(countsBytes);
    uLongf unpacked = countsBytes;
    if (uncompress(counts, &unpacked, packed, packedBytes) != Z_OK || unpacked != countsBytes) {
        fprintf(stderr, "%s: trace counts do not unpack\n", name);
        return 1;
    }
    Fake fake = {.counts = counts};
    const H264Backend backend = {.ctx = &fake,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeEmit,
                                 .recycle = switch_fake_recycle,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    VP9Decoder *d = vp9_decoder_create(&backend);
    int bad = 0, record = 0;
    unsigned adapting = 0;  // frames whose next frame needs NVDEC's counts: one wait each
    if (vp9_decoder_configure(d, 512, 288))
        return 1;
    size_t at = 0;
    for (int p = 0; p < packets; p++) {
        const uint8_t *packet = data + at;
        at += sizes[p];
        size_t off[8], len[8];
        const unsigned frames = vp9_superframe_split(packet, sizes[p], off, len);
        for (unsigned i = 0; i < frames; i++, record++) {
            if (record >= traced) {
                fprintf(stderr, "%s: more frames than libvpx decoded\n", name);
                return bad + 1;
            }
            const VP9TraceFrame *t = &trace[record];
            fake.frame = t;
            adapting += !t->show_existing && !t->error_resilient && !t->frame_parallel;
            fake.decoding = true;
            fake.callStart = fake.engine.submitted;
            const int ret = vp9_decoder_decode(d, packet + off[i], len[i], (void *)1);
            fake.decoding = false;
            if (!settled)
                continue;
            vp9_decoder_settle(d);
            const VP9FrameHeader *h = vp9_decoder_last_header(d);
            const VP9State *s = vp9_decoder_state(d);
            const bool inter = !t->key && !t->intra_only;
            const int got[] = {h->key_frame,
                               h->show_frame,
                               h->intra_only,
                               h->error_resilient,
                               h->refresh_frame_context,
                               h->frame_parallel,
                               h->frame_context_idx,
                               h->base_qindex,
                               h->lf_level,
                               h->lf_sharpness,
                               s->seg.enabled,
                               s->seg.update_map,
                               s->seg.temporal_update,
                               h->log2_tile_cols,
                               h->log2_tile_rows,
                               h->tx_mode,
                               inter ? h->interp_filter : t->interp_filter,
                               inter ? h->reference_mode : t->reference_mode,
                               h->refresh_frame_flags};
            const int want[] = {t->key,           t->show,           t->intra_only,     t->error_resilient,
                                t->refresh_ctx,   t->frame_parallel, t->ctx_idx,        t->base_q,
                                t->lf_level,      t->lf_sharpness,   t->seg_enabled,    t->seg_update_map,
                                t->seg_temporal,  t->log2_tile_cols, t->log2_tile_rows, t->tx_mode,
                                t->interp_filter, t->reference_mode, t->refresh_flags};
            static const char *const kNames[] = {"key",           "show",           "intra_only",     "error_resilient",
                                                 "refresh_ctx",   "frame_parallel", "ctx_idx",        "base_q",
                                                 "lf_level",      "lf_sharpness",   "seg_enabled",    "seg_update_map",
                                                 "seg_temporal",  "log2_tile_cols", "log2_tile_rows", "tx_mode",
                                                 "interp_filter", "reference_mode", "refresh_flags"};
            if (ret < 0) {
                fprintf(stderr, "%s frame %d: decode failed (%d)\n", name, record, ret);
                bad++;
                continue;
            }
            for (unsigned k = 0; !t->show_existing && k < sizeof(got) / sizeof(got[0]); k++)
                if (got[k] != want[k]) {
                    fprintf(stderr, "%s frame %d: %s %d, libvpx %d\n", name, record, kNames[k], got[k], want[k]);
                    bad++;
                }
            const uint32_t fc = crcOf(vp9_decoder_frame_context(d), sizeof(VP9FrameContext));
            const uint32_t contexts = crcOf(s->contexts, sizeof(s->contexts));
            if (fc != t->fc_crc || contexts != t->contexts_crc) {
                fprintf(stderr, "%s frame %d (%s%s): context CRC %#010x stored %#010x, libvpx %#010x %#010x\n", name,
                        record,
                        t->key          ? "key"
                        : t->intra_only ? "intra"
                                        : "inter",
                        t->adapted ? ", adapted" : "", fc, contexts, t->fc_crc, t->contexts_crc);
                bad++;
            }
        }
    }
    if (record != traced) {
        fprintf(stderr, "%s: %d frames decoded, libvpx %d\n", name, record, traced);
        bad++;
    }
    if (!settled) {
        vp9_decoder_settle(d);
        const VP9TraceFrame *t = &trace[traced - 1];
        const uint32_t fc = crcOf(vp9_decoder_frame_context(d), sizeof(VP9FrameContext));
        const uint32_t contexts = crcOf(vp9_decoder_state(d)->contexts, sizeof(vp9_decoder_state(d)->contexts));
        if (fc != t->fc_crc || contexts != t->contexts_crc) {
            fprintf(stderr,
                    "%s, adapting at the next frame: last context CRC %#010x stored %#010x, libvpx %#010x "
                    "%#010x\n",
                    name, fc, contexts, t->fc_crc, t->contexts_crc);
            bad++;
        }
    }
    if (fake.ownWaits) {
        fprintf(stderr, "%s: %u decode calls waited for their own job\n", name, fake.ownWaits);
        bad++;
    }
    bad += fake.refused + (fake.recycled == 0);
    if (fake.engine.busyWrites || fake.engine.waits != adapting) {
        fprintf(stderr, "%s: %u CPU writes into a buffer a queued job uses, %u waits for %u adapting frames\n", name,
                fake.engine.busyWrites, fake.engine.waits, adapting);
        bad++;
    }
    *shownOut = fake.shown;
    // The checker's refusals, each from the clip's first (accepted) submit.
    if (fake.haveFirst && !strcmp(name, "A") && settled) {
        static const char *const kExpect[] = {NULL,
                                              "codec",
                                              "control params",
                                              "histogram",
                                              "encryption parameters",
                                              "v1 fields",
                                              "surface too small",
                                              "segmentation map",
                                              "motion vectors",
                                              "filter offset",
                                              "filter buffer",
                                              "pitch",
                                              "relocated method",
                                              "missing relocation",
                                              "bitstream",
                                              "dimensions",
                                              "setup size"};
        for (int c = 0; c < (int)(sizeof(kExpect) / sizeof(kExpect[0])); c++) {
            TegraNVDECSubmit s = fake.first;
            uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
            memcpy(avail, fake.firstAvail, sizeof(avail));
            nvdec_vp9_pic_s *p = (nvdec_vp9_pic_s *)s.setup;
            int find[3] = {-1, -1, -1};
            for (uint32_t i = 0; i < s.entry_count; i++) {
                if (s.entries[i].method == NVC5B0_SET_CONTROL_PARAMS)
                    find[0] = (int)i;
                if (s.entries[i].method == NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 12)
                    find[1] = (int)i;
                if (s.entries[i].method == NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET)
                    find[2] = (int)i;
            }
            switch (c) {
            case 1:
                s.codec = 2;  // VC-1, which NVDEC is not given
                break;
            case 2:
                s.entries[find[0]].value &= ~0x10u;
                break;
            case 3:
                p->enable_histogram = 1;
                break;
            case 4:
                p->wrapped_content_key[2] = 7;
                break;
            case 5:
                p->v1.bit_depth = 2;
                break;
            case 6:
                avail[find[1]] = (uint64_t)p->framestride[0] * 16;
                break;
            case 7:
                avail[find[2]] = 64;
                break;
            case 8:
                for (uint32_t i = 0; i < s.entry_count; i++)
                    if (s.entries[i].method == NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET)
                        avail[i] = 1024;
                break;
            case 9:
                p->Vp9BsdCtrlOffset += 1;
                break;
            case 10:
                for (uint32_t i = 0; i < s.entry_count; i++)
                    if (s.entries[i].method == NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET)
                        avail[i] = 4096;
                break;
            case 11:
                p->framestride[1] += 64;
                break;
            case 12:
                s.entries[find[1]].method = NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 16;
                break;
            case 13:
                for (uint32_t i = 0; i < s.entry_count; i++)
                    if (s.entries[i].method == NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET)
                        s.entries[i].method = NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET;  // counters missing, MV read twice
                break;
            case 14:
                p->stream_len = 0x40000000;
                break;
            case 15:
                p->width = 5000;
                break;
            case 16:
                s.setup_bytes = sizeof(nvdec_h264_pic_s);
                break;
            }
            const char *why = tegra_nvdec_check(&s, avail);
            const bool ok = c == 13 ? why && (!strcmp(why, "repeated relocation") || !strcmp(why, "missing relocation"))
                                    : (why == NULL) == (kExpect[c] == NULL) && (!why || !strcmp(why, kExpect[c]));
            if (!ok) {
                fprintf(stderr, "VP9 refusal case %d: checker said \"%s\", expected \"%s\"\n", c,
                        why ? why : "accepted", kExpect[c] ? kExpect[c] : "accepted");
                bad++;
            }
        }
    }
    vp9_decoder_destroy(d);
    free(counts);
    return bad;
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    int shownA = 0, shownB = 0;
    int shownLate = 0;
    const int badA = runClip("A", kVP9ClipSizes, kVP9ClipPackets, kVP9ClipData, kTraceAFrames,
                             (int)(sizeof(kTraceAFrames) / sizeof(kTraceAFrames[0])), kTraceACounts,
                             sizeof(kTraceACounts), kTraceACountsBytes, true, &shownA);
    const int badLate = runClip("A", kVP9ClipSizes, kVP9ClipPackets, kVP9ClipData, kTraceAFrames,
                                (int)(sizeof(kTraceAFrames) / sizeof(kTraceAFrames[0])), kTraceACounts,
                                sizeof(kTraceACounts), kTraceACountsBytes, false, &shownLate);
    const int badB = runClip("B", kTraceBSizes, (int)(sizeof(kTraceBSizes) / sizeof(kTraceBSizes[0])), kTraceBData,
                             kTraceBFrames, (int)(sizeof(kTraceBFrames) / sizeof(kTraceBFrames[0])), kTraceBCounts,
                             sizeof(kTraceBCounts), kTraceBCountsBytes, true, &shownB);
    const bool pass = !badA && !badB && !badLate && shownA == kVP9ClipShown && shownLate == kVP9ClipShown;
    printf("%s vp9: clip A %d frames (%d shown), clip B %d frames: headers, contexts and adaptation equal libvpx's, "
           "every submit accepted, refusals as expected (%d + %d mismatches); clip A adapting each frame at the next "
           "one's start, no decode waiting for its own job (%d mismatches)\n",
           pass ? "PASS" : "FAIL", (int)(sizeof(kTraceAFrames) / sizeof(kTraceAFrames[0])), shownA,
           (int)(sizeof(kTraceBFrames) / sizeof(kTraceBFrames[0])), badA, badB, badLate);
    return pass ? 0 : 1;
}
