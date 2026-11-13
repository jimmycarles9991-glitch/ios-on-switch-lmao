// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the MPEG-1/2 half of AVD.videodecoder, against FFmpeg (test/MPEG2Trace.h, MPEG2Clip.h, made by
// mpeg2gen from FFmpeg 8.1.2's trace_headers). MPEG2Syntax/MPEG2NVDEC run over a fake NVDEC; per picture of an MPEG-2
// Main clip (B-pictures, loaded matrices, alternate scan, intra VLC, non-linear quantiser scale, 10-bit intra DC), an
// interlaced one and an MPEG-1 one, what NVDEC is given must equal what FFmpeg parsed: the coding parameters, the
// f_codes, the slice count and offsets, the slices' bytes with averne's end sequence, the matrices, the codec type
// (MPEG-1 or MPEG-2), the macroblock rows (field pairs for an interlaced sequence) and the slots the picture's type
// names; the frame out must be the one decoded, tagged with its field order. Field pictures (FFmpeg encodes none) are
// built here: a pair goes into one surface, the second with secondfield set, and out once; a lone field is dropped. A
// picture whose references are missing is skipped. Every submit must pass the kernel's checker, which must refuse each
// kind of bad MPEG-2 submit derived from the first one; 4:2:2 and oversized sequences are refused.
//   mpeg2_host           prints "PASS mpeg2 ..." (mpeg2_host_test)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/AVDBits.h"
#include "../../AVD/MPEG2NVDEC.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "fake_engine.h"
#include "../MPEG2Clip.h"
#include "../MPEG2Trace.h"

typedef struct {
    uint8_t *va[64];
    uint64_t size[64];
    int submits, refused, outputs;
    TegraNVDECSubmit last, first;
    uint64_t firstAvail[TEGRA_NVDEC_MAX_ENTRIES];
    bool haveFirst;
    H264Picture out;
    FakeEngine engine;
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

static void fakeFree(void *ctx, uint32_t handle)
{
    Fake *f = ctx;
    free(f->va[handle]);
    f->va[handle] = NULL;
}

static const char *check(const Fake *f, const TegraNVDECSubmit *s, uint64_t *avail)
{
    memset(avail, 0, sizeof(uint64_t) * TEGRA_NVDEC_MAX_ENTRIES);
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (e->value >= 64 || !f->va[e->value] || e->offset > f->size[e->value])
            return "relocation outside a buffer";
        avail[i] = f->size[e->value] - e->offset;
    }
    return tegra_nvdec_check(s, avail);
}

static int fakeSubmit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    Fake *f = ctx;
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
    const char *why = check(f, s, avail);
    if (why) {
        fprintf(stderr, "kernel check refuses MPEG-2 job %d: %s\n", f->submits, why);
        f->refused++;
        return -1;
    }
    if (!f->haveFirst) {
        f->first = *s;
        memcpy(f->firstAvail, avail, sizeof(avail));
        f->haveFirst = true;
    }
    f->last = *s;
    f->submits++;
    *fence = fake_engine_submit(&f->engine, s);
    return 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *wait)
{
    fake_engine_wait(&((Fake *)ctx)->engine, wait->fence);
    return 0;
}

static void *fakeDecoded(void *ctx, const H264Picture *picture)
{
    Fake *f = ctx;
    f->outputs++;
    f->out = *picture;
    return NULL;
}

static uint32_t reloc(const TegraNVDECSubmit *s, uint16_t method, uint32_t *offset)
{
    for (uint32_t i = 0; i < s->entry_count; i++) {
        if (s->entries[i].method == method && s->entries[i].kind == TEGRA_NVDEC_ENTRY_RELOC) {
            if (offset)
                *offset = s->entries[i].offset;
            return s->entries[i].value;
        }
    }
    return 0;
}

static uint32_t value(const TegraNVDECSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method && s->entries[i].kind == TEGRA_NVDEC_ENTRY_VALUE)
            return s->entries[i].value;
    return UINT32_MAX;
}

static uint32_t slot(const TegraNVDECSubmit *s, int i)
{
    return reloc(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), NULL);
}

static MPEG2Decoder *create(Fake *f)
{
    const H264Backend backend = {.ctx = f,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    return mpeg2_decoder_create(&backend);
}

static const uint8_t kEnd[16] = {0, 0, 1, 0xb7, 0, 0, 0, 0, 0, 0, 1, 0xb7, 0, 0, 0, 0};

// The slices of a sample, start codes included, back to back with their offsets, as NVDEC must be given them.
static bool slicesMatch(const Fake *f, const TegraNVDECSubmit *s, const uint8_t *sample, size_t size)
{
    uint32_t bitsOff = 0, slicesOff = 0;
    const uint32_t input = reloc(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, &bitsOff);
    reloc(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, &slicesOff);
    const uint8_t *bits = f->va[input] + bitsOff;
    const uint32_t *offsets = (const uint32_t *)(f->va[input] + slicesOff);
    uint32_t at = 0, n = 0;
    for (size_t pos = avd_next_start_code(sample, size, 0); pos + 4 <= size;) {
        const size_t next = avd_next_start_code(sample, size, pos + 4);
        if (sample[pos + 3] >= 1 && sample[pos + 3] <= 0xaf) {
            if (offsets[n++] != at || memcmp(bits + at, sample + pos, next - pos))
                return false;
            at += (uint32_t)(next - pos);
        }
        pos = next;
    }
    const nvdec_mpeg2_pic_s *p = (const nvdec_mpeg2_pic_s *)s->setup;
    return offsets[n] == at && n == p->slice_count && p->stream_len == at + 16 && !memcmp(bits + at, kEnd, 16);
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t config;
    const size_t *pictures;
    int count;
    const int *size, *intra, *inter;
    const MPEG2TracePicture *trace;
} Clip;

static bool matrices(const MPEG2Sequence *seq, const int *intra, const int *inter)
{
    static const uint8_t kDefaultIntra[64] = {
        8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
        34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
        35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
    };
    for (int i = 0; i < 64; i++) {
        if (seq->intra_matrix[i] != (intra[i] < 0 ? kDefaultIntra[i] : intra[i]) ||
            seq->inter_matrix[i] != (inter[i] < 0 ? 16 : inter[i]))
            return false;
    }
    return true;
}

// Every picture of a clip against FFmpeg's trace: 0, or the failing picture + 1.
static int runClip(const Clip *c, Fake *f)
{
    MPEG2Sequence seq = {0};
    const bool mpeg2 = c->size[3];
    if (mpeg2_parse_config(&seq, c->data, c->config) != 1 || seq.width != c->size[0] || seq.height != c->size[1] ||
        seq.progressive_sequence != c->size[2] || seq.mpeg2 != mpeg2 || !matrices(&seq, c->intra, c->inter)) {
        fprintf(stderr, "%s: sequence %ux%u progressive %d MPEG-2 %d\n", c->name, seq.width, seq.height,
                seq.progressive_sequence, seq.mpeg2);
        return 1;
    }
    MPEG2Decoder *d = create(f);
    if (mpeg2_decoder_set_sequence(d, &seq))
        return 1;
    const uint32_t hmbs = c->size[2] ? (c->size[1] + 15u) / 16 : (c->size[1] + 31u) / 32 * 2;
    uint32_t refs[2] = {0, 0};
    for (int i = 0; i < c->count; i++) {
        const int *t = c->trace[i].field;
        const uint8_t *sample = c->data + c->pictures[i];
        const size_t size = c->pictures[i + 1] - c->pictures[i];
        const int submits = f->submits;
        if (mpeg2_decoder_decode(d, sample, size, (void *)(uintptr_t)(i + 1)) != 1 || f->submits != submits + 1) {
            fprintf(stderr, "%s picture %d: not decoded\n", c->name, i);
            return i + 1;
        }
        const TegraNVDECSubmit *s = &f->last;
        const nvdec_mpeg2_pic_s *p = (const nvdec_mpeg2_pic_s *)s->setup;
        bool ok = p->picture_coding_type == t[0] && p->intra_dc_precision == t[7] && p->picture_structure == t[8] &&
                  p->top_field_first == (unsigned)t[9] && p->frame_pred_frame_dct == t[10] &&
                  p->concealment_motion_vectors == t[11] && p->q_scale_type == (unsigned)t[12] &&
                  p->intra_vlc_format == t[13] && p->alternate_scan == t[14] && !p->secondfield &&
                  p->full_pel_fwd_vector == (unsigned)(mpeg2 ? 0 : t[5]) &&
                  p->full_pel_bwd_vector == (unsigned)(mpeg2 ? 0 : t[6]) &&
                  p->slice_count == (unsigned)c->trace[i].slices && p->PicWidthInMbs == (c->size[0] + 15) / 16 &&
                  p->FrameHeightInMbs == hmbs && !memcmp(p->quant_mat_8x8intra, seq.intra_matrix, 64) &&
                  !memcmp(p->quant_mat_8x8nonintra, seq.inter_matrix, 64) &&
                  (value(s, NVC5B0_SET_CONTROL_PARAMS) & 0xf) == (mpeg2 ? 1u : 0u);
        for (int k = 0; k < 4; k++)
            ok = ok && (t[1 + k] < 0 || p->f_code[k] == t[1 + k]);
        ok = ok && slicesMatch(f, s, sample, size);
        const uint32_t current = slot(s, 0);
        ok = ok &&
             (t[0] == 1   ? slot(s, 1) == current && slot(s, 2) == current
              : t[0] == 2 ? slot(s, 1) == refs[1] && slot(s, 2) == current && current != refs[1]
                          : slot(s, 1) == refs[0] && slot(s, 2) == refs[1] && current != refs[0] && current != refs[1]);
        const uint8_t fields = !mpeg2 || t[15] ? AVD_FIELDS_PROGRESSIVE
                               : t[9]          ? AVD_FIELDS_TOP_FIRST
                                               : AVD_FIELDS_BOTTOM_FIRST;
        ok = ok && f->out.handle == current && f->out.width == (uint32_t)c->size[0] &&
             f->out.height == (uint32_t)c->size[1] && f->out.token == (void *)(uintptr_t)(i + 1) &&
             f->out.fields == fields;
        if (!ok) {
            fprintf(stderr, "%s picture %d (type %d): structure %d dc %d fc %d %d %d %d slices %u fields %u\n", c->name,
                    i, t[0], p->picture_structure, p->intra_dc_precision, p->f_code[0], p->f_code[1], p->f_code[2],
                    p->f_code[3], p->slice_count, f->out.fields);
            return i + 1;
        }
        if (t[0] != 3) {
            refs[0] = refs[1];
            refs[1] = current;
        }
    }
    mpeg2_decoder_destroy(d);
    return 0;
}

// A picture header and coding extension of an MPEG-2 field picture (or frame picture), then one slice.
static size_t fieldPicture(uint8_t *out, uint8_t type, uint8_t structure)
{
    size_t n = 0;
    const uint8_t header[] = {0, 0, 1, 0, 0, (uint8_t)(type << 3), 0, 0};  // temporal_reference 0, vbv_delay 0
    memcpy(out + n, header, sizeof(header));
    n += sizeof(header);
    if (type != 1) {  // full_pel 0 and f_code 7 (MPEG-2) per direction, then extra_bit_picture 0
        out[n - 1] = 0x03;
        out[n++] = 0x80 | (type == 3 ? 0x38 : 0);
    }
    // f_codes 2,2 / 2,2 (15s for an I-picture), intra_dc_precision 0, the structure, top_field_first 0,
    // frame_pred_frame_dct 0, concealment 0, q_scale_type 0, intra_vlc 0, alternate_scan 0, rff 0, 420 type 0,
    // progressive_frame 0, composite 0
    const uint8_t fc = type == 1 ? 0xff : 0x22;
    const uint8_t ext[] = {0,
                           0,
                           1,
                           0xb5,
                           (uint8_t)(0x80 | (fc >> 4)),
                           (uint8_t)(fc << 4 | (fc >> 4)),
                           (uint8_t)(fc << 4 | (structure & 3)),
                           0x00,
                           0x00};
    memcpy(out + n, ext, sizeof(ext));
    n += sizeof(ext);
    const uint8_t slice[] = {0, 0, 1, 0x01, 0x30, 0x55, 0xaa, 0x55, 0xaa, 0x80};
    memcpy(out + n, slice, sizeof(slice));
    return n + sizeof(slice);
}

// An I/P field pair, a B field pair, then a lone top field before a frame picture.
static bool fieldPairs(Fake *f, const MPEG2Sequence *interlaced)
{
    MPEG2Decoder *d = create(f);
    if (mpeg2_decoder_set_sequence(d, interlaced))
        return false;
    uint8_t sample[128];
    size_t n = fieldPicture(sample, 1, 1);
    n += fieldPicture(sample + n, 2, 2);
    int submits = f->submits;
    const int outputs = f->outputs;
    bool ok = mpeg2_decoder_decode(d, sample, n, NULL) == 1 && f->submits == submits + 2 && f->outputs == outputs + 1;
    const nvdec_mpeg2_pic_s *p = (const nvdec_mpeg2_pic_s *)f->last.setup;
    const uint32_t pair = slot(&f->last, 0);
    ok = ok && p->secondfield && p->picture_structure == 2 && p->picture_coding_type == 2 &&
         slot(&f->last, 1) == pair && f->out.handle == pair && f->out.fields == AVD_FIELDS_TOP_FIRST;
    // A P frame after it, then a B field pair: bottom first, between the two references.
    n = fieldPicture(sample, 2, 3);
    ok = ok && mpeg2_decoder_decode(d, sample, n, NULL) == 1 && slot(&f->last, 1) == pair;
    const uint32_t future = slot(&f->last, 0);
    n = fieldPicture(sample, 3, 2);
    n += fieldPicture(sample + n, 3, 1);
    submits = f->submits;
    ok = ok && mpeg2_decoder_decode(d, sample, n, NULL) == 1 && f->submits == submits + 2 &&
         slot(&f->last, 1) == pair && slot(&f->last, 2) == future && f->out.fields == AVD_FIELDS_BOTTOM_FIRST &&
         ((const nvdec_mpeg2_pic_s *)f->last.setup)->secondfield;
    // A lone field: decoded, never put out; the frame picture after it goes out on its own.
    n = fieldPicture(sample, 1, 1);
    const int lone = f->outputs;
    ok = ok && mpeg2_decoder_decode(d, sample, n, NULL) == 0 && f->outputs == lone;
    n = fieldPicture(sample, 1, 3);
    ok =
        ok && mpeg2_decoder_decode(d, sample, n, NULL) == 1 && !((const nvdec_mpeg2_pic_s *)f->last.setup)->secondfield;
    mpeg2_decoder_destroy(d);
    return ok;
}

// Clip A from its third picture (a B-picture): skipped without a job until the I-picture.
static bool missingReferences(Fake *f, const MPEG2Sequence *seq)
{
    MPEG2Decoder *d = create(f);
    const int submits = f->submits;
    const size_t *v = kMPEG2ClipAPictures;
    bool ok = !mpeg2_decoder_set_sequence(d, seq) &&
              mpeg2_decoder_decode(d, kMPEG2ClipA + v[2], v[3] - v[2], NULL) == 0 && f->submits == submits;
    mpeg2_decoder_destroy(d);
    return ok;
}

typedef void (*Mutation)(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *avail);

static int indexOf(const TegraNVDECSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method)
            return (int)i;
    return 0;
}

static void setupSize(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)p, (void)a;
    s->setup_bytes--;
}

static void controlType(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)p, (void)a;
    s->entries[indexOf(s, NVC5B0_SET_CONTROL_PARAMS)].value =
        (s->entries[indexOf(s, NVC5B0_SET_CONTROL_PARAMS)].value & ~0xfu) | 4;
}

static void noSlices(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)p, (void)a;
    s->entries[indexOf(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET)].method = NVC5B0_SET_HISTORY_OFFSET;
}

static void fewSlices(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    a[indexOf(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET)] = p->slice_count * 4;
}

static void history(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->HistBufferSize = 1;
}

static void structure(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->picture_structure = 0;
}

static void secondFrame(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->picture_structure = 3;
    p->secondfield = 1;
}

static void nv24(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->ref_memory_layout[0] = 1;
}

static void tfOutput(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->displayPara.enableTFOutput = 1;
}

static void dims(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->FrameHeight += 16;
}

static void pitch(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->pitch_chroma += 64;
}

static void surface(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    a[indexOf(s, NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4)] =
        (uint64_t)p->pitch_luma * ((p->FrameHeightInMbs * 16u + 31) & ~31u) - 1;
}

static void timer(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->gptimer_timeout_value = (1u << 27) + 1;
}

static void encrypted(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    p->encryption_params.enable_encryption = 1;
}

static void substreams(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    (void)s, (void)a;
    ((uint8_t *)&p->ssm)[0] = 1;
}

static void stream(TegraNVDECSubmit *s, nvdec_mpeg2_pic_s *p, uint64_t *a)
{
    a[indexOf(s, NVC5B0_SET_IN_BUF_BASE_OFFSET)] = p->stream_len - 1;
}

static int refusals(const Fake *f)
{
    static const Mutation kMutations[] = {setupSize,   controlType, noSlices,   fewSlices, history, structure,
                                          secondFrame, nv24,        tfOutput,   dims,      pitch,   surface,
                                          timer,       encrypted,   substreams, stream};
    int refused = 0;
    for (unsigned i = 0; i < sizeof(kMutations) / sizeof(kMutations[0]); i++) {
        TegraNVDECSubmit s = f->first;
        uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
        memcpy(avail, f->firstAvail, sizeof(avail));
        kMutations[i](&s, (nvdec_mpeg2_pic_s *)s.setup, avail);
        if (tegra_nvdec_check(&s, avail))
            refused++;
        else
            fprintf(stderr, "checker accepts bad MPEG-2 submit %u\n", i);
    }
    return refused == (int)(sizeof(kMutations) / sizeof(kMutations[0])) ? refused : -1;
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    static Fake f;
    const Clip clips[3] = {
        {"A", kMPEG2ClipA, kMPEG2ClipAConfig, kMPEG2ClipAPictures, 18, kMPEG2TraceASize, kMPEG2TraceAIntra,
         kMPEG2TraceAInter, kMPEG2TraceA},
        {"B", kMPEG2ClipB, kMPEG2ClipBConfig, kMPEG2ClipBPictures, 12, kMPEG2TraceBSize, kMPEG2TraceBIntra,
         kMPEG2TraceBInter, kMPEG2TraceB},
        {"C", kMPEG2ClipC, kMPEG2ClipCConfig, kMPEG2ClipCPictures, 15, kMPEG2TraceCSize, kMPEG2TraceCIntra,
         kMPEG2TraceCInter, kMPEG2TraceC},
    };
    int failed[3];
    for (int i = 0; i < 3; i++)
        failed[i] = runClip(&clips[i], &f);
    MPEG2Sequence seqA = {0}, seqB = {0};
    mpeg2_parse_config(&seqA, kMPEG2ClipA, kMPEG2ClipAConfig);
    mpeg2_parse_config(&seqB, kMPEG2ClipB, kMPEG2ClipBConfig);
    const bool fields = fieldPairs(&f, &seqB);
    const bool skipped = missingReferences(&f, &seqA);
    const int refused = refusals(&f);
    MPEG2Sequence chroma422 = seqA, huge = seqA;
    chroma422.chroma_format = 2;
    huge.width = 4096;
    MPEG2Decoder *d = create(&f);
    const bool unsupported =
        mpeg2_decoder_set_sequence(d, &chroma422) == -1 && mpeg2_decoder_set_sequence(d, &huge) == -1;
    mpeg2_decoder_destroy(d);
    const bool ok = !failed[0] && !failed[1] && !failed[2] && fields && skipped && refused > 0 && unsupported &&
                    !f.refused && !f.engine.waits && !f.engine.busyWrites;
    printf("%s mpeg2: clip A 18, clip B 12 (interlaced), clip C 15 (MPEG-1) pictures against FFmpeg, field pairs %s, "
           "missing references %s, checker refuses %d bad submits, 4:2:2 and oversized %s, %d jobs refused, %u waits, "
           "%u CPU writes into a buffer a queued job uses\n",
           ok ? "PASS" : "FAIL", fields ? "ok" : "wrong", skipped ? "skipped" : "decoded", refused,
           unsupported ? "refused" : "taken", f.refused, f.engine.waits, f.engine.busyWrites);
    if (failed[0] || failed[1] || failed[2])
        printf("failed at clip A %d, B %d, C %d\n", failed[0] - 1, failed[1] - 1, failed[2] - 1);
    return ok ? 0 : 1;
}
