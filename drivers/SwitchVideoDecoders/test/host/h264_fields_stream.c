// SPDX-License-Identifier: BSD-2-Clause-Patent
// See h264_fields_stream.h. Clause numbers are ITU-T H.264 (08/2021).
#include "h264_fields_stream.h"
#include <stdlib.h>
#include <string.h>

enum {
    WIDTH_MBS = 16,
    LOG2_FRAME_NUM = 4,
    LOG2_POC_LSB = 8
};

typedef struct {
    uint8_t bytes[1 << 15];
    size_t bits;
} Bits;

static void put(Bits *b, uint32_t value, unsigned n)
{
    while (n--) {
        if (value >> n & 1)
            b->bytes[b->bits >> 3] |= (uint8_t)(0x80 >> (b->bits & 7));
        b->bits++;
    }
}

static void ue(Bits *b, uint32_t v)
{
    unsigned n = 0;
    while ((v + 1) >> (n + 1))
        n++;
    put(b, 0, n);
    put(b, v + 1, n + 1);
}

static void se(Bits *b, int32_t v)
{
    ue(b, v > 0 ? 2 * (uint32_t)v - 1 : 2 * (uint32_t)(-v));
}

static void trailing(Bits *b)
{
    put(b, 1, 1);
    while (b->bits & 7)
        put(b, 0, 1);
}

// 7.4.1: emulation_prevention_three_byte where the RBSP would form a start code.
static size_t escape(uint8_t *dst, uint8_t header, const Bits *b)
{
    size_t n = 0, zeros = 0;
    dst[n++] = header;
    for (size_t i = 0; i < b->bits / 8; i++) {
        if (zeros >= 2 && b->bytes[i] <= 3) {
            dst[n++] = 3;
            zeros = 0;
        }
        dst[n++] = b->bytes[i];
        zeros = b->bytes[i] ? 0 : zeros + 1;
    }
    return n;
}

static void sps(const FieldsScenario *s, Bits *b)
{
    put(b, 77, 8);  // Main
    put(b, 0, 8);
    put(b, 30, 8);
    ue(b, 0);
    ue(b, LOG2_FRAME_NUM - 4);
    ue(b, 0);  // pic_order_cnt_type
    ue(b, LOG2_POC_LSB - 4);
    ue(b, s->max_num_ref_frames);
    put(b, s->gaps, 1);
    ue(b, WIDTH_MBS - 1);
    ue(b, 0);      // one map unit: a macroblock pair
    put(b, 0, 1);  // frame_mbs_only_flag
    put(b, 0, 1);  // mb_adaptive_frame_field_flag
    put(b, 1, 1);  // direct_8x8_inference_flag
    put(b, 0, 1);
    put(b, 0, 1);
    trailing(b);
}

static void pps(Bits *b)
{
    ue(b, 0);
    ue(b, 0);
    put(b, 0, 1);  // CAVLC
    put(b, 1, 1);  // bottom_field_pic_order_in_frame_present_flag
    ue(b, 0);
    ue(b, 0);
    ue(b, 0);
    put(b, 0, 3);  // weighted_pred_flag, weighted_bipred_idc
    se(b, 0);
    se(b, 0);
    se(b, 0);
    put(b, 1, 1);  // deblocking_filter_control_present_flag
    put(b, 0, 2);
    trailing(b);
}

static void mmcos(Bits *b, const char *text)
{
    for (const char *p = text; *p;) {
        char *end;
        const unsigned op = (unsigned)strtoul(p, &end, 10);
        unsigned args[2] = {0, 0}, count = 0;
        p = end;
        while (*p == ':' && count < 2) {
            args[count++] = (unsigned)strtoul(p + 1, &end, 10);
            p = end;
        }
        while (*p == ' ')
            p++;
        ue(b, op);
        for (unsigned i = 0; i < count; i++)
            ue(b, args[i]);
    }
    ue(b, 0);
}

// A frame picture's odd rows (its bottom field) one level up, so a field reference to either is told apart.
static void pcm(Bits *b, uint8_t luma, bool frame)
{
    while (b->bits & 7)
        put(b, 0, 1);
    for (unsigned i = 0; i < 256; i++)
        put(b, luma + (frame && (i / 16) % 2), 8);
    for (unsigned i = 0; i < 128; i++)
        put(b, 128, 8);
}

static void slice(const FieldsPicture *pic, uint8_t level, Bits *b)
{
    const bool p = pic->type == 'P', field = pic->structure != FS_FRAME;
    ue(b, 0);
    ue(b, p ? 0 : 2);
    ue(b, 0);
    put(b, pic->frame_num, LOG2_FRAME_NUM);
    put(b, field, 1);
    if (field)
        put(b, pic->structure == FS_BOTTOM, 1);
    if (pic->idr)
        ue(b, 0);
    put(b, pic->poc_lsb, LOG2_POC_LSB);
    if (!field)
        se(b, 1);  // delta_pic_order_cnt_bottom: the bottom field later
    if (p) {
        put(b, 1, 1);  // num_ref_idx_active_override_flag
        ue(b, pic->probes ? pic->probes - 1u : 0);
        put(b, 0, 1);  // ref_pic_list_modification_flag_l0
    }
    if (pic->ref) {
        if (pic->idr) {
            put(b, 0, 1);
            put(b, pic->long_term, 1);
        } else {
            put(b, pic->mmco != NULL, 1);
            if (pic->mmco)
                mmcos(b, pic->mmco);
        }
    }
    se(b, 0);  // slice_qp_delta
    ue(b, 1);  // disable_deblocking_filter_idc
    const unsigned rows = field ? 1 : 2;
    for (unsigned mb = 0; mb < WIDTH_MBS * rows; mb++) {
        const unsigned x = mb % WIDTH_MBS;
        const bool probe = p && mb < WIDTH_MBS && x >= 1 && x <= pic->probes;
        if (p)
            ue(b, 0);  // mb_skip_run
        if (!probe) {
            ue(b, p ? 30 : 25);  // I_PCM
            pcm(b, level, !field);
            continue;
        }
        ue(b, 0);  // P_L0_16x16
        if (pic->probes == 2)
            put(b, x - 1 == 0, 1);  // te(v) with cMax 1
        else if (pic->probes > 2)
            ue(b, x - 1);
        // mvp is the left macroblock's vector (8.4.1.3: B and C lie above the picture's top row): 0 beside the I_PCM
        // block, -16 x (x - 1) after it; the vector is -16 x x.
        se(b, -64);
        se(b, 0);
        ue(b, 0);  // coded_block_pattern 0
    }
    trailing(b);
}

static int append(FieldsStream *out, size_t *capacity, uint8_t header, const Bits *b, bool sample)
{
    uint8_t *nal = malloc(b->bits / 8 * 3 / 2 + 8);
    if (!nal)
        return -1;
    const size_t n = escape(nal, header, b);
    if (out->annexb_size + n + 8 > *capacity) {
        free(nal);
        return -1;
    }
    memcpy(out->annexb + out->annexb_size, "\0\0\0\1", 4);
    memcpy(out->annexb + out->annexb_size + 4, nal, n);
    out->annexb_size += n + 4;
    if (sample) {
        const unsigned s = out->sample_count - 1;
        uint8_t *dst = out->samples + out->sample_offset[s] + out->sample_size[s];
        dst[0] = (uint8_t)(n >> 24), dst[1] = (uint8_t)(n >> 16), dst[2] = (uint8_t)(n >> 8), dst[3] = (uint8_t)n;
        memcpy(dst + 4, nal, n);
        out->sample_size[s] += n + 4;
    }
    free(nal);
    return 0;
}

int fields_stream_write(const FieldsScenario *s, FieldsStream *out)
{
    memset(out, 0, sizeof(*out));
    size_t capacity = 64u << 20;
    out->annexb = malloc(capacity);
    out->samples = malloc(capacity);
    static Bits b;
    memset(&b, 0, sizeof(b));
    sps(s, &b);
    out->sps_size = escape(out->sps, 0x67, &b);
    memset(&b, 0, sizeof(b));
    pps(&b);
    out->pps_size = escape(out->pps, 0x68, &b);
    if (!out->annexb || !out->samples)
        return -1;
    memcpy(out->annexb, "\0\0\0\1", 4);
    memcpy(out->annexb + 4, out->sps, out->sps_size);
    memcpy(out->annexb + 4 + out->sps_size, "\0\0\0\1", 4);
    memcpy(out->annexb + 8 + out->sps_size, out->pps, out->pps_size);
    out->annexb_size = 8 + out->sps_size + out->pps_size;
    size_t used = 0;
    for (unsigned i = 0; i < s->count; i++) {
        const FieldsPicture *pic = &s->pictures[i];
        if (pic->new_sample || !out->sample_count) {
            if (out->sample_count == 64)
                return -1;
            if (out->sample_count)
                used = out->sample_offset[out->sample_count - 1] + out->sample_size[out->sample_count - 1];
            out->sample_offset[out->sample_count++] = used;
        }
        memset(&b, 0, sizeof(b));
        slice(pic, (uint8_t)(16 + 3 * i), &b);
        const uint8_t header = (uint8_t)((pic->ref ? 3 : 0) << 5 | (pic->idr ? 5 : 1));
        if (append(out, &capacity, header, &b, true))
            return -1;
    }
    return 0;
}

void fields_stream_free(FieldsStream *stream)
{
    free(stream->annexb);
    free(stream->samples);
}
