// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for interlaced H.264 in AVD.videodecoder: h264_fields_scenarios.h's field-coded streams through H264NVDEC
// with a fake NVDEC backend. For each picture it checks the job (field_pic_flag, bottom_field_flag, second_field, a
// second field in its first field's surface, the kernel's checker accepting it, the order counts it is given), builds
// RefPicList0 from the dpb[] table NVDEC is given (8.2.4.2.1, 8.2.4.2.2 and 8.2.4.2.5, as the engine builds it) and
// compares it with FFmpeg's, and checks that every sample's frame is answered exactly once, in decoding order. Then
// the checker refuses each interlaced setup it must not take.
//   h264_fields_host                        prints "PASS h264 fields: ..." (h264_fields_host_test)
//   h264_fields_host --dump <case> <file>   writes the case's stream (Annex B) for FFmpeg
//   h264_fields_host --oracle <case> <yuv>  prints the RefPicList0 FFmpeg's decode of it shows, per P picture
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/H264NVDEC.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"
#include "fake_engine.h"
#include "h264_fields_scenarios.h"

enum {
    MAX_JOBS = 64,
    WIDTH = 256,
    HEIGHT = 32
};

typedef struct {
    uint8_t *va[64];
    uint64_t size[64];
    nvdec_h264_pic_s jobs[MAX_JOBS];
    unsigned job_count, refused;
    TegraNVDECSubmit first;
    uint64_t first_avail[TEGRA_NVDEC_MAX_ENTRIES];
    unsigned emitted[64], emits, decoded;
    uint8_t fields[64];
    FakeEngine engine;
    unsigned pairJobsWrong;  // a frame whose first field's job is not the job before its second field's
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
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].kind == TEGRA_NVDEC_ENTRY_RELOC)
            avail[i] = f->size[s->entries[i].value] - s->entries[i].offset;
    const char *why = tegra_nvdec_check(s, avail);
    if (why || f->job_count == MAX_JOBS) {
        fprintf(stderr, "job %u refused: %s\n", f->job_count, why ? why : "too many");
        f->refused++;
        return -1;
    }
    const nvdec_h264_pic_s *p = (const nvdec_h264_pic_s *)s->setup;
    if (!f->job_count) {
        f->first = *s;
        memcpy(f->first_avail, avail, sizeof(avail));
    }
    f->jobs[f->job_count++] = *p;
    *fence = fake_engine_submit(&f->engine, s);
    return 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *w)
{
    fake_engine_wait(&((Fake *)ctx)->engine, w->fence);
    return 0;
}

static void *fakeDecoded(void *ctx, const H264Picture *pic)
{
    Fake *f = ctx;
    const unsigned sample = (unsigned)(uintptr_t)pic->token;
    if (sample < 64)
        f->fields[sample] = pic->fields;
    f->pairJobsWrong += pic->first_field_job && pic->first_field_job + 1 != pic->job;
    f->decoded++;
    return (void *)(uintptr_t)(sample + 1);
}

static void fakeEmit(void *ctx, void *item)
{
    Fake *f = ctx;
    if (f->emits < 64)
        f->emitted[f->emits++] = (unsigned)(uintptr_t)item - 1;
}

static void fakeDiscard(void *ctx, void *item)
{
    fakeEmit(ctx, item);
}

static FieldsScenario scenario(const FieldsCase *c, FieldsPicture *pictures)
{
    for (unsigned i = 0; i < c->count; i++)
        pictures[i] = c->steps[i].picture;
    return (FieldsScenario){c->name, c->max_num_ref_frames, c->gaps, pictures, c->count};
}

static void append(char *out, size_t size, const char *text)
{
    const size_t n = strlen(out);
    snprintf(out + n, size - n, "%s%s", n ? " " : "", text);
}

// Which picture each surface's top and bottom field last came from: "n", "n'" (a frame picture's bottom field).
typedef char Name[8];

// 8.2.4.2.1 (a frame: frame stores with both fields marked, short-term by FrameNumWrap descending, then long-term by
// LongTermFrameIdx) or 8.2.4.2.2 and 8.2.4.2.5 (a field: frame stores with either field marked, alternating parity from
// the current one), over the dpb[] table as NVDEC is given it.
// `active`: num_ref_idx_l0_active_minus1 + 1 from the slice header, which NVDEC parses itself.
static void list0(const nvdec_h264_pic_s *p, unsigned active, Name names[17][2], char *out, size_t size)
{
    const int maxFrameNum = 1 << (p->log2_max_frame_num_minus4 + 4);
    const unsigned parity = p->bottom_field_flag;
    unsigned order[32], count = 0;
    out[0] = 0;
    for (int longTerm = 0; longTerm < 2; longTerm++) {
        unsigned stores[16], n = 0;
        for (unsigned i = 0; i < 16; i++) {
            const nvdec_dpb_entry_s *e = &p->dpb[i];
            const unsigned kind = longTerm ? 2 : 1;
            const bool top = e->top_field_marking == kind, bottom = e->bottom_field_marking == kind;
            if (p->field_pic_flag ? (top || bottom) : (top && bottom))
                stores[n++] = i;
        }
        for (unsigned a = 0; a < n; a++) {  // sort: short by FrameNumWrap descending, long by index ascending
            for (unsigned b = a + 1; b < n; b++) {
                int ka = p->dpb[stores[a]].FrameIdx, kb = p->dpb[stores[b]].FrameIdx;
                if (!longTerm) {
                    ka = ka > (int)p->frame_num ? ka - maxFrameNum : ka;
                    kb = kb > (int)p->frame_num ? kb - maxFrameNum : kb;
                }
                if (longTerm ? kb < ka : kb > ka) {
                    const unsigned t = stores[a];
                    stores[a] = stores[b];
                    stores[b] = t;
                }
            }
        }
        if (!p->field_pic_flag) {
            for (unsigned a = 0; a < n; a++)
                order[count++] = stores[a] << 2 | 3;
            continue;
        }
        const unsigned kind = longTerm ? 2 : 1;
        unsigned next[2] = {0, 0};
        for (unsigned want = parity;;) {
            unsigned *at = &next[want];
            while (*at < n &&
                   (want ? p->dpb[stores[*at]].bottom_field_marking : p->dpb[stores[*at]].top_field_marking) != kind)
                (*at)++;
            if (*at < n) {
                order[count++] = stores[(*at)++] << 2 | (want ? 2 : 1);
                want ^= 1;
                continue;
            }
            unsigned *other = &next[want ^ 1];
            while (*other < n && (want ? p->dpb[stores[*other]].top_field_marking
                                       : p->dpb[stores[*other]].bottom_field_marking) != kind)
                (*other)++;
            if (*other >= n)
                break;
            want ^= 1;
        }
    }
    for (unsigned i = 0; i < count && i < active; i++) {
        const nvdec_dpb_entry_s *e = &p->dpb[order[i] >> 2];
        char item[24];
        if (e->not_existing)
            snprintf(item, sizeof(item), "-");
        else if ((order[i] & 3) == 3)
            snprintf(item, sizeof(item), "%s/%s", names[e->index][0], names[e->index][1]);
        else
            snprintf(item, sizeof(item), "%s", names[e->index][(order[i] & 3) == 2]);
        append(out, size, item);
    }
}

static bool resets(const char *mmco)
{
    return mmco && (!strncmp(mmco, "5", 1) || strstr(mmco, " 5"));
}

// The order counts NVDEC is given (no pic_order_cnt_lsb wraps here, so a count is its lsb; a frame's bottom field is
// one later): the picture's own, and a second field's first field's (0 for a first field's other field); each dpb[]
// field's as decoded, 0 for a field never decoded and counted from the picture after its memory_management_control
// operation 5; and in a second field's own-surface entry, its first field's count in both slots. counts: each
// surface's fields as decoded, updated after the picture.
static int orderCounts(const FieldsCase *c, unsigned j, const nvdec_h264_pic_s *p, bool second, int32_t counts[17][2])
{
    const FieldsPicture *pic = &c->steps[j].picture;
    const unsigned parity = pic->structure == FS_BOTTOM;
    int32_t want[2] = {pic->poc_lsb, pic->poc_lsb + 1};
    if (pic->structure != FS_FRAME) {
        want[parity] = pic->poc_lsb;
        want[!parity] = second ? counts[p->CurrPicIdx][!parity] : 0;
    }
    int bad = p->CurrFieldOrderCnt[0] != want[0] || p->CurrFieldOrderCnt[1] != want[1];
    for (unsigned i = 0; i < 16; i++) {
        const nvdec_dpb_entry_s *e = &p->dpb[i];
        if (!e->state || e->not_existing)
            continue;
        const bool own = second && e->index == p->CurrPicIdx;
        for (unsigned f = 0; f < 2; f++)
            bad += (int32_t)e->FieldOrderCnt[f] != counts[e->index][own ? !parity : f];
    }
    if (bad)
        fprintf(stderr, "%s: picture %u: order counts %d %d (want %d %d) or a dpb[] entry's differ\n", c->name, j,
                p->CurrFieldOrderCnt[0], p->CurrFieldOrderCnt[1], want[0], want[1]);
    if (!second)
        counts[p->CurrPicIdx][0] = counts[p->CurrPicIdx][1] = 0;
    for (unsigned f = 0; f < 2; f++)
        if (pic->structure & (1u << f))
            counts[p->CurrPicIdx][f] = !resets(pic->mmco) ? want[f] : pic->structure == FS_FRAME ? (int32_t)f : 0;
    return bad;
}

static int runCase(const FieldsCase *c, unsigned *pictures, unsigned *frames)
{
    FieldsPicture pics[64];
    const FieldsScenario s = scenario(c, pics);
    FieldsStream stream;
    if (fields_stream_write(&s, &stream))
        return 1;
    Fake fake = {0};
    const H264Backend backend = {.ctx = &fake,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeDiscard,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    H264Decoder *d = h264_decoder_create(&backend);
    int bad = 0;
    if (!d || h264_decoder_parameter_set(d, stream.sps, stream.sps_size) ||
        h264_decoder_parameter_set(d, stream.pps, stream.pps_size) || h264_decoder_configure(d)) {
        fprintf(stderr, "%s: configure refused the stream\n", c->name);
        return 1;
    }
    int answers[64];
    unsigned expectFrames = 0;
    for (unsigned i = 0; i < stream.sample_count; i++) {
        answers[i] = h264_decoder_decode(d, stream.samples + stream.sample_offset[i], stream.sample_size[i], 4,
                                         (void *)(uintptr_t)i, false);
        expectFrames += answers[i] == 1;
        if (answers[i] < 0) {
            fprintf(stderr, "%s: sample %u failed (%d)\n", c->name, i, answers[i]);
            bad++;
        }
    }
    h264_decoder_flush(d);
    h264_decoder_destroy(d);

    Name names[17][2];
    memset(names, 0, sizeof(names));
    int32_t counts[17][2];
    memset(counts, 0, sizeof(counts));
    unsigned firstSurface = 0, pairFrameNum = 0;
    bad += fake.job_count != c->count || fake.refused;
    for (unsigned j = 0; j < fake.job_count && j < c->count; j++) {
        const FieldsPicture *pic = &c->steps[j].picture, *prev = j ? &c->steps[j - 1].picture : NULL;
        const nvdec_h264_pic_s *p = &fake.jobs[j];
        const bool field = pic->structure != FS_FRAME;
        const bool second = field && prev && prev->structure != FS_FRAME && prev->structure != pic->structure &&
                            !fake.jobs[j - 1].second_field && pairFrameNum == pic->frame_num && !pic->idr &&
                            prev->ref == pic->ref;
        pairFrameNum = resets(pic->mmco) ? 0 : pic->frame_num;
        if (p->field_pic_flag != field || p->bottom_field_flag != (pic->structure == FS_BOTTOM) ||
            p->second_field != second || (second && p->CurrPicIdx != firstSurface) || p->MbaffFrameFlag ||
            p->frame_mbs_only_flag || p->FrameHeightInMbs != 2) {
            fprintf(stderr, "%s: picture %u: field %u bottom %u second %u surface %u (want %u %u %u %u)\n", c->name, j,
                    p->field_pic_flag, p->bottom_field_flag, p->second_field, p->CurrPicIdx, field,
                    pic->structure == FS_BOTTOM, second, firstSurface);
            bad++;
        }
        firstSurface = p->CurrPicIdx;
        bad += orderCounts(c, j, p, second, counts);
        if (c->steps[j].list0) {
            char got[256];
            list0(p, pic->probes, names, got, sizeof(got));
            if (strcmp(got, c->steps[j].list0)) {
                fprintf(stderr, "%s: picture %u: RefPicList0 \"%s\", FFmpeg's \"%s\"\n", c->name, j, got,
                        c->steps[j].list0);
                bad++;
            }
        }
        for (unsigned f = 0; f < 2; f++)
            if (pic->structure & (1u << f))
                snprintf(names[p->CurrPicIdx][f], sizeof(Name), field ? "%u" : f ? "%u'" : "%u", j);
    }
    // Every sample whose token a frame took is answered once, in decoding order; the others not at all.
    unsigned next = 0;
    for (unsigned e = 0; e < fake.emits; e++) {
        while (next < stream.sample_count && answers[next] != 1)
            next++;
        if (next >= stream.sample_count || fake.emitted[e] != next) {
            fprintf(stderr, "%s: answer %u is sample %u\n", c->name, e, fake.emitted[e]);
            bad++;
            break;
        }
        next++;
    }
    bad += fake.emits != expectFrames;
    // Asynchronous decoding: no wait, no CPU write into an input a queued job still reads, and a field pair's frame
    // waits for both its jobs.
    if (fake.engine.waits || fake.engine.busyWrites || fake.pairJobsWrong) {
        fprintf(stderr, "%s: %u waits, %u CPU writes into a buffer a queued job uses, %u field pairs' jobs wrong\n",
                c->name, fake.engine.waits, fake.engine.busyWrites, fake.pairJobsWrong);
        bad++;
    }
    // A frame's rows interleave as its first field's parity says (a frame picture's top field is the earlier).
    for (unsigned j = 0, sample = 0; j < c->count && j < fake.job_count; j++) {
        sample += j && c->steps[j].picture.new_sample;
        const uint8_t want =
            c->steps[j].picture.structure == FS_BOTTOM ? AVD_FIELDS_BOTTOM_FIRST : AVD_FIELDS_TOP_FIRST;
        if (!fake.jobs[j].second_field && answers[sample] == 1 && fake.fields[sample] != want) {
            fprintf(stderr, "%s: sample %u fields %u, want %u\n", c->name, sample, fake.fields[sample], want);
            bad++;
        }
    }
    *pictures += fake.job_count;
    *frames += fake.emits;
    fields_stream_free(&stream);
    return bad;
}

// The setups the checker must refuse, each from an accepted field job (the first one, an IDR top field, and the
// second, whose dpb[] holds its first field).
static int refusals(const TegraNVDECSubmit *first, const uint64_t *avail, const nvdec_h264_pic_s *second,
                    unsigned *cases)
{
    int bad = 0;
    for (int c = 0; c <= 11; c++) {
        TegraNVDECSubmit s = *first;
        nvdec_h264_pic_s *p = (nvdec_h264_pic_s *)s.setup;
        const char *expected = "picture structure";
        switch (c) {
        case 0:
            expected = NULL;
            break;
        case 1:
            *p = *second, expected = NULL;
            break;
        case 2:
            p->frame_mbs_only_flag = 1;  // a field of a progressive stream
            break;
        case 3:
            p->MbaffFrameFlag = 1;  // MBAFF is a frame's
            break;
        case 4:
            p->field_pic_flag = 0;  // bottom_field_flag on a frame
            p->bottom_field_flag = 1;
            break;
        case 5:
            *p = *second, p->field_pic_flag = 0, p->bottom_field_flag = 0;  // second_field on a frame
            break;
        case 6:
            p->FrameHeightInMbs = 3;  // half a macroblock pair
            break;
        case 7:
            p->frame_surfaces = 1, expected = "surface format";
            break;
        case 8:
            p->luma_bot_offset = 1, expected = "surface offsets";
            break;
        case 9:
            *p = *second, p->dpb[0].top_field_marking = 3, expected = "dpb entry";
            break;
        case 10:
            *p = *second, p->dpb[0].state = 0, expected = "dpb entry";  // marked, yet not used for reference
            break;
        case 11:
            *p = *second, p->frame_mbs_only_flag = 1, p->field_pic_flag = p->bottom_field_flag = p->second_field = 0;
            expected = "dpb entry";  // a field-coded reference in a progressive stream
            break;
        }
        const char *why = tegra_nvdec_check(&s, avail);
        if ((why == NULL) != (expected == NULL) || (why && strcmp(why, expected))) {
            fprintf(stderr, "refusal case %d: checker said \"%s\", expected \"%s\"\n", c, why ? why : "accepted",
                    expected ? expected : "accepted");
            bad++;
        }
        ++*cases;
    }
    return bad;
}

static const FieldsCase *named(const char *name)
{
    for (unsigned i = 0; i < sizeof(kFieldsCases) / sizeof(kFieldsCases[0]); i++)
        if (!strcmp(kFieldsCases[i].name, name))
            return &kFieldsCases[i];
    return NULL;
}

static void name(char *out, size_t size, int value)
{
    const int id = (value - 16) / 3, rest = (value - 16) % 3;
    snprintf(out, size, value < 16 || rest == 2 ? "?%d" : rest ? "%d'" : "%d", value < 16 || rest == 2 ? value : id);
}

// --oracle: each P picture's probes in FFmpeg's output frames, found by the level of its first macroblock.
static int oracle(const FieldsCase *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 2;
    static uint8_t yuv[256][WIDTH * HEIGHT * 3 / 2];
    unsigned frames = 0;
    while (frames < 256 && fread(yuv[frames], sizeof(yuv[0]), 1, fp) == 1)
        frames++;
    fclose(fp);
    for (unsigned j = 0; j < c->count; j++) {
        const FieldsPicture *pic = &c->steps[j].picture;
        if (pic->type != 'P')
            continue;
        const unsigned row = pic->structure == FS_BOTTOM;
        const int level = 16 + 3 * (int)j;
        const uint8_t *frame = NULL;
        for (unsigned f = 0; f < frames && !frame; f++)
            if (yuv[f][row * WIDTH] == level && (pic->structure != FS_FRAME || yuv[f][WIDTH] == level + 1))
                frame = yuv[f];
        char list[256] = "";
        for (unsigned x = 1; frame && x <= pic->probes; x++) {
            char item[24], top[8], bottom[8];
            name(top, sizeof(top), frame[row * WIDTH + x * 16 + 8]);
            name(bottom, sizeof(bottom), frame[WIDTH + x * 16 + 8]);
            if (pic->structure == FS_FRAME)
                snprintf(item, sizeof(item), "%s/%s", top, bottom);
            else
                snprintf(item, sizeof(item), "%s", top);
            append(list, sizeof(list), item);
        }
        printf("%s picture %u: \"%s\"\n", c->name, j, frame ? list : "not in FFmpeg's output");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && (!strcmp(argv[1], "--dump") || !strcmp(argv[1], "--oracle"))) {
        const FieldsCase *c = named(argv[2]);
        if (!c)
            return 2;
        if (!strcmp(argv[1], "--oracle"))
            return oracle(c, argv[3]);
        FieldsPicture pics[64];
        const FieldsScenario s = scenario(c, pics);
        FieldsStream stream;
        FILE *fp = fopen(argv[3], "wb");
        const int ok = fp && !fields_stream_write(&s, &stream) &&
                       fwrite(stream.annexb, 1, stream.annexb_size, fp) == stream.annexb_size;
        if (fp)
            fclose(fp);
        return ok ? 0 : 1;
    }
    unsigned pictures = 0, frames = 0, cases = 0;
    int bad = 0;
    const unsigned count = sizeof(kFieldsCases) / sizeof(kFieldsCases[0]);
    for (unsigned i = 0; i < count; i++)
        bad += runCase(&kFieldsCases[i], &pictures, &frames);
    // The refusal cases start from the first stream's first two jobs.
    FieldsPicture pics[64];
    const FieldsScenario s = scenario(&kFieldsCases[0], pics);
    FieldsStream stream;
    Fake fake = {0};
    const H264Backend backend = {.ctx = &fake,
                                 .alloc = fakeAlloc,
                                 .free = fakeFree,
                                 .submit = fakeSubmit,
                                 .wait = fakeWait,
                                 .decoded = fakeDecoded,
                                 .emit = fakeEmit,
                                 .discard = fakeDiscard,
                                 .inputs = FAKE_ENGINE_DEPTH + 1};
    H264Decoder *d = h264_decoder_create(&backend);
    if (fields_stream_write(&s, &stream) || !d || h264_decoder_parameter_set(d, stream.sps, stream.sps_size) ||
        h264_decoder_parameter_set(d, stream.pps, stream.pps_size) || h264_decoder_configure(d) ||
        h264_decoder_decode(d, stream.samples, stream.sample_size[0], 4, NULL, false) != 1 || fake.job_count != 2)
        return 1;
    const int refused = refusals(&fake.first, fake.first_avail, &fake.jobs[1], &cases);
    h264_decoder_destroy(d);
    fields_stream_free(&stream);
    const bool pass = !bad && !refused;
    printf("%s h264 fields: %u streams, %u pictures, %u frames, %d mismatches, %d of %u refusal cases wrong\n",
           pass ? "PASS" : "FAIL", count, pictures, frames, bad, refused, cases);
    return pass ? 0 : 1;
}
