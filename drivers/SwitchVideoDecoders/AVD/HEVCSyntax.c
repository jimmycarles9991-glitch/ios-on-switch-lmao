// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCSyntax.h. Clause numbers are ITU-T H.265 (V8).
#include "HEVCSyntax.h"
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t bits, pos;
    bool bad;
} Bits;

static uint32_t u(Bits *b, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (b->pos >= b->bits) {
            b->bad = true;
            return 0;
        }
        v = v << 1 | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t ue(Bits *b)
{
    unsigned zeros = 0;
    while (!u(b, 1)) {
        if (b->bad || ++zeros > 31) {
            b->bad = true;
            return 0;
        }
    }
    return zeros ? (uint32_t)((1ull << zeros) - 1 + u(b, zeros)) : 0;
}

static int32_t se(Bits *b)
{
    const uint32_t k = ue(b);
    return k & 1 ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

static unsigned ceilLog2(uint32_t v)
{
    unsigned n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

size_t hevc_unescape(const uint8_t *nal, size_t size, uint8_t *out, size_t cap)
{
    size_t n = 0, zeros = 0;
    for (size_t i = 0; i < size && n < cap; i++) {
        if (zeros >= 2 && nal[i] == 3) {  // emulation_prevention_three_byte
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        out[n++] = nal[i];
    }
    return n;
}

// Scaling lists (7.3.4, 7.4.5). Table 7-6's 8x8 defaults laid out in raster order (the up-right diagonal scan of
// 6.5.3 applied to its values).
static const uint8_t kDefaultIntra[64] = {16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25,
                                          16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
                                          17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65,
                                          21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115};
static const uint8_t kDefaultInter[64] = {16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25,
                                          16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
                                          17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54,
                                          20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91};

// 6.5.3: the raster position of scan position i in an n x n block (n = 4 or 8).
static unsigned diagonal(unsigned n, unsigned i)
{
    unsigned k = 0;
    for (unsigned d = 0; d < 2 * n - 1; d++)
        for (int y = (int)d; y >= 0; y--) {
            const unsigned x = d - (unsigned)y;
            if ((unsigned)y < n && x < n) {
                if (k == i)
                    return (unsigned)y * n + x;
                k++;
            }
        }
    return 0;
}

void hevc_default_scaling_list(HEVCScalingList *sl)
{
    memset(sl, 16, sizeof(*sl));
    for (int size = 1; size < 4; size++)
        for (int m = 0; m < 6; m++)
            memcpy(sl->sl[size][m], m < 3 ? kDefaultIntra : kDefaultInter, 64);
}

static void scalingListData(Bits *b, HEVCScalingList *sl)
{
    hevc_default_scaling_list(sl);
    for (int size = 0; size < 4; size++)
        for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
            const unsigned coefs = size ? 64 : 16;
            if (!u(b, 1)) {  // scaling_list_pred_mode_flag 0: a copy, or the default (delta 0)
                const uint32_t delta = ue(b) * (size == 3 ? 3 : 1);
                if (delta > (uint32_t)m) {
                    b->bad = true;
                    return;
                }
                if (delta) {
                    memcpy(sl->sl[size][m], sl->sl[size][m - delta], coefs);
                    if (size > 1)
                        sl->dc[size - 2][m] = sl->dc[size - 2][m - delta];
                } else {
                    if (size)
                        memcpy(sl->sl[size][m], m < 3 ? kDefaultIntra : kDefaultInter, 64);
                    else
                        memset(sl->sl[size][m], 16, 16);
                    if (size > 1)
                        sl->dc[size - 2][m] = 16;
                }
                continue;
            }
            int next = 8;
            if (size > 1) {
                const int32_t dc = se(b) + 8;  // scaling_list_dc_coef_minus8 + 8
                if (dc < 1 || dc > 255) {
                    b->bad = true;
                    return;
                }
                next = dc;
                sl->dc[size - 2][m] = (uint8_t)dc;
            }
            for (unsigned i = 0; i < coefs; i++) {
                next = (next + se(b) + 256) % 256;
                sl->sl[size][m][diagonal(size ? 8 : 4, i)] = (uint8_t)next;
            }
        }
}

const HEVCScalingList *hevc_scaling_list(const HEVCSPS *sps, const HEVCPPS *pps)
{
    return pps->scaling_list_present ? &pps->scaling : &sps->scaling;
}

// Reference picture sets (7.3.7, 7.4.8).
static bool stRefPicSet(Bits *b, const HEVCShortTermRPS *sets, unsigned idx, unsigned count, HEVCShortTermRPS *out)
{
    memset(out, 0, sizeof(*out));
    if (idx && u(b, 1)) {  // inter_ref_pic_set_prediction_flag
        const uint32_t deltaIdx = idx == count ? ue(b) + 1 : 1;
        if (deltaIdx > idx)
            return false;
        const HEVCShortTermRPS *ref = &sets[idx - deltaIdx];
        const int sign = (int)u(b, 1);
        const int32_t deltaRps = (1 - 2 * sign) * (int32_t)(ue(b) + 1);
        if (deltaRps < -32768 || deltaRps > 32768)
            return false;
        const unsigned refNeg = ref->num_negative, refPos = ref->num_positive, refAll = refNeg + refPos;
        uint8_t used[17], useDelta[17];
        for (unsigned j = 0; j <= refAll; j++) {
            used[j] = (uint8_t)u(b, 1);
            useDelta[j] = used[j] ? 1 : (uint8_t)u(b, 1);
        }
        unsigned i = 0;  // (7-61)
        for (int j = (int)refPos - 1; j >= 0; j--) {
            const int32_t d = ref->delta[1][j] + deltaRps;
            if (d < 0 && useDelta[refNeg + (unsigned)j] && i < 16)
                out->delta[0][i] = (int16_t)d, out->used[0][i++] = used[refNeg + (unsigned)j];
        }
        if (deltaRps < 0 && useDelta[refAll] && i < 16)
            out->delta[0][i] = (int16_t)deltaRps, out->used[0][i++] = used[refAll];
        for (unsigned j = 0; j < refNeg; j++) {
            const int32_t d = ref->delta[0][j] + deltaRps;
            if (d < 0 && useDelta[j] && i < 16)
                out->delta[0][i] = (int16_t)d, out->used[0][i++] = used[j];
        }
        out->num_negative = (uint8_t)i;
        i = 0;  // (7-62)
        for (int j = (int)refNeg - 1; j >= 0; j--) {
            const int32_t d = ref->delta[0][j] + deltaRps;
            if (d > 0 && useDelta[j] && i < 16)
                out->delta[1][i] = (int16_t)d, out->used[1][i++] = used[j];
        }
        if (deltaRps > 0 && useDelta[refAll] && i < 16)
            out->delta[1][i] = (int16_t)deltaRps, out->used[1][i++] = used[refAll];
        for (unsigned j = 0; j < refPos; j++) {
            const int32_t d = ref->delta[1][j] + deltaRps;
            if (d > 0 && useDelta[refNeg + j] && i < 16)
                out->delta[1][i] = (int16_t)d, out->used[1][i++] = used[refNeg + j];
        }
        out->num_positive = (uint8_t)i;
        return out->num_negative + out->num_positive <= 16 && !b->bad;
    }
    const uint32_t neg = ue(b), pos = ue(b);
    if (neg > 16 || pos > 16 || neg + pos > 16)
        return false;
    out->num_negative = (uint8_t)neg, out->num_positive = (uint8_t)pos;
    int32_t poc = 0;
    for (unsigned i = 0; i < neg; i++) {
        poc -= (int32_t)ue(b) + 1;
        out->delta[0][i] = (int16_t)poc;
        out->used[0][i] = (uint8_t)u(b, 1);
    }
    poc = 0;
    for (unsigned i = 0; i < pos; i++) {
        poc += (int32_t)ue(b) + 1;
        out->delta[1][i] = (int16_t)poc;
        out->used[1][i] = (uint8_t)u(b, 1);
    }
    return !b->bad && poc > -32768 && poc < 32768;
}

static bool begin(Bits *b, uint8_t *rbsp, size_t cap, const uint8_t *nal, size_t size)
{
    const size_t n = hevc_unescape(nal, size, rbsp, cap);
    b->p = rbsp, b->bits = n * 8, b->pos = 16, b->bad = n < 3;  // after the two-byte NAL unit header
    return !b->bad;
}

// 7.3.3: general profile, tier and level (the sub-layers' are skipped). Returns the profile this decoder sees: the
// general profile_idc, or the lowest of 1..3 a compatibility flag names.
static unsigned profileTierLevel(Bits *b, unsigned maxSubLayersMinus1, uint8_t *level)
{
    u(b, 2), u(b, 1);  // general_profile_space, general_tier_flag
    unsigned idc = u(b, 5), compatible = 0;
    for (unsigned j = 0; j < 32; j++)
        if (u(b, 1) && j >= 1 && j <= 3 && !compatible)
            compatible = j;
    u(b, 4), u(b, 32), u(b, 12);  // source flags, 43 constraint/reserved bits, general_inbld_flag
    *level = (uint8_t)u(b, 8);
    uint8_t profilePresent[8] = {0}, levelPresent[8] = {0};
    for (unsigned i = 0; i < maxSubLayersMinus1; i++)
        profilePresent[i] = (uint8_t)u(b, 1), levelPresent[i] = (uint8_t)u(b, 1);
    if (maxSubLayersMinus1)
        for (unsigned i = maxSubLayersMinus1; i < 8; i++)
            u(b, 2);
    for (unsigned i = 0; i < maxSubLayersMinus1; i++) {
        if (profilePresent[i])
            u(b, 32), u(b, 32), u(b, 24);  // 88 bits
        if (levelPresent[i])
            u(b, 8);
    }
    return idc >= 1 && idc <= 3 ? idc : compatible;
}

int hevc_parse_sps(HEVCParams *params, const uint8_t *nal, size_t size)
{
    uint8_t rbsp[4096];  // per call: sessions on other threads parse their own sets
    Bits b;
    if (!begin(&b, rbsp, sizeof(rbsp), nal, size))
        return -1;
    u(&b, 4);  // sps_video_parameter_set_id
    const unsigned maxSub = u(&b, 3);
    u(&b, 1);
    if (maxSub > 6)
        return -1;
    HEVCSPS s;
    memset(&s, 0, sizeof(s));
    s.profile_idc = (uint8_t)profileTierLevel(&b, maxSub, &s.level_idc);
    const uint32_t id = ue(&b);
    s.chroma_format_idc = (uint8_t)ue(&b);
    if (id > 15 || s.chroma_format_idc != 1)
        return -1;  // 4:2:0 only (no separate_colour_plane_flag then)
    const uint32_t w = ue(&b), h = ue(&b);
    if (w < 8 || h < 8 || w > 8192 || h > 8192)
        return -1;
    s.width = (uint16_t)w, s.height = (uint16_t)h;
    if (u(&b, 1)) {  // conformance_window_flag, in chroma units (SubWidthC = SubHeightC = 2)
        s.crop_left = (uint16_t)(2 * ue(&b)), s.crop_right = (uint16_t)(2 * ue(&b));
        s.crop_top = (uint16_t)(2 * ue(&b)), s.crop_bottom = (uint16_t)(2 * ue(&b));
        if (s.crop_left + s.crop_right >= w || s.crop_top + s.crop_bottom >= h)
            return -1;
    }
    s.bit_depth = (uint8_t)(ue(&b) + 8), s.bit_depth_chroma = (uint8_t)(ue(&b) + 8);
    s.log2_max_poc_lsb = (uint8_t)(ue(&b) + 4);
    if (s.log2_max_poc_lsb > 16)
        return -1;
    const bool subLayerInfo = u(&b, 1);
    for (unsigned i = subLayerInfo ? 0 : maxSub; i <= maxSub; i++) {
        const uint32_t dpb = ue(&b) + 1, reorder = ue(&b), latency = ue(&b);
        if (dpb > 16 || reorder >= dpb)
            return -1;
        s.max_dec_pic_buffering = (uint8_t)dpb, s.max_num_reorder = (uint8_t)reorder;
        s.max_latency_increase_plus1 = latency;  // the last (HighestTid) wins
    }
    s.log2_min_cb = (uint8_t)(ue(&b) + 3), s.log2_diff_max_min_cb = (uint8_t)ue(&b);
    s.log2_min_tb = (uint8_t)(ue(&b) + 2), s.log2_diff_max_min_tb = (uint8_t)ue(&b);
    if (s.log2_min_cb + s.log2_diff_max_min_cb > 6 || s.log2_min_cb + s.log2_diff_max_min_cb < 4 ||
        s.log2_min_tb + s.log2_diff_max_min_tb > 5)
        return -1;
    s.max_th_depth_inter = (uint8_t)ue(&b), s.max_th_depth_intra = (uint8_t)ue(&b);
    hevc_default_scaling_list(&s.scaling);
    if ((s.scaling_list_enabled = u(&b, 1)) && u(&b, 1))
        scalingListData(&b, &s.scaling);
    s.amp = u(&b, 1), s.sao = u(&b, 1);
    if ((s.pcm = u(&b, 1))) {
        s.pcm_bit_depth = (uint8_t)(u(&b, 4) + 1), s.pcm_bit_depth_chroma = (uint8_t)(u(&b, 4) + 1);
        s.log2_min_pcm = (uint8_t)(ue(&b) + 3), s.log2_diff_max_min_pcm = (uint8_t)ue(&b);
        s.pcm_loop_filter_disabled = u(&b, 1);
    }
    const uint32_t sets = ue(&b);
    if (sets > 64)
        return -1;
    s.num_short_term_rps = (uint8_t)sets;
    for (unsigned i = 0; i < sets; i++)
        if (!stRefPicSet(&b, s.st_rps, i, sets, &s.st_rps[i]))
            return -1;
    if ((s.long_term_present = u(&b, 1))) {
        const uint32_t n = ue(&b);
        if (n > 32)
            return -1;
        s.num_long_term_sps = (uint8_t)n;
        for (unsigned i = 0; i < n; i++)
            s.lt_poc_lsb_sps[i] = (uint16_t)u(&b, s.log2_max_poc_lsb), s.lt_used_sps[i] = (uint8_t)u(&b, 1);
    }
    s.temporal_mvp = u(&b, 1), s.strong_intra_smoothing = u(&b, 1);
    if (u(&b, 1)) {  // vui_parameters_present_flag
        if (u(&b, 1) && u(&b, 8) == 255)
            u(&b, 16), u(&b, 16);  // aspect_ratio_idc, EXTENDED_SAR
        if (u(&b, 1))
            u(&b, 1);    // overscan
        if (u(&b, 1)) {  // video_signal_type_present_flag
            u(&b, 3);
            s.full_range = u(&b, 1);
            if ((s.colour_description = u(&b, 1)))
                s.colour_primaries = (uint8_t)u(&b, 8), s.transfer = (uint8_t)u(&b, 8), s.matrix = (uint8_t)u(&b, 8);
        }
        if ((s.chroma_location = u(&b, 1))) {
            s.chroma_top = ue(&b);
            s.chroma_bottom = ue(&b);
        }
    }
    if (b.bad)
        return -1;
    // Main (8-bit), Main 10 and Main Still Picture: the profiles NVDEC 2.0 decodes.
    if (s.profile_idc < 1 || s.profile_idc > 3 || s.bit_depth != s.bit_depth_chroma ||
        (s.bit_depth != 8 && s.bit_depth != 10) || (s.bit_depth == 10 && s.profile_idc != 2))
        return -1;
    s.valid = true;
    params->sps[id] = s;
    return 0;
}

int hevc_parse_pps(HEVCParams *params, const uint8_t *nal, size_t size)
{
    uint8_t rbsp[4096];  // per call: sessions on other threads parse their own sets
    Bits b;
    if (!begin(&b, rbsp, sizeof(rbsp), nal, size))
        return -1;
    HEVCPPS p;
    memset(&p, 0, sizeof(p));
    const uint32_t id = ue(&b), sps = ue(&b);
    if (id > 63 || sps > 15)
        return -1;
    p.sps_id = (uint8_t)sps;
    p.dependent_slice_segments = u(&b, 1), p.output_flag_present = u(&b, 1);
    p.num_extra_slice_header_bits = (uint8_t)u(&b, 3);
    p.sign_data_hiding = u(&b, 1), p.cabac_init_present = u(&b, 1);
    const uint32_t l0 = ue(&b) + 1, l1 = ue(&b) + 1;
    const int32_t qp = se(&b);
    if (l0 > 15 || l1 > 15 || qp < -26 - 6 * 8 || qp > 25)
        return -1;
    p.num_ref_idx_l0_default = (uint8_t)l0, p.num_ref_idx_l1_default = (uint8_t)l1, p.init_qp_minus26 = (int8_t)qp;
    p.constrained_intra_pred = u(&b, 1), p.transform_skip = u(&b, 1);
    if ((p.cu_qp_delta = u(&b, 1)))
        p.diff_cu_qp_delta_depth = (uint8_t)ue(&b);
    const int32_t cb = se(&b), cr = se(&b);
    if (cb < -12 || cb > 12 || cr < -12 || cr > 12 || p.diff_cu_qp_delta_depth > 3)
        return -1;
    p.cb_qp_offset = (int8_t)cb, p.cr_qp_offset = (int8_t)cr;
    p.slice_chroma_qp_offsets_present = u(&b, 1);
    p.weighted_pred = u(&b, 1), p.weighted_bipred = u(&b, 1), p.transquant_bypass = u(&b, 1);
    p.tiles = u(&b, 1), p.entropy_coding_sync = u(&b, 1);
    p.num_tile_columns = p.num_tile_rows = 1;
    if (p.tiles) {
        const uint32_t cols = ue(&b) + 1, rows = ue(&b) + 1;
        if (cols > 20 || rows > 22)
            return -1;
        p.num_tile_columns = (uint8_t)cols, p.num_tile_rows = (uint8_t)rows;
        if (!(p.uniform_spacing = u(&b, 1))) {
            for (unsigned i = 0; i + 1 < cols; i++)
                p.column_width[i] = (uint16_t)(ue(&b) + 1);
            for (unsigned i = 0; i + 1 < rows; i++)
                p.row_height[i] = (uint16_t)(ue(&b) + 1);
        }
        p.loop_filter_across_tiles = u(&b, 1);
    }
    p.loop_filter_across_slices = u(&b, 1);
    if ((p.deblocking_control = u(&b, 1))) {
        p.deblocking_override = u(&b, 1);
        if (!(p.deblocking_disabled = u(&b, 1))) {
            const int32_t beta = se(&b), tc = se(&b);
            if (beta < -6 || beta > 6 || tc < -6 || tc > 6)
                return -1;
            p.beta_offset = (int8_t)(beta * 2), p.tc_offset = (int8_t)(tc * 2);
        }
    }
    if ((p.scaling_list_present = u(&b, 1)))
        scalingListData(&b, &p.scaling);
    p.lists_modification_present = u(&b, 1);
    p.log2_parallel_merge_level = (uint8_t)(ue(&b) + 2);
    p.slice_header_extension_present = u(&b, 1);
    if (u(&b, 1))
        p.range_extension = u(&b, 1);  // pps_extension_present_flag, pps_range_extension_flag
    if (b.bad || p.log2_parallel_merge_level > 6)
        return -1;
    p.valid = true;
    params->pps[id] = p;
    return 0;
}

// 7.3.6.1, up to the reference picture syntax.
int hevc_parse_slice_header(const HEVCParams *params, const uint8_t *nal, size_t size, HEVCSliceHeader *out)
{
    uint8_t rbsp[1024];
    Bits b;
    if (!begin(&b, rbsp, sizeof(rbsp), nal, size))
        return -1;
    memset(out, 0, sizeof(*out));
    out->nal_type = (nal[0] >> 1) & 0x3f;
    out->temporal_id = (uint8_t)((nal[1] & 7) - 1);
    if (out->nal_type > HEVC_NAL_CRA || (nal[1] & 7) == 0)
        return -1;
    out->first_slice = u(&b, 1);
    if (out->nal_type >= HEVC_NAL_BLA_W_LP)
        out->no_output_of_prior_pics = u(&b, 1);
    const uint32_t ppsId = ue(&b);
    if (ppsId > 63 || !params->pps[ppsId].valid)
        return -1;
    const HEVCPPS *pps = &params->pps[ppsId];
    const HEVCSPS *sps = &params->sps[pps->sps_id];
    if (!sps->valid)
        return -1;
    out->pps_id = (uint8_t)ppsId;
    if (!out->first_slice) {
        if (pps->dependent_slice_segments)
            out->dependent = u(&b, 1);
        const unsigned ctb = sps->log2_min_cb + sps->log2_diff_max_min_cb;
        const uint32_t ctbs = ((sps->width + (1u << ctb) - 1) >> ctb) * ((sps->height + (1u << ctb) - 1) >> ctb);
        u(&b, ceilLog2(ctbs));  // slice_segment_address
    }
    out->pic_output = true;
    if (out->dependent)
        return b.bad ? -1 : 0;  // the rest comes from the independent segment before it
    u(&b, pps->num_extra_slice_header_bits);
    const uint32_t type = ue(&b);
    if (type > HEVC_SLICE_I)
        return -1;
    out->slice_type = (uint8_t)type;
    const size_t skipFrom = b.pos;
    if (pps->output_flag_present)
        out->pic_output = u(&b, 1);
    if (out->nal_type != HEVC_NAL_IDR_W_RADL && out->nal_type != HEVC_NAL_IDR_N_LP) {
        out->poc_lsb = (uint16_t)u(&b, sps->log2_max_poc_lsb);
        if ((out->st_rps_from_sps = u(&b, 1))) {
            const unsigned idx = sps->num_short_term_rps > 1 ? u(&b, ceilLog2(sps->num_short_term_rps)) : 0;
            if (idx >= sps->num_short_term_rps)
                return -1;
            out->st_rps = sps->st_rps[idx];
        } else {
            const size_t at = b.pos;
            if (!stRefPicSet(&b, sps->st_rps, sps->num_short_term_rps, sps->num_short_term_rps, &out->st_rps))
                return -1;
            out->st_rps_bits = (uint16_t)(b.pos - at);
        }
        if (sps->long_term_present) {
            const uint32_t fromSps = sps->num_long_term_sps ? ue(&b) : 0, own = ue(&b);
            if (fromSps > sps->num_long_term_sps || fromSps + own > 32)
                return -1;
            out->num_lt = (uint8_t)(fromSps + own);
            for (unsigned i = 0; i < out->num_lt; i++) {
                if (i < fromSps) {
                    const unsigned idx = sps->num_long_term_sps > 1 ? u(&b, ceilLog2(sps->num_long_term_sps)) : 0;
                    if (idx >= sps->num_long_term_sps)
                        return -1;
                    out->lt[i].poc_lsb = sps->lt_poc_lsb_sps[idx];
                    out->lt[i].used = sps->lt_used_sps[idx];
                } else {
                    out->lt[i].poc_lsb = (uint16_t)u(&b, sps->log2_max_poc_lsb);
                    out->lt[i].used = u(&b, 1);
                }
                if ((out->lt[i].msb_present = u(&b, 1))) {
                    const uint32_t cycle = ue(&b);
                    // (7-52): accumulated within the SPS-named and within the slice's own entries
                    out->lt[i].msb_cycle = cycle + (i && i != fromSps ? out->lt[i - 1].msb_cycle : 0);
                } else if (i && i != fromSps) {
                    out->lt[i].msb_cycle = out->lt[i - 1].msb_cycle;
                }
            }
        }
    }
    out->skip_bits = (uint16_t)(b.pos - skipFrom);
    return b.bad ? -1 : 0;
}
