// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCNVENC.h. Every value below is NVIDIA's T210 driver's for an IDR or P picture (L4T 32.7.6 libnvtvmr, read
// with llvm-objdump; the reading, its addresses and its three sizes are in TegraNVENC/nvidia/SOURCE-H265), and
// test/hevcnvenc_host.c checks the setups against that driver's byte for byte. Setup and methods: NVIDIA's
// nvenc_drv.h and clc5b7.h (MIT, drivers/TegraNVENC/nvidia), whose H.265 layout NVENC 5.0's app 3 reads as written.
#include "HEVCNVENC.h"
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../../TegraNVENC/nvidia/clc5b7.h"
#include "../../TegraNVENC/TegraNVENCCheckH265.h"

#define ALIGN(v, a) (((v) + (a) - 1) & ~((uint32_t)(a) - 1))

// The H.265 app fetches the slice control in 64-byte, the ME control in 3 x 64-byte and the MD control in one 128-byte
// transfer, each aligned to its size; it copies the quant control's offset and never reads the block.
enum {
    SLICE_OFF = TEGRA_NVENC_H265_SETUP_HEADER,
    ME_OFF = SLICE_OFF + 64,
    MD_OFF = ME_OFF + 192,
    Q_OFF = MD_OFF + 128,
    SETUP_BYTES = Q_OFF + 16,
};

// The driver's ME, MD and quant controls (its T210 path, quality level 0): the same for every size it was read at.
static const uint8_t kMeControl[192] = {
    0x02, 0x40, 0x07, 0x00, 0x01, 0x60, 0x00, 0x0c, 0xff, 0x0f, 0xff, 0x03, 0x80, 0x02, 0x00, 0x28, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x1c, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x89, 0x1c, 0x1c, 0x7f, 0x1c, 0x1c, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x08, 0x1c, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6e, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t kMdControl[128] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x55, 0x55, 0x55, 0x57, 0x55, 0x55, 0x55, 0x07, 0x08, 0x04,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x0b, 0x00, 0x00, 0x0f, 0xf7, 0x00, 0x0e, 0xff, 0x79, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3a, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t kQuantControl[16] = {
    0x6b, 0x05, 0xff, 0xaa, 0xaa, 0xff, 0x0f, 0x00, 0x0f, 0x0f, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int hevcnvenc_configure(HEVCEncConfig *c)
{
    c->log2_ctb = 5;
    c->log2_min_cb = 3;
    c->log2_min_tb = 2;
    c->log2_max_tb = 5;
    c->max_tr_depth_inter = c->max_tr_depth_intra = 3;
    c->amp = true;
    c->sao = c->temporal_mvp = c->strong_intra_smoothing = false;
    if (hevcenc_configure(c))
        return -1;
    return hevcnvenc_ctbs(c) <= TEGRA_NVENC_H265_MAX_CTBS ? 0 : -1;
}

uint32_t hevcnvenc_ctbs(const HEVCEncConfig *c)
{
    return (hevcenc_coded_width(c) / HEVCNVENC_CTB) * (hevcenc_coded_height(c) / HEVCNVENC_CTB);
}

void hevcnvenc_layout(const HEVCEncConfig *c, uint32_t inputLayout, H264EncLayout *l)
{
    memset(l, 0, sizeof(*l));
    const uint32_t width = hevcenc_coded_width(c), height = hevcenc_coded_height(c);
    h264enc_surface(inputLayout, width, height, &l->input);
    h264enc_surface(TEGRA_NVENC_LAYOUT_TILED16, width, height, &l->recon);
    l->bitstream_bytes = ALIGN(tegra_nvenc_h265_bitstream_bytes(width, height), 65536);
    l->status_bytes = 4096;
    l->history_bytes = ALIGN(tegra_nvenc_history_bytes(width, height), 256);
    l->history_offset = l->status_bytes;
    l->aux_bytes = l->history_offset + l->history_bytes;
    l->mepred_bytes = ALIGN((width / 16) * (height / 16) * TEGRA_NVENC_MEPRED_BYTES_PER_MB, 65536);
}

static void surfaceConfig(const HEVCEncConfig *c, uint32_t layout, uint32_t chromaMode, const H264EncSurface *s,
                          nvenc_h265_surface_cfg_s *cfg, bool input)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->frame_width_minus1 = (unsigned short)(hevcenc_coded_width(c) - 1);
    cfg->frame_height_minus1 = (unsigned short)(hevcenc_coded_height(c) - 1);
    cfg->sfc_pitch = cfg->sfc_pitch_chroma = (unsigned short)s->pitch;
    // Only the input has a chroma method app 3 reads: it finds reconstructions and references at their luma address
    // plus this offset (SOURCE-H265).
    cfg->chroma_top_frm_offset = chromaMode == TEGRA_NVENC_CHROMA_OFFSET || !input ? s->chroma_offset / 256 : 0;
    if (!input) {
        cfg->tiled_16x16 = 1;  // references are always 16x16 tiled (nvenc_drv.h 1.5)
        return;
    }
    cfg->tiled_16x16 = layout == TEGRA_NVENC_LAYOUT_TILED16;
    cfg->block_height = tegra_nvenc_layout_gobs(layout);
}

static uint8_t qp(uint32_t v)
{
    return (uint8_t)(v > 51 ? 51 : v);
}

// The driver's DPB at its default of one reference (two slots, libnvtvmr 0x46160): picture POC n is reconstructed in
// slot n & 1, so a P picture's reference is in slot (n - 1) & 1.
static uint32_t referenceSlot(const HEVCEncPicture *p)
{
    return (p->poc - 1) & 1;
}

// The driver's constant-QP rate control block: its initial QPs are the I picture's, +2 for P and +5 for B. App 3
// reads the block only under rate control (nvenc.bin 0x1493d); the picture's QP is its slice control's.
static void rateControl(nvenc_h265_rc_s *rc, uint8_t iqp)
{
    rc->hrd_type = 2;
    rc->QP[0] = qp(iqp + 2u);
    rc->QP[1] = qp(iqp + 5u);
    rc->QP[2] = iqp;
    rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = 51;
    rc->maxQPD = 6;
    rc->baseQPD = 3;
    rc->gop_length = 0xfff;
    rc->rc_class = 1;
    rc->i_size_ratio_x = 2;
    rc->i_size_ratio_y = 1;
}

static void pictureControl(const HEVCEncConfig *c, const H264EncLayout *l, const HEVCEncPicture *p,
                           nvenc_h265_pic_control_s *pc)
{
    const uint32_t ctbs = hevcnvenc_ctbs(c);
    memset(pc->l0, -2, sizeof(pc->l0));  // unused entries, as the driver writes them
    memset(pc->l1, -2, sizeof(pc->l1));
    memset(pc->temp_dist_l0, -1, sizeof(pc->temp_dist_l0));
    memset(pc->temp_dist_l1, -1, sizeof(pc->temp_dist_l1));
    memset(pc->dist_scale_factor, -1, sizeof(pc->dist_scale_factor));
    pc->max_slice_size = ctbs;
    pc->pic_output_flag = 1;
    pc->pic_order_cnt_lsb = (unsigned short)(p->poc & ((1u << HEVCENC_LOG2_MAX_POC_LSB) - 1));
    pc->slice_control_offset = SLICE_OFF;
    pc->me_control_offset = ME_OFF;
    pc->md_control_offset = MD_OFF;
    pc->q_control_offset = Q_OFF;
    pc->hist_buf_size = l->history_bytes;
    pc->bitstream_buf_size = tegra_nvenc_h265_bitstream_bytes(hevcenc_coded_width(c), hevcenc_coded_height(c));
    pc->pic_type = p->idr ? 3 : 0;
    pc->ref_pic_flag = 1;
    pc->codec = 5;
    uint32_t log2 = 0;
    while ((1u << log2) < ctbs)
        log2++;
    pc->ceil_log2_pic_size_in_ctbs = log2;
    pc->slice_stat_offset = TEGRA_NVENC_H265_SLICE_STAT_OFFSET;
    // The driver's; read only with MPEC and AQ statistics on, which the checker refuses.
    pc->mpec_stat_offset = 0x1000b0;
    pc->aq_stat_offset = 0x900230;
    pc->short_term_ref_pic_set_sps_flag = 1;
    pc->nal_unit_type = hevcenc_nal_type(p);
    pc->nuh_temporal_id_plus1 = 1;
    if (p->idr)
        return;
    pc->l0[0] = (char)(referenceSlot(p) << 1);
    pc->temp_dist_l0[0] = 1;
    // The driver's (0x402b8): (16384 + |td| / 2) / td x td, rounded to 8 fractional bits, is 256 for every entry.
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++)
            pc->dist_scale_factor[i][j] = 256;
    }
    pc->ref_pic_list_modification.NumPocTotalCurr = 1;
}

static void setup(const HEVCEncConfig *c, uint32_t layout, uint32_t chromaMode, const H264EncLayout *l,
                  const HEVCEncPicture *p, uint8_t *bytes)
{
    nvenc_h265_drv_pic_setup_s *s = (nvenc_h265_drv_pic_setup_s *)bytes;
    s->magic = 0xd0b70006u;  // NV_NVENC_5_0
    surfaceConfig(c, layout, chromaMode, &l->recon, &s->refpic_cfg, false);
    surfaceConfig(c, layout, chromaMode, &l->input, &s->input_cfg, true);
    surfaceConfig(c, layout, chromaMode, &l->recon, &s->outputpic_cfg, false);
    nvenc_h265_sps_data_s *sps = &s->sps_data;
    sps->chroma_format_idc = 1;
    sps->log2_max_pic_order_cnt_lsb_minus4 = HEVCENC_LOG2_MAX_POC_LSB - 4;
    sps->min_cu_size = c->log2_min_cb - 3;
    sps->max_cu_size = c->log2_ctb - 3;
    sps->min_tu_size = c->log2_min_tb - 2;
    sps->max_tu_size = c->log2_max_tb - 2;
    sps->max_transform_depth_inter = c->max_tr_depth_inter;
    sps->max_transform_depth_intra = c->max_tr_depth_intra;
    sps->amp_enabled_flag = c->amp;
    sps->num_short_term_ref_pic_sets = 1;
    s->pps_data.transform_skip_enabled_flag = 1;
    s->pps_data.pps_loop_filter_across_slices_enabled_flag = 1;
    rateControl(&s->rate_control, p->idr || p->qp < 2 ? qp(p->qp) : qp(p->qp - 2u));  // a P picture's is I + 2
    pictureControl(c, l, p, &s->pic_control);
    s->gpTimer_timeout_val = TEGRA_NVENC_GPTIMER_MAX;  // unread: app 3 times a picture by its size (SOURCE-H265)

    nvenc_h265_slice_control_s *slice = (nvenc_h265_slice_control_s *)(bytes + SLICE_OFF);
    slice->num_ctus = hevcnvenc_ctbs(c);
    slice->qp_avr = qp(p->qp);
    // The driver's P (0x3eb9c, 0x3f510): slice_type takes pic_type's numbering (0 P, 1 B, 2 I), not nvenc_drv.h's
    // (0 B, 1 P); which type NVENC writes into the slice header is not established (SOURCE-H265).
    slice->slice_type = p->idr ? 2 : 0;
    slice->slice_loop_filter_across_slices_enabled_flag = 1;
    slice->qp_slice_max = 51;
    memcpy(bytes + ME_OFF, kMeControl, sizeof(kMeControl));
    memcpy(bytes + MD_OFF, kMdControl, sizeof(kMdControl));
    memcpy(bytes + Q_OFF, kQuantControl, sizeof(kQuantControl));
}

void hevcnvenc_fill_submit(const HEVCEncConfig *c, uint32_t inputLayout, uint32_t chromaMode, const H264EncLayout *l,
                           const HEVCEncPicture *p, const H264EncHandles *h, TegraNVENCSubmit *s)
{
    memset(s, 0, sizeof(*s));
    s->codec = TEGRA_NVENC_CODEC_H265;
    s->setup_bytes = SETUP_BYTES;
    setup(c, inputLayout, chromaMode, l, p, s->setup);

    const bool method = chromaMode == TEGRA_NVENC_CHROMA_METHOD;
    TegraNVENCEntry *e = s->entries;
    uint32_t n = 0;
#define VALUE(m, v)              e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_VALUE, (v), 0}
#define RELOC(m, handle, offset) e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_RELOC, (handle), (offset)}
    VALUE(NVC5B7_SET_APPLICATION_ID, NVC5B7_SET_APPLICATION_ID_ID_NVENC_H265);
    // The driver's control word (codec type 3, FORCE_OUTPIC, SLICE_STAT_ON, constant QP) with the GP timer on.
    VALUE(NVC5B7_SET_CONTROL_PARAMS, NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | (1u << 8) | (1u << 11) | (1u << 12));
    VALUE(NVC5B7_SET_PICTURE_INDEX, p->picture_index);
    RELOC(NVC5B7_SET_IN_CUR_PIC, h->input, 0);
    RELOC(NVC5B7_SET_IN_CUR_PIC_CHROMA_U, h->input, method ? l->input.chroma_offset : 0);
    RELOC(NVC5B7_SET_OUT_REF_PIC_LUMA, h->recon, 0);
    if (!p->idr) {
        // The driver's for P pictures only (0x3f370): the predictors the picture before wrote, and its reference's
        // luma in the slot its list entry names (0x3fedc, one method a reference; chroma by refpic_cfg's offset).
        RELOC(NVC5B7_SET_IN_MEPRED_DATA, h->mepred[~p->poc & 1], 0);
        RELOC(NVC5B7_SET_OUT_MEPRED_DATA, h->mepred[p->poc & 1], 0);
        RELOC(NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * referenceSlot(p), h->ref, 0);
    }
    RELOC(NVC5B7_SET_OUT_BITSTREAM, h->bitstream, 0);
    RELOC(NVC5B7_SET_OUT_ENC_STATUS, h->aux, 0);
    RELOC(NVC5B7_SET_IOHISTORY, h->aux, l->history_offset);
    VALUE(NVC5B7_EXECUTE, 1u << 8 /* AWAKEN */);
#undef VALUE
#undef RELOC
    s->entry_count = n;
    s->clean[0] = (TegraNVENCRange){h->input, 0, l->input.bytes, 0};
    s->clean_count = 1;
}
