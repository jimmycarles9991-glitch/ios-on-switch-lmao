// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for H264NVENC.c's setups at sizes that are not whole macroblocks (1080p, the HW Test's): every surface
// config carries the coded size, the kernel's checker takes the IDR and P submits, and the display size is refused
// (NVENC 5.0's H.264 app fetches one macroblock row too few from it; TegraNVENC/nvidia/SOURCE). Both pictures name
// reference slot 0, which the checker bounds IDR or not. B pictures (bPictures): a reordering session's P and B
// setups differ exactly where NVIDIA's T210 driver's do, with its values (TegraNVENC/nvidia/SOURCE), the checker takes
// them and refuses each kind of bad B submit, and the SPS and B slice header say what the setup does.
// Prints "PASS h264nvenc" when all hold.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../SwitchVideoDecoders/AVD/H264Syntax.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"
#include "../AVE/H264NVENC.h"

static int gFailures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            gFailures++;                  \
        }                                 \
    } while (0)

// The app reads the setup at DMEM 0xa00: input_cfg's size at 0xa24, outputpic_cfg's at 0xa44.
_Static_assert(offsetof(nvenc_h264_drv_pic_setup_s, input_cfg) == 0x24, "input_cfg");
_Static_assert(offsetof(nvenc_h264_drv_pic_setup_s, outputpic_cfg) == 0x44, "outputpic_cfg");

static const H264EncHandles kHandles = {.input = 1, .recon = 2, .ref = 3, .bitstream = 4, .aux = 5, .mepred = {6, 7}};

// refShort: the reference buffer that many bytes smaller than a picture.
static const char *check(const TegraNVENCSubmit *s, const H264EncLayout *l, uint64_t refShort)
{
    const uint64_t sizes[8] = {0,
                               l->input.bytes,
                               l->recon.bytes,
                               l->recon.bytes - refShort,
                               l->bitstream_bytes,
                               l->aux_bytes,
                               l->mepred_bytes,
                               l->mepred_bytes};
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
            avail[i] = sizes[s->entries[i].value] - s->entries[i].offset;
    return tegra_nvenc_check(s, avail);
}

static bool relocates(const TegraNVENCSubmit *s, uint16_t method, uint32_t handle)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method)
            return s->entries[i].kind == TEGRA_NVENC_ENTRY_RELOC && s->entries[i].value == handle;
    return false;
}

static void displaySize(nvenc_h264_surface_cfg_s *cfg, const H264EncConfig *c)
{
    cfg->frame_width_minus1 = (unsigned short)(c->width - 1);
    cfg->frame_height_minus1 = (unsigned short)(c->height - 1);
}

// The SPS a reordering session states, and a B picture's slice header, parsed back by AVD's H264Syntax.c.
static void syntaxB(const H264EncConfig *c)
{
    uint8_t nal[192];
    H264ParamSets *sets = calloc(1, sizeof(*sets));
    uint8_t id = 0;
    size_t n = h264enc_write_sps(c, nal, sizeof(nal));
    CHECK(n && !h264_parse_sps(sets, nal, n, &id), "the reordering SPS does not parse");
    const H264SPS *sps = &sets->sps[id];
    CHECK(sps->max_num_ref_frames == 2 && sps->bitstream_restriction && sps->max_num_reorder_frames == 1 &&
              sps->max_dec_frame_buffering == 2,
          "SPS: %u references, reorder %u, buffering %u (expected 2, 1, 2)", sps->max_num_ref_frames,
          sps->max_num_reorder_frames, sps->max_dec_frame_buffering);
    n = h264enc_write_pps(c, nal, sizeof(nal));
    CHECK(n && !h264_parse_pps(sets, nal, n, &id), "the PPS does not parse");
    const H264EncPicture b = {.type = H264ENC_B, .qp = 28, .frame_num = 2, .poc_lsb = 4, .ref_slot = {0, 1}};
    uint8_t rbsp[64];
    H264Bits bits;
    h264bits_init(&bits, rbsp, sizeof(rbsp));
    h264enc_slice_header(c, &b, &bits);
    h264bits_trailing(&bits);
    n = h264enc_nal(0x01, rbsp, h264bits_size(&bits), nal, sizeof(nal));
    H264Slice slice;
    CHECK(n && !h264_parse_slice(sets, nal, n, &slice) && slice.slice_type == H264_SLICE_B && !slice.nal_ref_idc &&
              slice.frame_num == 2 && slice.poc_lsb == 4,
          "the B slice header does not parse as a non-reference B slice, frame_num 2, POC 4");
    const H264EncPicture intra = {.type = H264ENC_I, .qp = 25, .frame_num = 2, .poc_lsb = 2, .unreferenced = true};
    h264bits_init(&bits, rbsp, sizeof(rbsp));
    h264enc_slice_header(c, &intra, &bits);
    h264bits_trailing(&bits);
    n = h264enc_nal(0x01, rbsp, h264bits_size(&bits), nal, sizeof(nal));
    CHECK(n && !h264_parse_slice(sets, nal, n, &slice) && slice.slice_type == H264_SLICE_I && !slice.nal_ref_idc &&
              !slice.adaptive_ref_pic_marking,
          "the unreferenced I slice header does not parse as a non-reference I slice");
    free(sets);
}

static void checkSize(uint32_t width, uint32_t height)
{
    H264EncConfig c = {.width = width,
                       .height = height,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .fps_num = 60,
                       .fps_den = 1,
                       .input_layout = TEGRA_NVENC_LAYOUT_TILED16,
                       .chroma_mode = TEGRA_NVENC_CHROMA_OFFSET};
    CHECK(!h264enc_configure(&c), "configure %ux%u", width, height);
    H264EncLayout l;
    h264enc_layout(&c, &l);
    const uint32_t codedW = h264enc_width_mbs(&c) * 16, codedH = h264enc_height_mbs(&c) * 16;
    for (int k = 0; k < 2; k++) {
        const H264EncPicture picture = {.type = k ? H264ENC_P : H264ENC_IDR, .qp = 26, .frame_num = (uint32_t)k};
        static TegraNVENCSubmit s;
        h264enc_fill_submit(&c, &l, &picture, &kHandles, &s);
        nvenc_h264_drv_pic_setup_s *p = (nvenc_h264_drv_pic_setup_s *)s.setup;
        const nvenc_h264_surface_cfg_s *cfgs[3] = {&p->refpic_cfg, &p->input_cfg, &p->outputpic_cfg};
        for (int i = 0; i < 3; i++)
            CHECK(cfgs[i]->frame_width_minus1 + 1u == codedW && cfgs[i]->frame_height_minus1 + 1u == codedH,
                  "%ux%u: surface %d is %ux%u, not the coded %ux%u", width, height, i, cfgs[i]->frame_width_minus1 + 1u,
                  cfgs[i]->frame_height_minus1 + 1u, codedW, codedH);
        const nvenc_h264_slice_control_s *slice =
            (const nvenc_h264_slice_control_s *)(s.setup + p->pic_control.slice_control_offset);
        CHECK(slice->num_mb == (codedW / 16) * (codedH / 16), "%ux%u: slice of %u macroblocks", width, height,
              slice->num_mb);
        const bool slot0 = relocates(&s, NVC5B7_SET_IN_REF_PIC0_LUMA, kHandles.ref);
        CHECK(slot0, "%ux%u %s: reference slot 0 is not the other reconstruction", width, height, k ? "P" : "IDR");
        const char *why = check(&s, &l, 4096);
        CHECK(why && !strcmp(why, "surface too small"), "%ux%u %s with a short reference: checker said \"%s\"", width,
              height, k ? "P" : "IDR", why ? why : "accepted");
        why = check(&s, &l, 0);
        CHECK(!why, "%ux%u %s: the checker refuses it: %s", width, height, k ? "P" : "IDR", why ? why : "");
        if (codedW == width && codedH == height)
            continue;
        displaySize(&p->refpic_cfg, &c);
        displaySize(&p->input_cfg, &c);
        displaySize(&p->outputpic_cfg, &c);
        why = check(&s, &l, 0);
        CHECK(why && !strcmp(why, "dimensions"), "%ux%u %s at the display size: checker said \"%s\"", width, height,
              k ? "P" : "IDR", why ? why : "accepted");
    }
}

// A reordering session's buffers: 1 input, 2..4 reconstructions, 5 bitstream, 6 aux, 7..8 ME predictors, 9..11
// colocated motion.
static const H264EncHandles kB = {.input = 1,
                                  .recon = 4,
                                  .ref = 2,
                                  .bitstream = 5,
                                  .aux = 6,
                                  .mepred = {7, 8},
                                  .ref1 = 3,
                                  .coloc_out = 11,
                                  .coloc_in = 10};

static const char *checkB(const TegraNVENCSubmit *s, const H264EncLayout *l, uint64_t colocShort)
{
    const uint64_t sizes[12] = {0,
                                l->input.bytes,
                                l->recon.bytes,
                                l->recon.bytes,
                                l->recon.bytes,
                                l->bitstream_bytes,
                                l->aux_bytes,
                                l->mepred_bytes,
                                l->mepred_bytes,
                                l->coloc_bytes,
                                l->coloc_bytes - colocShort,
                                l->coloc_bytes};
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
            avail[i] = sizes[s->entries[i].value] - s->entries[i].offset;
    return tegra_nvenc_check(s, avail);
}

// The fields NVIDIA's T210 driver's B setup changes against its P (libnvtvmr eval at 1280x720 High CABAC 8x8).
static bool bField(size_t at, const nvenc_h264_pic_control_s *pc, const uint8_t *setup)
{
    const size_t pic = (size_t)((const uint8_t *)pc - setup);

    const struct {
        size_t offset, size;
    } fields[] = {
        {offsetof(nvenc_h264_drv_pic_setup_s, rate_control) + offsetof(nvenc_h264_rc_s, QP), 3},  // the picture's QP
        {pic + offsetof(nvenc_h264_pic_control_s, l0), 1},
        {pic + offsetof(nvenc_h264_pic_control_s, l1), 1},
        {pic + offsetof(nvenc_h264_pic_control_s, temp_dist_l0), 1},
        {pic + offsetof(nvenc_h264_pic_control_s, temp_dist_l1), 1},
        {pic + offsetof(nvenc_h264_pic_control_s, dist_scale_factor), sizeof(pc->dist_scale_factor)},
        {pic + offsetof(nvenc_h264_pic_control_s, frame_num), 4},           // frame_num, pic_order_cnt_lsb
        {pic + offsetof(nvenc_h264_pic_control_s, hist_buf_size) + 12, 1},  // pic_type, ref_pic_flag
        {pc->me_control_offset + 12, 4},  // predsrc: coloc_enable, const_mv_stamp_l0/l1
        {pc->me_control_offset + 20, 4},
        {pc->md_control_offset + 11, 1},  // bdirect_mode
        {pc->slice_control_offset, 4},    // qp_avr
    };

    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (at >= fields[i].offset && at < fields[i].offset + fields[i].size)
            return true;
    return false;
}

static void bPictures(void)
{
    H264EncConfig c = {.width = 1280,
                       .height = 720,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .fps_num = 60,
                       .fps_den = 1,
                       .input_layout = TEGRA_NVENC_LAYOUT_TILED16,
                       .chroma_mode = TEGRA_NVENC_CHROMA_OFFSET,
                       .reorder = true};
    CHECK(!h264enc_configure(&c) && c.reorder, "configure 1280x720 with B pictures");
    H264EncLayout l;
    h264enc_layout(&c, &l);
    CHECK(l.coloc_bytes == 3600 * 64, "colocated buffer %u bytes, NVIDIA's driver gives 230400", l.coloc_bytes);
    // IBBP after an IDR in slot 0: P3 in slot 1, then B1 and B2 between them (the driver's frame_num 2, POC 2 and 4).
    const H264EncPicture p3 = {.type = H264ENC_P, .qp = 25, .frame_num = 1, .poc_lsb = 6, .gap = {2, 0}};
    static TegraNVENCSubmit ps, bs;
    H264EncHandles hp = kB;
    hp.ref1 = hp.coloc_in = 0;
    h264enc_fill_submit(&c, &l, &p3, &hp, &ps);
    const nvenc_h264_pic_control_s *ppc = &((const nvenc_h264_drv_pic_setup_s *)ps.setup)->pic_control;
    CHECK(ppc->l0[0] == 0 && ppc->temp_dist_l0[0] == 3 && ppc->dist_scale_factor[7][0] == 256,
          "P3: l0 %d, distance %d (the driver's 0, 3)", ppc->l0[0], ppc->temp_dist_l0[0]);
    const char *why = checkB(&ps, &l, 0);
    CHECK(!why, "P3 with colocated output: the checker refuses it: %s", why ? why : "");

    static const struct {
        uint8_t gap[2];
        short scale;
    } kBs[] = {{{0, 1}, 85}, {{1, 0}, 171}, {{0, 0}, 128}};  // B1 and B2 of IBBP, the B of IBP

    for (unsigned k = 0; k < 3; k++) {
        const H264EncPicture b = {.type = H264ENC_B,
                                  .qp = 28,
                                  .frame_num = 2,
                                  .poc_lsb = 2 + 2 * k,
                                  .ref_slot = {0, 1},
                                  .gap = {kBs[k].gap[0], kBs[k].gap[1]}};
        h264enc_fill_submit(&c, &l, &b, &kB, &bs);
        const nvenc_h264_pic_control_s *pc = &((const nvenc_h264_drv_pic_setup_s *)bs.setup)->pic_control;
        CHECK(pc->pic_type == 1 && !pc->ref_pic_flag && pc->l0[0] == 0 && pc->l1[0] == 2 &&
                  pc->temp_dist_l0[0] == kBs[k].gap[0] + 1 && pc->temp_dist_l1[0] == -(kBs[k].gap[1] + 1) &&
                  pc->dist_scale_factor[0][0] == kBs[k].scale && pc->dist_scale_factor[1][0] == -1,
              "B %u: type %u ref %u lists %d %d distances %d %d scale %d (expected %d)", k, pc->pic_type,
              pc->ref_pic_flag, pc->l0[0], pc->l1[0], pc->temp_dist_l0[0], pc->temp_dist_l1[0],
              pc->dist_scale_factor[0][0], kBs[k].scale);
        const nvenc_h264_me_control_s *me = (const nvenc_h264_me_control_s *)(bs.setup + pc->me_control_offset);
        const nvenc_h264_md_control_s *md = (const nvenc_h264_md_control_s *)(bs.setup + pc->md_control_offset);
        CHECK(me->predsrc.coloc_enable && me->predsrc.const_mv_stamp_l0 == 3 && me->predsrc.const_mv_stamp_l1 == 3 &&
                  md->bdirect_mode == 1 && !md->bskip_enable && !md->intra_luma4x4_mode_enable,
              "B %u: ME and MD controls are not the driver's B controls", k);
        for (size_t i = 0; i < bs.setup_bytes; i++)
            CHECK(bs.setup[i] == ps.setup[i] || bField(i, pc, bs.setup),
                  "B %u: setup byte %#zx is %#x, the P's %#x: NVIDIA's driver changes nothing there", k, i, bs.setup[i],
                  ps.setup[i]);
        CHECK(relocates(&bs, NVC5B7_SET_IN_REF_PIC0_LUMA, kB.ref) &&
                  relocates(&bs, NVC5B7_SET_IN_REF_PIC1_LUMA, kB.ref1) &&
                  relocates(&bs, NVC5B7_SET_IN_COLOC_DATA, kB.coloc_in) &&
                  relocates(&bs, NVC5B7_SET_OUT_COLOC_DATA, kB.coloc_out),
              "B %u: both references and both colocated buffers are not relocated", k);
        why = checkB(&bs, &l, 0);
        CHECK(!why, "B %u: the checker refuses it: %s", k, why ? why : "");
    }

    // What AVE codes a refused B picture as: an I picture nothing predicts from, between the same anchors.
    const H264EncPicture intra = {.type = H264ENC_I, .qp = 25, .frame_num = 2, .poc_lsb = 2, .unreferenced = true};
    H264EncHandles hi = kB;
    hi.ref1 = hi.coloc_in = 0;
    static TegraNVENCSubmit is;
    h264enc_fill_submit(&c, &l, &intra, &hi, &is);
    const nvenc_h264_pic_control_s *ipc = &((const nvenc_h264_drv_pic_setup_s *)is.setup)->pic_control;
    why = checkB(&is, &l, 0);
    CHECK(!why && ipc->pic_type == H264ENC_I && !ipc->ref_pic_flag && ipc->l0[0] == -2,
          "unreferenced I: type %u ref %u, checker said %s", ipc->pic_type, ipc->ref_pic_flag, why ? why : "ok");

    static const struct {
        const char *name, *why;
    } cases[] = {
        {"a reference B", "picture structure or type"},
        {"no colocated input", "colocated input"},
        {"colocated input in a P picture", "colocated input"},
        {"no colocated hint", "ME colocated hint"},
        {"temporal direct", "mode decision"},
        {"B_Skip", "mode decision"},
        {"both lists in one slot", "B references"},
        {"a field reference", "reference slot"},
        {"list 1 names an unrelocated slot", "reference slot"},
        {"list 1 ahead", "B references"},
        {"a scale factor not 8-203's", "B references"},
        {"two list-1 entries", "list 1"},
        {"a short colocated input", "colocated buffer"},
        {"two list-0 references", "reference count"},
    };

    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        static TegraNVENCSubmit m;
        m = k == 2 ? ps : bs;
        nvenc_h264_drv_pic_setup_s *p = (nvenc_h264_drv_pic_setup_s *)m.setup;
        nvenc_h264_pic_control_s *pc = &p->pic_control;
        nvenc_h264_me_control_s *me = (nvenc_h264_me_control_s *)(m.setup + pc->me_control_offset);
        nvenc_h264_md_control_s *md = (nvenc_h264_md_control_s *)(m.setup + pc->md_control_offset);
        uint64_t colocShort = 0;
        switch (k) {
        case 0:
            pc->ref_pic_flag = 1;
            break;
        case 1:
            for (uint32_t i = 0; i < m.entry_count; i++)
                if (m.entries[i].method == NVC5B7_SET_IN_COLOC_DATA) {
                    memmove(&m.entries[i], &m.entries[i + 1], (m.entry_count - i - 1) * sizeof(m.entries[0]));
                    m.entry_count--;
                    break;
                }
            break;
        case 2:
            for (uint32_t i = 0; i < m.entry_count; i++)
                if (m.entries[i].method == NVC5B7_SET_OUT_COLOC_DATA)
                    m.entries[i].method = NVC5B7_SET_IN_COLOC_DATA;
            break;
        case 3:
            me->predsrc.coloc_enable = 0;
            break;
        case 4:
            md->bdirect_mode = 2;
            break;
        case 5:
            md->bskip_enable = 1;
            break;
        case 6:
            pc->l1[0] = 0;
            break;
        case 7:
            pc->l1[0] = 3;
            break;
        case 8:
            pc->l1[0] = 4;
            break;
        case 9:
            pc->temp_dist_l1[0] = 1;
            break;
        case 10:
            pc->dist_scale_factor[0][0] = 256;
            break;
        case 11:
            pc->l1[1] = 2;
            break;
        case 12:
            colocShort = 256;
            break;
        case 13:
            p->pps_data.num_ref_idx_l0_active_minus1 = 1;
            break;
        }
        why = checkB(&m, &l, colocShort);
        CHECK(why && !strcmp(why, cases[k].why), "%s: checker said \"%s\", expected \"%s\"", cases[k].name,
              why ? why : "accepted", cases[k].why);
    }
    syntaxB(&c);
}

int main(void)
{
    bPictures();
    checkSize(1920, 1080);
    checkSize(1280, 720);
    checkSize(640, 360);
    checkSize(1278, 718);
    if (gFailures) {
        printf("FAIL h264nvenc: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS h264nvenc\n");
    return 0;
}
