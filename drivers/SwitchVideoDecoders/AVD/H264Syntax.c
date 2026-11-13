// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 syntax parsing; see H264Syntax.h. Clause numbers are ITU-T H.264 (08/2021).
#include "H264Syntax.h"
#include <string.h>

typedef struct {
    uint8_t data[1024];
    size_t size, bit;
    bool overrun;
} Reader;

// 7.4.1: drop emulation_prevention_three_byte after the NAL header. Slice headers end well inside 1 KiB; parameter
// sets larger than that are refused rather than truncated.
static bool load(Reader *r, const uint8_t *nal, size_t size, bool allowTruncate)
{
    size_t zeros = 0;
    r->size = r->bit = 0;
    r->overrun = false;
    for (size_t i = 1; i < size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        if (r->size == sizeof(r->data))
            return allowTruncate;
        zeros = nal[i] ? 0 : zeros + 1;
        r->data[r->size++] = nal[i];
    }
    return r->size > 0;
}

static uint32_t bits(Reader *r, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r->bit >= r->size * 8) {
            r->overrun = true;
            return 0;
        }
        v = (v << 1) | ((r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1);
        r->bit++;
    }
    return v;
}

static bool flag(Reader *r)
{
    return bits(r, 1);
}

static uint32_t ue(Reader *r)
{
    unsigned zeros = 0;
    while (!r->overrun && !bits(r, 1)) {
        if (++zeros > 31) {
            r->overrun = true;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1) + bits(r, zeros) : 0;
}

static int32_t se(Reader *r)
{
    const uint32_t k = ue(r);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

// 7.2 more_rbsp_data(): something other than the rbsp_stop_one_bit and its trailing zeros remains.
static bool moreData(const Reader *r)
{
    size_t last = r->size;
    while (last && !r->data[last - 1])
        last--;
    if (!last)
        return false;
    const unsigned trailing = (unsigned)__builtin_ctz(r->data[last - 1]);
    return r->bit < (last - 1) * 8 + (7 - trailing);
}

static const uint8_t kZigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t kZigzag8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                     12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                     58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
// Table 7-3 and 7-4, in zigzag order.
static const uint8_t kDefault4[2][16] = {{6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42},
                                         {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34}};
static const uint8_t kDefault8[2][64] = {
    {6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
     25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31,
     31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42},
    {9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22,
     22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27,
     27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35}};

static void defaultList(uint8_t *out, unsigned size, bool inter)
{
    for (unsigned j = 0; j < size; j++)
        out[size == 16 ? kZigzag4[j] : kZigzag8[j]] = size == 16 ? kDefault4[inter][j] : kDefault8[inter][j];
}

// 7.3.2.1.1.1 scaling_list(); returns false for useDefaultScalingMatrixFlag. Stored in raster order.
static bool scalingList(Reader *r, uint8_t *out, unsigned size)
{
    unsigned last = 8, next = 8;
    for (unsigned j = 0; j < size; j++) {
        if (next) {
            next = (last + (unsigned)se(r) + 256) % 256;
            if (!j && !next)
                return false;
        }
        out[size == 16 ? kZigzag4[j] : kZigzag8[j]] = (uint8_t)(next ? next : last);
        last = next ? next : last;
    }
    return true;
}

// The scaling lists of an SPS or a PPS with fall-back rule A (fallback == NULL) or B (fallback = the SPS's lists),
// Table 7-2. Only the two 8x8 luma lists matter for 4:2:0, the only chroma format accepted downstream.
static void scalingMatrices(Reader *r, unsigned lists8, uint8_t s4[6][16], uint8_t s8[2][64], const uint8_t (*fb4)[16],
                            const uint8_t (*fb8)[64])
{
    for (unsigned i = 0; i < 6; i++) {
        if (flag(r)) {
            if (!scalingList(r, s4[i], 16))
                defaultList(s4[i], 16, i >= 3);
        } else if (i == 0 || i == 3) {
            if (fb4)
                memcpy(s4[i], fb4[i], 16);
            else
                defaultList(s4[i], 16, i == 3);
        } else {
            memcpy(s4[i], s4[i - 1], 16);
        }
    }
    for (unsigned i = 0; i < lists8; i++) {
        uint8_t scratch[64];
        uint8_t *dst = i < 2 ? s8[i] : scratch;  // 4:4:4 chroma lists are parsed and dropped
        if (flag(r)) {
            if (!scalingList(r, dst, 64))
                defaultList(dst, 64, i & 1);
        } else if (i < 2) {
            if (fb8)
                memcpy(dst, fb8[i], 64);
            else
                defaultList(dst, 64, i & 1);
        }
    }
}

static void flat(uint8_t s4[6][16], uint8_t s8[2][64])
{
    memset(s4, 16, 6 * 16);
    memset(s8, 16, 2 * 64);
}

static void hrd(Reader *r)
{
    const uint32_t count = ue(r) + 1;
    bits(r, 8);
    for (uint32_t i = 0; i < count && i < 32 && !r->overrun; i++) {
        ue(r);
        ue(r);
        flag(r);
    }
    bits(r, 20);
}

// E.1.1; a damaged VUI only loses the VUI.
static void vui(Reader *r, H264SPS *s)
{
    if (flag(r) && bits(r, 8) == 255)
        bits(r, 32);  // aspect_ratio_idc, Extended_SAR
    if (flag(r))
        flag(r);    // overscan
    if (flag(r)) {  // video_signal_type
        bits(r, 3);
        s->full_range = flag(r);
        if ((s->colour_description = flag(r))) {
            s->colour_primaries = (uint8_t)bits(r, 8);
            s->transfer = (uint8_t)bits(r, 8);
            s->matrix = (uint8_t)bits(r, 8);
        }
    }
    if ((s->chroma_location = flag(r))) {
        s->chroma_top = ue(r);
        s->chroma_bottom = ue(r);
    }
    if (flag(r))
        bits(r, 32), bits(r, 32), flag(r);  // timing_info
    const bool nal = flag(r);
    if (nal)
        hrd(r);
    const bool vcl = flag(r);
    if (vcl)
        hrd(r);
    if (nal || vcl)
        flag(r);  // low_delay_hrd_flag
    flag(r);      // pic_struct_present_flag
    if (flag(r)) {
        flag(r);
        ue(r), ue(r), ue(r), ue(r);
        const uint32_t reorder = ue(r), buffering = ue(r);
        if (!r->overrun && reorder <= 16 && buffering <= 16) {
            s->bitstream_restriction = true;
            s->max_num_reorder_frames = (uint8_t)reorder;
            s->max_dec_frame_buffering = (uint8_t)buffering;
        }
    }
    if (r->overrun) {
        s->full_range = s->colour_description = s->chroma_location = s->bitstream_restriction = false;
        r->overrun = false;
    }
}

int h264_parse_sps(H264ParamSets *sets, const uint8_t *nal, size_t size, uint8_t *idOut)
{
    Reader r;
    H264SPS s;
    memset(&s, 0, sizeof(s));
    if (!load(&r, nal, size, false))
        return -1;
    s.profile_idc = (uint8_t)bits(&r, 8);
    s.constraint_flags = (uint8_t)bits(&r, 8);
    s.level_idc = (uint8_t)bits(&r, 8);
    const uint32_t id = ue(&r);
    if (id >= 32)
        return -1;
    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    flat(s.scaling4, s.scaling8);
    switch (s.profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
        s.chroma_format_idc = (uint8_t)ue(&r);
        if (s.chroma_format_idc > 3)
            return -1;
        if (s.chroma_format_idc == 3)
            s.separate_colour_plane = flag(&r);
        s.bit_depth_luma = (uint8_t)(ue(&r) + 8);
        s.bit_depth_chroma = (uint8_t)(ue(&r) + 8);
        s.transform_bypass = flag(&r);
        if ((s.scaling_matrix_present = flag(&r)))
            scalingMatrices(&r, s.chroma_format_idc != 3 ? 2 : 6, s.scaling4, s.scaling8, NULL, NULL);
        break;
    default:
        break;
    }
    const uint32_t log2FrameNum = ue(&r) + 4;
    s.poc_type = (uint8_t)ue(&r);
    if (log2FrameNum > 16 || s.poc_type > 2)
        return -1;
    s.log2_max_frame_num = (uint8_t)log2FrameNum;
    if (s.poc_type == 0) {
        const uint32_t log2Poc = ue(&r) + 4;
        if (log2Poc > 16)
            return -1;
        s.log2_max_poc_lsb = (uint8_t)log2Poc;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = flag(&r);
        s.offset_for_non_ref_pic = se(&r);
        s.offset_for_top_to_bottom_field = se(&r);
        const uint32_t cycle = ue(&r);
        if (cycle > 255)
            return -1;
        s.num_ref_frames_in_poc_cycle = (uint8_t)cycle;
        for (uint32_t i = 0; i < cycle; i++)
            s.offset_for_ref_frame[i] = se(&r);
    }
    const uint32_t refs = ue(&r);
    if (refs > 16)
        return -1;
    s.max_num_ref_frames = (uint8_t)refs;
    s.gaps_in_frame_num_allowed = flag(&r);
    const uint32_t w = ue(&r) + 1, h = ue(&r) + 1;
    if (w > 1024 || h > 1024)
        return -1;
    s.width_mbs = (uint16_t)w;
    s.height_map_units = (uint16_t)h;
    if (!(s.frame_mbs_only = flag(&r)))
        s.mb_adaptive_frame_field = flag(&r);
    s.height_mbs = (uint16_t)(h * (2 - s.frame_mbs_only));
    s.direct_8x8_inference = flag(&r);
    if (flag(&r)) {
        const unsigned cx = s.chroma_format_idc == 1 || s.chroma_format_idc == 2 ? 2 : 1;
        const unsigned cy = (s.chroma_format_idc == 1 ? 2 : 1) * (2 - s.frame_mbs_only);
        s.crop_left = ue(&r) * cx;
        s.crop_right = ue(&r) * cx;
        s.crop_top = ue(&r) * cy;
        s.crop_bottom = ue(&r) * cy;
        if (s.crop_left + s.crop_right >= w * 16 || s.crop_top + s.crop_bottom >= h * 16 * (2 - s.frame_mbs_only))
            return -1;
    }
    if (r.overrun)
        return -1;
    if (flag(&r))
        vui(&r, &s);
    s.valid = true;
    sets->sps[id] = s;
    if (idOut)
        *idOut = (uint8_t)id;
    return 0;
}

int h264_parse_pps(H264ParamSets *sets, const uint8_t *nal, size_t size, uint8_t *idOut)
{
    Reader r;
    H264PPS p;
    memset(&p, 0, sizeof(p));
    if (!load(&r, nal, size, false))
        return -1;
    const uint32_t id = ue(&r), spsId = ue(&r);
    if (id >= 256 || spsId >= 32 || !sets->sps[spsId].valid)
        return -1;
    const H264SPS *s = &sets->sps[spsId];
    p.sps_id = (uint8_t)spsId;
    p.entropy_coding_mode = flag(&r);
    p.bottom_field_pic_order_present = flag(&r);
    const uint32_t groups = ue(&r) + 1;
    if (groups > 8)
        return -1;
    p.num_slice_groups = (uint8_t)groups;
    if (groups > 1) {  // FMO (Baseline only): parsed far enough to report, never decoded here
        if (idOut)
            *idOut = (uint8_t)id;
        sets->pps[id] = p;
        return 0;
    }
    const uint32_t l0 = ue(&r) + 1, l1 = ue(&r) + 1;
    if (l0 > 32 || l1 > 32)
        return -1;
    p.num_ref_idx_active[0] = (uint8_t)l0;
    p.num_ref_idx_active[1] = (uint8_t)l1;
    p.weighted_pred = flag(&r);
    p.weighted_bipred_idc = (uint8_t)bits(&r, 2);
    p.pic_init_qp = (int8_t)(26 + se(&r));
    se(&r);  // pic_init_qs_minus26
    p.chroma_qp_index_offset = (int8_t)se(&r);
    p.deblocking_filter_control_present = flag(&r);
    p.constrained_intra_pred = flag(&r);
    p.redundant_pic_cnt_present = flag(&r);
    p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
    memcpy(p.scaling4, s->scaling4, sizeof(p.scaling4));
    memcpy(p.scaling8, s->scaling8, sizeof(p.scaling8));
    if (moreData(&r)) {
        p.transform_8x8_mode = flag(&r);
        if (flag(&r)) {
            const unsigned lists8 = p.transform_8x8_mode ? (s->chroma_format_idc != 3 ? 2 : 6) : 0;
            if (s->scaling_matrix_present)
                scalingMatrices(&r, lists8, p.scaling4, p.scaling8, s->scaling4, s->scaling8);
            else
                scalingMatrices(&r, lists8, p.scaling4, p.scaling8, NULL, NULL);
            if (!p.transform_8x8_mode) {  // no 8x8 lists sent: fall back as for absent ones
                if (s->scaling_matrix_present)
                    memcpy(p.scaling8, s->scaling8, sizeof(p.scaling8));
                else
                    defaultList(p.scaling8[0], 64, false), defaultList(p.scaling8[1], 64, true);
            }
        }
        p.second_chroma_qp_index_offset = (int8_t)se(&r);
    }
    if (r.overrun || p.pic_init_qp < 0 || p.pic_init_qp > 51)
        return -1;
    p.valid = true;
    sets->pps[id] = p;
    if (idOut)
        *idOut = (uint8_t)id;
    return 0;
}

static bool refListModification(Reader *r)
{
    if (!flag(r))
        return true;
    for (unsigned n = 0; n < 33; n++) {
        const uint32_t idc = ue(r);
        if (idc == 3)
            return true;
        if (idc > 2 || r->overrun)
            return false;
        ue(r);
    }
    return false;
}

static void predWeightTable(Reader *r, unsigned chromaArrayType, const uint8_t count[2], bool b)
{
    ue(r);
    if (chromaArrayType)
        ue(r);
    for (unsigned list = 0; list < (b ? 2u : 1u); list++) {
        for (unsigned i = 0; i < count[list] && !r->overrun; i++) {
            if (flag(r))
                se(r), se(r);
            if (chromaArrayType && flag(r))
                se(r), se(r), se(r), se(r);
        }
    }
}

int h264_parse_slice(const H264ParamSets *sets, const uint8_t *nal, size_t size, H264Slice *out)
{
    Reader r;
    memset(out, 0, sizeof(*out));
    if (size < 2)
        return -1;
    out->nal_ref_idc = (nal[0] >> 5) & 3;
    out->nal_unit_type = nal[0] & 0x1f;
    if (!load(&r, nal, size, true))
        return -1;
    out->first_mb_in_slice = ue(&r);
    const uint32_t type = ue(&r);
    const uint32_t ppsId = ue(&r);
    if (type > 9 || ppsId >= 256 || !sets->pps[ppsId].valid)
        return -1;
    out->slice_type = (uint8_t)(type % 5);
    out->pps_id = (uint8_t)ppsId;
    const H264PPS *p = &sets->pps[ppsId];
    const H264SPS *s = &sets->sps[p->sps_id];
    if (!s->valid)
        return -1;
    const bool idr = out->nal_unit_type == H264_NAL_IDR;
    if (s->separate_colour_plane)
        bits(&r, 2);
    out->frame_num = bits(&r, s->log2_max_frame_num);
    if (!s->frame_mbs_only && (out->field_pic = flag(&r)))
        out->bottom_field = flag(&r);
    if (idr)
        out->idr_pic_id = ue(&r);
    if (s->poc_type == 0) {
        out->poc_lsb = bits(&r, s->log2_max_poc_lsb);
        if (p->bottom_field_pic_order_present && !out->field_pic)
            out->delta_poc_bottom = se(&r);
    }
    if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        out->delta_poc[0] = se(&r);
        if (p->bottom_field_pic_order_present && !out->field_pic)
            out->delta_poc[1] = se(&r);
    }
    if (p->redundant_pic_cnt_present)
        ue(&r);
    const bool b = out->slice_type == H264_SLICE_B;
    const bool intra = out->slice_type == H264_SLICE_I || out->slice_type == H264_SLICE_SI;
    if (b)
        flag(&r);  // direct_spatial_mv_pred_flag
    uint8_t count[2] = {p->num_ref_idx_active[0], p->num_ref_idx_active[1]};
    if (!intra && flag(&r)) {
        const uint32_t l0 = ue(&r) + 1, l1 = b ? ue(&r) + 1 : count[1];
        if (l0 > 32 || l1 > 32)
            return -1;
        count[0] = (uint8_t)l0;
        count[1] = (uint8_t)l1;
    }
    if (!intra && (!refListModification(&r) || (b && !refListModification(&r))))
        return -1;
    if ((p->weighted_pred && !intra && !b) || (p->weighted_bipred_idc == 1 && b))
        predWeightTable(&r, s->separate_colour_plane ? 0 : s->chroma_format_idc, count, b);
    if (out->nal_ref_idc) {
        if (idr) {
            out->no_output_of_prior_pics = flag(&r);
            out->long_term_reference = flag(&r);
        } else if ((out->adaptive_ref_pic_marking = flag(&r))) {
            for (;;) {
                const uint32_t op = ue(&r);
                if (!op)
                    break;
                if (op > 6 || out->mmco_count == 32 || r.overrun)
                    return -1;
                H264MMCO *m = &out->mmco[out->mmco_count++];
                m->type = (uint8_t)op;
                if (op == 1 || op == 3)
                    m->difference_of_pic_nums_minus1 = ue(&r);
                if (op == 2)
                    m->long_term_pic_num = ue(&r);
                if (op == 3 || op == 6)
                    m->long_term_frame_idx = ue(&r);
                if (op == 4)
                    m->max_long_term_frame_idx_plus1 = ue(&r);
            }
        }
    }
    return r.overrun ? -1 : 0;
}

bool h264_slice_new_picture(const H264Slice *a, const H264Slice *b)
{
    return !b->first_mb_in_slice || a->frame_num != b->frame_num || a->pps_id != b->pps_id ||
           a->field_pic != b->field_pic || a->bottom_field != b->bottom_field || !a->nal_ref_idc != !b->nal_ref_idc ||
           a->poc_lsb != b->poc_lsb || a->delta_poc_bottom != b->delta_poc_bottom ||
           a->delta_poc[0] != b->delta_poc[0] || a->delta_poc[1] != b->delta_poc[1] ||
           a->nal_unit_type != b->nal_unit_type || a->idr_pic_id != b->idr_pic_id;
}
