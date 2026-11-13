// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the H.264 half of AVD.videodecoder: runs H264Syntax/H264NVDEC over an Annex-B stream with a fake
// NVDEC backend and checks every submit with the kernel's checker (TegraNVDECCheck.h).
//   h264_host <stream.264>             prints "out <decode index> <poc>" in display order (compare with ffprobe)
//   h264_host --clip                   test/VDECClip.h's clip: every submit accepted, pictures out in x264's display
//                                      order, and the checker refusing each kind of bad submit derived from the first
//                                      one; prints "PASS h264 clip ..." (h264_host_test)
//   h264_host --mbaff                  test/H264MbaffClip.h's MBAFF clip: every submit an MBAFF frame the checker
//                                      accepts, pictures out in FFmpeg's display order tagged top field first
// Build: cc -O1 -o h264_host h264_host.c ../../AVD/H264Syntax.c ../../AVD/H264NVDEC.c ../../AVD/H264DPB.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/H264NVDEC.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "../VDECClip.h"
#include "../H264MbaffClip.h"
#include "fake_engine.h"

typedef struct {
    uint8_t *va[64];
    uint64_t size[64];
    unsigned decoded, failures, recycled, notMbaff;
    bool clip, mbaff, haveFirst;
    TegraNVDECSubmit first;  // the first submit and its relocations' room, for the refusal cases
    uint64_t firstAvail[TEGRA_NVDEC_MAX_ENTRIES];
    unsigned order[64], outputs;
    FakeEngine engine;
} Fake;

static int fakeAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    Fake *f = ctx;
    for (uint32_t h = 1; h < 64; h++) {
        if (f->va[h])
            continue;
        f->va[h] = calloc(1, size);
        f->size[h] = size;
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
        if (e->value >= 64 || !f->va[e->value] || e->offset > f->size[e->value]) {
            fprintf(stderr, "bad relocation %u\n", i);
            return -1;
        }
        avail[i] = f->size[e->value] - e->offset;
    }
    const char *why = tegra_nvdec_check(s, avail);
    if (why) {
        fprintf(stderr, "kernel check refuses picture %u: %s\n", f->decoded, why);
        f->failures++;
        return -1;
    }
    const nvdec_h264_pic_s *p = (const nvdec_h264_pic_s *)s->setup;
    if (f->mbaff && (!p->MbaffFrameFlag || p->frame_mbs_only_flag || p->field_pic_flag || p->FrameHeightInMbs != 4))
        f->notMbaff++;
    if (!f->haveFirst) {
        f->first = *s;
        memcpy(f->firstAvail, avail, sizeof(avail));
        f->haveFirst = true;
    }
    *fence = fake_engine_submit(&f->engine, s);
    return 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *w)
{
    fake_engine_wait(&((Fake *)ctx)->engine, w->fence);
    return 0;
}

typedef struct {
    unsigned index;
    int32_t poc;
} Item;

static void *fakeDecoded(void *ctx, const H264Picture *pic)
{
    Fake *f = ctx;
    if (pic->luma != f->va[pic->handle] || (f->mbaff && pic->fields != AVD_FIELDS_TOP_FIRST))
        ++f->failures;
    Item *it = malloc(sizeof(*it));
    it->index = (unsigned)(uintptr_t)pic->token;
    it->poc = pic->poc;
    f->decoded++;
    return it;
}

static void fakeEmit(void *ctx, void *item)
{
    Fake *f = ctx;
    Item *it = item;
    if (f->clip && f->outputs < 64)
        f->order[f->outputs++] = it->index;
    if (!f->clip)
        printf("out %u %d\n", it->index, it->poc);
    free(it);
}

static void fakeDiscard(void *ctx, void *item)
{
    (void)ctx, free(item);
}

static int entryOf(const TegraNVDECSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method)
            return (int)i;
    return -1;
}

// --clip: the refusals the kernel must make, each from the clip's first (accepted) submit.
static int refusals(const Fake *f)
{
    int bad = 0;
    for (int c = 0; c <= 13; c++) {
        TegraNVDECSubmit s = f->first;
        uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
        memcpy(avail, f->firstAvail, sizeof(avail));
        nvdec_h264_pic_s *p = (nvdec_h264_pic_s *)s.setup;
        const int control = entryOf(&s, NVC5B0_SET_CONTROL_PARAMS), execute = entryOf(&s, NVC5B0_EXECUTE);
        const int luma = entryOf(&s, NVC5B0_SET_PICTURE_LUMA_OFFSET0),
                  status = entryOf(&s, NVC5B0_SET_NVDEC_STATUS_OFFSET);
        const int input = entryOf(&s, NVC5B0_SET_IN_BUF_BASE_OFFSET);
        const char *expected = NULL;
        switch (c) {
        case 0:
            break;  // as decoded: accepted
        case 1:
            s.codec = 2, expected = "codec";  // VC-1, which NVDEC is not given
            break;
        case 2:
            s.entries[control].value &= ~0x10u, expected = "control params";
            break;  // GP timer off
        case 3:
            p->gptimer_timeout_value = (1u << 27) + 1, expected = "GP timer limit";
            break;
        case 4: {
            const TegraNVDECEntry last = s.entries[execute];
            s.entries[execute] = s.entries[execute - 1];
            s.entries[execute - 1] = last;
            expected = "execute not last";
            break;
        }
        case 5:
            s.entries[execute].value |= 1u, expected = "execute flags";
            break;  // NOTIFY: a semaphore release
        case 6:
            s.entries[luma].offset += 16, expected = "relocation alignment";
            break;
        case 7:
            avail[luma] = 64, expected = "surface too small";
            break;
        case 8:
            ((uint8_t *)&p->encryption_params)[0] = 1, expected = "encryption parameters";
            break;
        case 9:
            memmove(&s.entries[status], &s.entries[status + 1], (s.entry_count - status - 1) * sizeof(s.entries[0]));
            memmove(&avail[status], &avail[status + 1], (s.entry_count - status - 1) * sizeof(avail[0]));
            s.entry_count--;
            expected = "missing relocation";
            break;
        case 10:
            avail[input] = p->stream_len - 1, expected = "bitstream";
            break;
        case 11:
            p->PicWidthInMbs = 257, expected = "dimensions";
            break;
        case 12:
            s.setup_bytes -= 4, expected = "setup size";
            break;
        case 13:
            p->displayPara.enableTFOutput = 1, expected = "display output or histogram";
            break;
        }
        const char *why = tegra_nvdec_check(&s, avail);
        if ((why == NULL) != (expected == NULL) || (why && strcmp(why, expected))) {
            fprintf(stderr, "refusal case %d: checker said \"%s\", expected \"%s\"\n", c, why ? why : "accepted",
                    expected ? expected : "accepted");
            bad++;
        }
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    static uint8_t data[64 << 20];
    Fake fake = {.mbaff = !strcmp(argv[1], "--mbaff")};
    fake.clip = fake.mbaff || !strcmp(argv[1], "--clip");
    const int frames = fake.mbaff ? kMbaffFrames : kClipFrames;
    const int *display = fake.mbaff ? kMbaffDisplay : kClipDisplay;
    size_t size;
    if (fake.clip) {
        size = fake.mbaff ? sizeof(kMbaffClip) : sizeof(kClip);
        memcpy(data, fake.mbaff ? kMbaffClip : kClip, size);
    } else {
        FILE *fp = fopen(argv[1], "rb");
        if (!fp)
            return 2;
        size = fread(data, 1, sizeof(data), fp);
        fclose(fp);
    }
    const H264Backend backend = {.ctx = &fake,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeDiscard,
                                 .recycle = switch_fake_recycle,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    H264Decoder *d = h264_decoder_create(&backend);

    // Split into NAL units, then access units (7.4.1.2.3: a new one starts at an AUD, a parameter set or SEI after
    // slices, or a slice with first_mb_in_slice 0); re-pack each as 4-byte length-prefixed NAL units.
    static size_t starts[1 << 16], ends[1 << 16];
    size_t count = 0;
    for (size_t i = 0; i + 3 <= size && count < (1 << 16);) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (count) {
                size_t e = i;
                while (e > starts[count - 1] && !data[e - 1])
                    e--;
                ends[count - 1] = e;
            }
            starts[count++] = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (!count)
        return 2;
    ends[count - 1] = size;
    for (size_t i = 0; i < count; i++) {
        const uint8_t t = data[starts[i]] & 0x1f;
        if (t == H264_NAL_SPS || t == H264_NAL_PPS)
            h264_decoder_parameter_set(d, data + starts[i], ends[i] - starts[i]);
    }
    if (h264_decoder_configure(d)) {
        fprintf(stderr, "configure refused the stream\n");
        return 1;
    }
    static uint8_t au[16 << 20];
    size_t auSize = 0;
    bool slices = false;
    unsigned index = 0;
    int errors = 0;
    for (size_t i = 0; i <= count; i++) {
        bool boundary = i == count;
        if (!boundary) {
            const uint8_t t = data[starts[i]] & 0x1f;
            if (slices && (t == 9 || t == H264_NAL_SPS || t == H264_NAL_PPS || t == H264_NAL_SEI))
                boundary = true;
            if (slices && (t == 1 || t == 5) && (data[starts[i] + 1] & 0x80))
                boundary = true;  // ue(first_mb) == 0
        }
        if (boundary && slices) {
            const int r = h264_decoder_decode(d, au, auSize, 4, (void *)(uintptr_t)index++, true);
            if (r < 0) {
                fprintf(stderr, "decode failed at access unit %u (%d)\n", index - 1, r);
                errors++;
            }
            auSize = 0;
            slices = false;
        }
        if (i == count)
            break;
        const size_t n = ends[i] - starts[i];
        au[auSize] = (uint8_t)(n >> 24), au[auSize + 1] = (uint8_t)(n >> 16);
        au[auSize + 2] = (uint8_t)(n >> 8), au[auSize + 3] = (uint8_t)n;
        memcpy(au + auSize + 4, data + starts[i], n);
        auSize += n + 4;
        const uint8_t t = data[starts[i]] & 0x1f;
        if (t == 1 || t == 5)
            slices = true;
    }
    h264_decoder_flush(d);
    h264_decoder_destroy(d);
    if (fake.clip) {
        // Display position p is the access unit i with display[i] == p.
        int misordered = fake.outputs != (unsigned)frames;
        for (unsigned p = 0; p < fake.outputs && p < (unsigned)frames; p++)
            if (fake.order[p] >= (unsigned)frames || display[fake.order[p]] != (int)p)
                misordered++;
        const int refused = fake.mbaff ? (int)fake.notMbaff : fake.haveFirst ? refusals(&fake) : 1;
        const bool pass = !errors && !fake.failures && !misordered && !refused && index == (unsigned)frames &&
                          fake.recycled == fake.decoded && !fake.engine.waits && !fake.engine.busyWrites;
        printf("%s h264 %s: %u access units, %u pictures, %d errors, %u refused by the checker, %d out of display "
               "order, %d %s, %u waits, %u CPU writes into a buffer a queued job uses\n",
               pass ? "PASS" : "FAIL", fake.mbaff ? "mbaff clip" : "clip", index, fake.decoded, errors, fake.failures,
               misordered, refused, fake.mbaff ? "not MBAFF frames" : "refusal cases wrong", fake.engine.waits,
               fake.engine.busyWrites);
        return pass ? 0 : 1;
    }
    fprintf(stderr, "%u access units, %u pictures, %d errors, %u refused\n", index, fake.decoded, errors,
            fake.failures);
    return errors || fake.failures ? 1 : 0;
}
