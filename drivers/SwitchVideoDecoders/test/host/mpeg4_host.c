// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the MPEG-4 Part 2 half of AVD.videodecoder, against FFmpeg (test/MPEG4Trace.h, MPEG4Clip.h, made by
// mpeg4gen from FFmpeg 8.1.2). MPEG4Syntax/MPEG4NVDEC run over a fake NVDEC; per VOP, what NVDEC is given must equal
// what FFmpeg's decoder parsed (picture type, rounding, f_code and b_code, quarter sample, resync markers, time
// increment bits) and, for B-VOPs, the TRD and TRB of FFmpeg's VOP times; the bitstream must be the VOP with averne's
// end sequence, the picture slots the current picture and the references the VOP's type names, and the picture out the
// one decoded. The matrices must be the ones clip A was encoded with. A VOP that is not coded puts the newest reference
// out with no job; a packed sample (a P- and a B-VOP) decodes both and puts the B-VOP out. Every submit must pass the
// kernel's checker (TegraNVDECCheck.h), which must refuse each kind of bad MPEG-4 submit derived from the first one.
// Clip C's layer (interlaced, data partitioned) and other unsupported layers must be refused.
//   mpeg4_host           prints "PASS mpeg4 ..." (mpeg4_host_test)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/MPEG4NVDEC.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "fake_engine.h"
#include "../MPEG4Clip.h"
#include "../MPEG4Trace.h"

// The matrices clip A was encoded with (mpeg4gen.c: -intra_matrix and -inter_matrix, raster order).
static const uint8_t kIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};
static const uint8_t kInter[64] = {
    16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23,
    24, 25, 19, 20, 21, 22, 23, 24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24,
    26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23, 24, 25, 27, 28, 30, 31, 40,
};

typedef struct {
    uint8_t *va[64];
    uint64_t size[64];
    int submits, refused, outputs;
    TegraNVDECSubmit last, first;
    uint64_t firstAvail[TEGRA_NVDEC_MAX_ENTRIES];
    bool haveFirst;
    uint32_t outHandle, outWidth, outHeight;
    void *outToken;
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
        fprintf(stderr, "kernel check refuses MPEG-4 job %d: %s\n", f->submits, why);
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
    f->outHandle = picture->handle;
    f->outWidth = picture->width;
    f->outHeight = picture->height;
    f->outToken = picture->token;
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

static uint32_t slot(const TegraNVDECSubmit *s, int i)
{
    return reloc(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), NULL);
}

static MPEG4Decoder *create(Fake *f)
{
    const H264Backend backend = {.ctx = f,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    return mpeg4_decoder_create(&backend);
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t config;
    const size_t *vops;
    int count, timeBits;
    const MPEG4TraceVOP *trace;
    uint16_t width, height;
} Clip;

// Every VOP of a clip, each field NVDEC is given against FFmpeg's; 0 or the failing VOP + 1.
static int runClip(const Clip *c, Fake *f, MPEG4VOL *vol, uint32_t *lastRef)
{
    memset(vol, 0, sizeof(*vol));
    if (mpeg4_parse_config(vol, c->data, c->config) != 1 || mpeg4_vol_unsupported(vol) || vol->width != c->width ||
        vol->height != c->height || vol->time_bits != c->timeBits) {
        fprintf(stderr, "%s: VOL %ux%u, %u time bits\n", c->name, vol->width, vol->height, vol->time_bits);
        return 1;
    }
    MPEG4Decoder *d = create(f);
    if (mpeg4_decoder_configure(d, vol))
        return 1;
    uint32_t refs[2] = {0, 0};  // past, future
    long long refTime[2] = {0, 0};
    for (int i = 0; i < c->count; i++) {
        const MPEG4TraceVOP *t = &c->trace[i];
        const uint8_t *sample = c->data + c->vops[i];
        const size_t size = c->vops[i + 1] - c->vops[i];
        const int submits = f->submits;
        if (mpeg4_decoder_decode(d, sample, size, (void *)(uintptr_t)(i + 1)) != 1 || f->submits != submits + 1) {
            fprintf(stderr, "%s VOP %d: not decoded\n", c->name, i);
            return i + 1;
        }
        size_t vop = 0;  // the VOP after any headers the sample starts with
        while (vop + 4 <= size &&
               !(sample[vop] == 0 && sample[vop + 1] == 0 && sample[vop + 2] == 1 && sample[vop + 3] == 0xb6))
            vop++;
        const TegraNVDECSubmit *s = &f->last;
        const nvdec_mpeg4_pic_s *p = (const nvdec_mpeg4_pic_s *)s->setup;
        const int type = t->type == 'I' ? 0 : t->type == 'P' ? 1 : 2;
        bool ok = p->ptype == type && p->rnd == 1 - t->rnd && p->vop_fcode_forward == t->f_code &&
                  p->vop_fcode_backward == t->b_code && p->quarter_sample == t->qpel &&
                  p->resync_marker_disable == !t->resync && p->vop_time_increment_bitcount == c->timeBits &&
                  p->FrameWidth == (c->width + 15) / 16 * 16 && p->FrameHeight == (c->height + 15) / 16 * 16 &&
                  p->stream_len == size - vop + 16;
        if (type == 2)
            ok = ok && p->trd[0] == refTime[1] - refTime[0] && p->trb[0] == t->time - refTime[0];
        uint32_t offset = 0;
        const uint32_t input = reloc(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, &offset);
        static const uint8_t kEnd[16] = {0, 0, 1, 0xb1, 0, 0, 0, 0, 0, 0, 1, 0xb1, 0, 0, 0, 0};
        ok = ok && !memcmp(f->va[input] + offset, sample + vop, size - vop) &&
             !memcmp(f->va[input] + offset + size - vop, kEnd, 16);
        const uint32_t current = slot(s, 0);
        ok = ok &&
             (type == 0   ? slot(s, 1) == current && slot(s, 2) == current
              : type == 1 ? slot(s, 1) == refs[1] && slot(s, 2) == current && current != refs[1]
                          : slot(s, 1) == refs[0] && slot(s, 2) == refs[1] && current != refs[0] && current != refs[1]);
        ok = ok && f->outHandle == current && f->outWidth == c->width && f->outHeight == c->height &&
             f->outToken == (void *)(uintptr_t)(i + 1);
        if (!ok) {
            fprintf(stderr, "%s VOP %d (%c): ptype %d rnd %d fc %d,%d qpel %d resync off %d trd %d trb %d len %u\n",
                    c->name, i, t->type, p->ptype, p->rnd, p->vop_fcode_forward, p->vop_fcode_backward,
                    p->quarter_sample, p->resync_marker_disable, p->trd[0], p->trb[0], p->stream_len);
            return i + 1;
        }
        if (type != 2) {
            refs[0] = refs[1];
            refs[1] = current;
            refTime[0] = refTime[1];
            refTime[1] = t->time;
        }
    }
    *lastRef = refs[1];
    mpeg4_decoder_destroy(d);
    return 0;
}

// A P-VOP with vop_coded 0 at time increment `time` (15 bits), stuffed to a byte.
static size_t notCoded(uint8_t *out, uint32_t time)
{
    const uint8_t prefix[4] = {0, 0, 1, 0xb6};
    memcpy(out, prefix, 4);
    uint64_t bits = 0;
    unsigned n = 0;

    const struct {
        uint32_t value;
        unsigned count;
    } fields[] = {{1, 2}, {0, 1}, {1, 1}, {time, 15}, {1, 1}, {0, 1}, {0, 1}};

    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        bits = bits << fields[i].count | fields[i].value;
        n += fields[i].count;
    }
    while (n % 8) {
        bits = bits << 1 | 1;
        n++;
    }
    for (unsigned i = 0; i < n / 8; i++)
        out[4 + i] = (uint8_t)(bits >> (n - 8 * (i + 1)));
    return 4 + n / 8;
}

// Clip A's samples 0-3, then 4 (P) and 5 (B) packed into one sample, then 6.
static bool packed(Fake *f, const MPEG4VOL *vol)
{
    MPEG4Decoder *d = create(f);
    if (mpeg4_decoder_configure(d, vol))
        return false;
    const size_t *v = kMPEG4ClipAVOPs;
    for (int i = 0; i < 4; i++) {
        if (mpeg4_decoder_decode(d, kMPEG4ClipA + v[i], v[i + 1] - v[i], NULL) != 1)
            return false;
    }
    const int submits = f->submits;
    const int ret = mpeg4_decoder_decode(d, kMPEG4ClipA + v[4], v[6] - v[4], NULL);
    const bool both = ret == 1 && f->submits == submits + 2 && f->outHandle == slot(&f->last, 0) &&
                      ((const nvdec_mpeg4_pic_s *)f->last.setup)->ptype == 2;
    const bool next = mpeg4_decoder_decode(d, kMPEG4ClipA + v[6], v[7] - v[6], NULL) == 1 && f->submits == submits + 3;
    mpeg4_decoder_destroy(d);
    return both && next;
}

typedef void (*Mutation)(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *avail);

static void setupSize(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p, (void)a, s->setup_bytes--;
}

static void wrongApp(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p, (void)a, s->entries[0].value = 3;
}

static void noScratch(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p, (void)a;
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET)
            s->entries[i].method = NVC5B0_SET_HISTOGRAM_OFFSET;
}

static void smallScratch(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p;
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET)
            a[i] = 0x3ff;
}

static void bigScratch(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->scratch_pic_buffer_size = 0x800;
}

static void smallSurface(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p;
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 8)
            a[i] = (uint64_t)p->FrameStride[0] * ((p->height + 31u) & ~31u) / 2 - 1;
}

static void nv24(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->curr_output_memory_layout = 1;
}

static void nv24Ref(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->ref_memory_layout[1] = 1;
}

static void tfOutput(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->displayPara.enableTFOutput = 1;
}

static void histogram(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->displayPara.enableHistogram = 1;
}

static void divx(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->divx_flags = 2;
}

static void pitch(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->FrameStride[1] += 64;
}

static void bigger(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->height = p->FrameHeight = 2064;
}

static void ragged(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->width = p->FrameWidth = 180;
}

static void timer(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->gptimer_timeout_value = 1u << 28;
}

static void encrypted(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->encryption_params.enable_encryption = 1;
}

static void history(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)s, (void)a, p->HistBufferSize = 1;
}

static void stream(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p;
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == NVC5B0_SET_IN_BUF_BASE_OFFSET)
            a[i] = p->stream_len - 1;
}

static void notify(TegraNVDECSubmit *s, nvdec_mpeg4_pic_s *p, uint64_t *a)
{
    (void)p, (void)a, s->entries[s->entry_count - 1].value |= 1;
}

static int refusals(const Fake *f)
{
    static const Mutation kMutations[] = {
        setupSize, wrongApp, noScratch, smallScratch, bigScratch, smallSurface, nv24,    nv24Ref, tfOutput, histogram,
        divx,      pitch,    bigger,    ragged,       timer,      encrypted,    history, stream,  notify};
    int refused = 0;
    for (unsigned i = 0; i < sizeof(kMutations) / sizeof(kMutations[0]); i++) {
        TegraNVDECSubmit s = f->first;
        uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
        memcpy(avail, f->firstAvail, sizeof(avail));
        kMutations[i](&s, (nvdec_mpeg4_pic_s *)s.setup, avail);
        if (tegra_nvdec_check(&s, avail))
            refused++;
        else
            fprintf(stderr, "checker accepts bad MPEG-4 submit %u\n", i);
    }
    return refused == (int)(sizeof(kMutations) / sizeof(kMutations[0])) ? refused : -1;
}

static bool unsupportedLayers(const MPEG4VOL *good)
{
    MPEG4VOL c = {0};
    const bool clipC = mpeg4_parse_config(&c, kMPEG4ClipC, sizeof(kMPEG4ClipC)) == 1 && c.interlaced &&
                       c.data_partitioned && mpeg4_vol_unsupported(&c);
    MPEG4VOL gmc = *good, rect = *good, wide = *good;
    gmc.sprite_enable = 2;
    rect.shape = 1;
    wide.width = 2048;
    Fake f = {0};
    MPEG4Decoder *d = create(&f);
    const bool refused = mpeg4_decoder_configure(d, &gmc) == -1 && mpeg4_decoder_configure(d, &rect) == -1 &&
                         mpeg4_decoder_configure(d, &wide) == -1 && !f.va[1];
    mpeg4_decoder_destroy(d);
    return clipC && refused && mpeg4_vol_unsupported(&gmc) && mpeg4_vol_unsupported(&rect);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static Fake f;
    const Clip clips[2] = {
        {"A", kMPEG4ClipA, kMPEG4ClipAConfig, kMPEG4ClipAVOPs, 18, kMPEG4TraceATimeBits, kMPEG4TraceA, 176, 144},
        {"B", kMPEG4ClipB, kMPEG4ClipBConfig, kMPEG4ClipBVOPs, 32, kMPEG4TraceBTimeBits, kMPEG4TraceB, 136, 72},
    };
    MPEG4VOL volA, volB;
    uint32_t lastRef = 0;
    const int failedA = runClip(&clips[0], &f, &volA, &lastRef);
    const bool matrices =
        volA.quant_type && !memcmp(volA.intra_matrix, kIntra, 64) && !memcmp(volA.inter_matrix, kInter, 64);
    const int failedB = runClip(&clips[1], &f, &volB, &lastRef);

    MPEG4Decoder *d = create(&f);
    bool skipped = !mpeg4_decoder_configure(d, &volA);
    for (int i = 0; skipped && i < 18; i++)
        skipped = mpeg4_decoder_decode(d, kMPEG4ClipA + kMPEG4ClipAVOPs[i], kMPEG4ClipAVOPs[i + 1] - kMPEG4ClipAVOPs[i],
                                       NULL) == 1;
    uint8_t nvop[16];
    const size_t nvopSize = notCoded(nvop, 18018);
    const int submits = f.submits;
    const uint32_t future = slot(&f.last, 2);  // the last VOP is a B-VOP: its future reference is the newest
    skipped = skipped && mpeg4_decoder_decode(d, nvop, nvopSize, (void *)7) == 1 && f.submits == submits &&
              f.outHandle == future && f.outToken == (void *)7;
    const bool truncated = mpeg4_decoder_decode(d, kMPEG4ClipA + kMPEG4ClipAVOPs[1], 5, NULL) == -1;
    mpeg4_decoder_destroy(d);

    const bool pack = packed(&f, &volA);
    // Joined after the I-VOP: the P- and B-VOPs before the next I-VOP are skipped without a job.
    d = create(&f);
    const int before = f.submits;
    const size_t *v = kMPEG4ClipAVOPs;
    const bool joined = !mpeg4_decoder_configure(d, &volA) &&
                        mpeg4_decoder_decode(d, kMPEG4ClipA + v[1], v[2] - v[1], NULL) == 0 &&
                        mpeg4_decoder_decode(d, kMPEG4ClipA + v[2], v[3] - v[2], NULL) == 0 && f.submits == before;
    mpeg4_decoder_destroy(d);
    const int refused = refusals(&f);
    const bool layers = unsupportedLayers(&volA);
    const bool ok = !failedA && !failedB && matrices && skipped && truncated && pack && joined && refused > 0 &&
                    layers && !f.refused && !f.engine.waits && !f.engine.busyWrites;
    printf("%s mpeg4: clip A 18 VOPs, clip B 32 VOPs against FFmpeg (matrices %s, not-coded VOP %s, truncated VOP "
           "%s, packed P+B %s, missing references %s), checker refuses %d bad submits, unsupported layers %s, %d jobs "
           "refused, %u waits, %u CPU writes into a buffer a queued job uses\n",
           ok ? "PASS" : "FAIL", matrices ? "ok" : "wrong", skipped ? "ok" : "wrong", truncated ? "refused" : "taken",
           pack ? "ok" : "wrong", joined ? "skipped" : "decoded", refused, layers ? "refused" : "taken", f.refused,
           f.engine.waits, f.engine.busyWrites);
    if (failedA || failedB)
        printf("failed at clip A VOP %d, clip B VOP %d\n", failedA - 1, failedB - 1);
    return ok ? 0 : 1;
}
