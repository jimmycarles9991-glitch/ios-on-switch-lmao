// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for HEVCNVENC.c: its IDR setups against NVIDIA's T210 driver's (L4T 32.7.6 libnvtvmr's H.265 path read
// at 1280x720, 1920x1080 and 640x360; TegraNVENC/nvidia/SOURCE-H265) byte for byte, every field but the surfaces'
// pitches and chroma offsets, the control offsets and the two fields the firmware never reads; the kernel's H.265
// checker accepting them and refusing each kind of bad submit; and HEVCEncSyntax.c's SPS and PPS, parsed back with
// AVD's parser, stating the coding tools the setup does. Prints "PASS hevcnvenc" when all hold.
#include <stdio.h>
#include <string.h>
#include "../../SwitchVideoDecoders/AVD/HEVCSyntax.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"
#include "../AVE/HEVCNVENC.h"

static int gFailures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            gFailures++;                  \
        }                                 \
    } while (0)

// The driver's buffers, byte by byte in hex; ".." where it took the value from the surface it was given.
static const char kHeader1280[] =
    "0600b7d0ff04df02........000000000000000000000000......000000000080000000ff04df0200000000000000000000000000000000"
    "000000000000000000..0000ff04df02........000000000000000000000000......00000000008000000021648f020000000000005000"
    "0000000002191c170000003333330603000000000000000000000000000000000000000000000000000000000000000000000000ff0f0000"
    "0000000000000000000000000000000000000000000000010002010000000000000000000000000000000000fefefefefefefefefefefefe"
    "fefefefeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000980300000000000002000000"
    "0000000000040000000e0000000c0000000d000000040b0000202b0000000000a70a00000000000080000000b00010003002900001131000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000f0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000";

static const char kSlice1280[] =
    "9803b8000402cc00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000";

static const char kHeader1920[] =
    "0600b7d07f073f04........000000000000000000000000......0000000000800000007f073f0400000000000000000000000000000000"
    "000000000000000000..00007f073f04........000000000000000000000000......00000000008000000021648f020000000000005000"
    "0000000002191c170000003333330603000000000000000000000000000000000000000000000000000000000000000000000000ff0f0000"
    "0000000000000000000000000000000000000000000000010002010000000000000000000000000000000000fefefefefefefefefefefefe"
    "fefefefeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000f80700000000000002000000"
    "0000000000040000000e0000000c0000000d00000042180000a05f0000000000a70b00000000000080000000b00010003002900001131000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000f0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000";

static const char kSlice1920[] =
    "f807b8000402cc00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000";

static const char kHeader640[] =
    "0600b7d07f027f01........000000000000000000000000......0000000000800000007f027f0100000000000000000000000000000000"
    "000000000000000000..00007f027f01........000000000000000000000000......00000000008000000021648f020000000000005000"
    "0000000002191c170000003333330603000000000000000000000000000000000000000000000000000000000000000000000000ff0f0000"
    "0000000000000000000000000000000000000000000000010002010000000000000000000000000000000000fefefefefefefefefefefefe"
    "fefefefeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000f00000000000000002000000"
    "0000000000040000000e0000000c0000000d000000ee020000400b0000000000a70800000000000080000000b00010003002900001131000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000f0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000";

static const char kSlice640[] =
    "f000b8000402cc00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000";

static const char kMe[] =
    "024007000160000cff0fff038002002800000000000040a2000000000000000000000000000000000000000000001c1c1c00000000000000"
    "891c1c7f1c1c0881000000000000000000000000000000000000000000000000000000000008081c08080000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000006e7c00000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000";

static const char kMd[] =
    "0000000000000000575555555755555507080404000000006d0b00000ff7000eff7901000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000003a020000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000";

static const char kQuant[] = "6b05ffaaaaff0f000f0f0f0000000000";

// Setup bytes the driver's values do not bind: surface pitches and chroma offsets (each session's own), the control
// offsets (where this setup puts its controls), wp_control_offset and gpTimer_timeout_val (unread by app 3).
static bool skipped(uint32_t i)
{
    static const struct {
        uint32_t from, to;
    } kSkip[] = {
        {0x08, 0x0c}, {0x18, 0x1c}, {0x28, 0x2c},   {0x38, 0x3c},   {0x40, 0x42},
        {0x48, 0x4c}, {0x58, 0x5c}, {0x18c, 0x19c}, {0x2b0, 0x2b4}, {0x30c, 0x310},
    };

    for (size_t k = 0; k < sizeof(kSkip) / sizeof(kSkip[0]); k++) {
        if (i >= kSkip[k].from && i < kSkip[k].to)
            return true;
    }
    return false;
}

static int compare(const char *what, const char *hex, const uint8_t *ours, size_t size, bool header)
{
    int differ = 0;
    for (uint32_t i = 0; i < size; i++) {
        unsigned value = 0;
        if (hex[2 * i] == '.' || (header && skipped(i)) || sscanf(hex + 2 * i, "%2x", &value) != 1)
            continue;
        if (ours[i] != value) {
            printf("  %s +%#x: %#04x, the driver's %#04x\n", what, i, ours[i], value);
            differ++;
        }
    }
    return differ;
}

static void checkSize(uint32_t width, uint32_t height, const char *header, const char *slice)
{
    HEVCEncConfig c = {.width = width, .height = height, .fps_num = 30, .fps_den = 1};
    CHECK(hevcnvenc_configure(&c) == 0, "%ux%u: configure", width, height);
    H264EncLayout l;
    hevcnvenc_layout(&c, &l);
    const H264EncHandles h = {1, 2, 3, 4, 5, {6, 7}};
    const HEVCEncPicture idr = {true, 0, 23};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &h, &s);
    const nvenc_h265_pic_control_s *pc = &((const nvenc_h265_drv_pic_setup_s *)s.setup)->pic_control;
    int differ = compare("setup", header, s.setup, TEGRA_NVENC_H265_SETUP_HEADER, true);
    differ += compare("slice", slice, s.setup + pc->slice_control_offset, 64, false);
    differ += compare("ME", kMe, s.setup + pc->me_control_offset, 192, false);
    differ += compare("MD", kMd, s.setup + pc->md_control_offset, 128, false);
    differ += compare("quant", kQuant, s.setup + pc->q_control_offset, 16, false);
    CHECK(!differ, "%ux%u: %d bytes differ from the driver's setup", width, height, differ);

    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s.entry_count; i++)
        avail[i] = 64u << 20;
    const char *why = tegra_nvenc_check(&s, avail);
    CHECK(!why, "%ux%u: the checker refused the setup: %s", width, height, why ? why : "");
}

typedef void (*Break)(TegraNVENCSubmit *s);

static nvenc_h265_drv_pic_setup_s *setupOf(TegraNVENCSubmit *s)
{
    return (nvenc_h265_drv_pic_setup_s *)s->setup;
}

static void pPicture(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.pic_type = 0;
}

static void reference(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.l0[0] = 0;
}

static void temporalMvp(TegraNVENCSubmit *s)
{
    setupOf(s)->sps_data.sps_temporal_mvp_enabled_flag = 1;
}

static void sao(TegraNVENCSubmit *s)
{
    setupOf(s)->sps_data.sample_adaptive_offset_enabled_flag = 1;
}

static void meMisaligned(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.me_control_offset += 4;
}

static void fewCtus(TegraNVENCSubmit *s)
{
    ((nvenc_h265_slice_control_s *)(s->setup + 1024))->num_ctus = 1;
}

static void noBitstream(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.bitstream_buf_size = 128u << 20;
}

static void magic(TegraNVENCSubmit *s)
{
    setupOf(s)->magic = 0xd0b70005u;
}

static void oddWidth(TegraNVENCSubmit *s)
{
    setupOf(s)->input_cfg.frame_width_minus1 = 1279 - 16;
}

static void twoPass(TegraNVENCSubmit *s)
{
    setupOf(s)->rate_control.two_pass_rc = 1;
}

static void refRelocation(TegraNVENCSubmit *s)
{
    s->entries[s->entry_count] = s->entries[s->entry_count - 1];
    s->entries[s->entry_count - 1] = (TegraNVENCEntry){NVC5B7_SET_IN_REF_PIC0_LUMA, TEGRA_NVENC_ENTRY_RELOC, 2, 0};
    s->entry_count++;
}

static void checkRefusals(void)
{
    static const struct {
        const char *name;
        Break apply;
    } kBreaks[] = {
        {"a P picture", pPicture},
        {"a reference in list 0", reference},
        {"temporal MVP", temporalMvp},
        {"SAO", sao},
        {"a misaligned ME control", meMisaligned},
        {"one CTB a slice", fewCtus},
        {"a bitstream past its buffer", noBitstream},
        {"another magic", magic},
        {"a width not of whole CTBs", oddWidth},
        {"two-pass RC", twoPass},
        {"a reference relocation", refRelocation},
    };

    HEVCEncConfig c = {.width = 1280, .height = 720};
    hevcnvenc_configure(&c);
    H264EncLayout l;
    hevcnvenc_layout(&c, &l);
    const H264EncHandles h = {1, 2, 3, 4, 5, {6, 7}};
    const HEVCEncPicture idr = {true, 0, 30};
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
    for (size_t k = 0; k < sizeof(kBreaks) / sizeof(kBreaks[0]); k++) {
        static TegraNVENCSubmit s;
        hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &h, &s);
        kBreaks[k].apply(&s);
        for (uint32_t i = 0; i < TEGRA_NVENC_MAX_ENTRIES; i++)
            avail[i] = 64u << 20;
        CHECK(tegra_nvenc_check(&s, avail) != NULL, "the checker took %s", kBreaks[k].name);
    }
}

// HEVCEncSyntax.c's SPS and PPS state the tools the setup does (the firmware codes the slices by the setup, decoders
// read them by the parameter sets).
static void checkSyntax(void)
{
    HEVCEncConfig c = {.width = 1280, .height = 720, .fps_num = 30, .fps_den = 1};
    hevcnvenc_configure(&c);
    uint8_t sps[192], pps[64];
    const size_t spsSize = hevcenc_write_sps(&c, sps, sizeof(sps)), ppsSize = hevcenc_write_pps(&c, pps, sizeof(pps));
    static HEVCParams params;
    memset(&params, 0, sizeof(params));
    CHECK(hevc_parse_sps(&params, sps, spsSize) == 0 && hevc_parse_pps(&params, pps, ppsSize) == 0,
          "AVD's parser refused the SPS or PPS");
    H264EncLayout l;
    hevcnvenc_layout(&c, &l);
    const H264EncHandles h = {1, 2, 3, 4, 5, {6, 7}};
    const HEVCEncPicture idr = {true, 0, 30};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &h, &s);
    const nvenc_h265_drv_pic_setup_s *p = (const nvenc_h265_drv_pic_setup_s *)s.setup;
    const HEVCSPS *q = &params.sps[0];
    CHECK(q->width == p->input_cfg.frame_width_minus1 + 1u && q->height == p->input_cfg.frame_height_minus1 + 1u,
          "SPS coded size %ux%u", q->width, q->height);
    CHECK(q->log2_min_cb == p->sps_data.min_cu_size + 3u &&
              q->log2_min_cb + q->log2_diff_max_min_cb == p->sps_data.max_cu_size + 3u &&
              q->log2_min_tb == p->sps_data.min_tu_size + 2u &&
              q->log2_min_tb + q->log2_diff_max_min_tb == p->sps_data.max_tu_size + 2u &&
              q->max_th_depth_inter == p->sps_data.max_transform_depth_inter &&
              q->max_th_depth_intra == p->sps_data.max_transform_depth_intra,
          "SPS coding tree");
    CHECK(q->amp == p->sps_data.amp_enabled_flag && q->sao == p->sps_data.sample_adaptive_offset_enabled_flag &&
              q->temporal_mvp == p->sps_data.sps_temporal_mvp_enabled_flag &&
              q->strong_intra_smoothing == p->sps_data.strong_intra_smoothing_enabled_flag &&
              q->log2_max_poc_lsb == p->sps_data.log2_max_pic_order_cnt_lsb_minus4 + 4u &&
              q->num_short_term_rps == p->sps_data.num_short_term_ref_pic_sets,
          "SPS tools");
    const HEVCPPS *r = &params.pps[0];
    CHECK(r->transform_skip == p->pps_data.transform_skip_enabled_flag &&
              r->loop_filter_across_slices == p->pps_data.pps_loop_filter_across_slices_enabled_flag &&
              r->cu_qp_delta == p->pps_data.cu_qp_delta_enabled_flag && !r->sign_data_hiding &&
              !r->constrained_intra_pred,
          "PPS tools");
}

int main(void)
{
    checkSize(1280, 720, kHeader1280, kSlice1280);
    checkSize(1920, 1080, kHeader1920, kSlice1920);
    checkSize(640, 360, kHeader640, kSlice640);
    checkRefusals();
    checkSyntax();
    if (gFailures) {
        printf("FAIL hevcnvenc: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS hevcnvenc\n");
    return 0;
}
