// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for HEVCNVENC.c: its IDR and first two P setups against NVIDIA's T210 driver's (L4T 32.7.6 libnvtvmr's
// H.265 path read at 1280x720, 1920x1080 and 640x360; TegraNVENC/nvidia/SOURCE-H265) byte for byte, every field but
// the surfaces' pitches and chroma offsets, the control offsets and the two fields the firmware never reads, and the P
// pictures' reference, ME predictor and picture index methods; the kernel's H.265 checker accepting them and refusing
// each kind of bad submit; and HEVCEncSyntax.c's SPS, PPS and P slice header, parsed back with AVD's parser, stating
// what the setup does. Prints "PASS hevcnvenc" when all hold.
#include <stdio.h>
#include <string.h>
#include "../../SwitchVideoDecoders/AVD/HEVCSyntax.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"
#include "../AVE/HEVCNVENC.h"
#include "hevcnvenc_goldens.h"

static int gFailures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            gFailures++;                  \
        }                                 \
    } while (0)

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

static const H264EncHandles kHandles = {.input = 1, .recon = 2, .ref = 3, .bitstream = 4, .aux = 5, .mepred = {6, 7}};

static const char *check(const TegraNVENCSubmit *s)
{
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
    for (uint32_t i = 0; i < TEGRA_NVENC_MAX_ENTRIES; i++)
        avail[i] = 64u << 20;
    return tegra_nvenc_check(s, avail);
}

// `hex` (a driver setup) with `patches` written over it, into `out`.
static const char *patched(const char *hex, const Patch *patches, size_t count, char *out, size_t cap)
{
    snprintf(out, cap, "%s", hex);
    for (size_t i = 0; i < count; i++)
        memcpy(out + 2 * patches[i].offset, patches[i].hex, strlen(patches[i].hex));
    return out;
}

static int compareSetup(const TegraNVENCSubmit *s, const char *header, const char *slice)
{
    const nvenc_h265_pic_control_s *pc = &((const nvenc_h265_drv_pic_setup_s *)s->setup)->pic_control;
    int differ = compare("setup", header, s->setup, TEGRA_NVENC_H265_SETUP_HEADER, true);
    differ += compare("slice", slice, s->setup + pc->slice_control_offset, 64, false);
    differ += compare("ME", kMe, s->setup + pc->me_control_offset, 192, false);
    differ += compare("MD", kMd, s->setup + pc->md_control_offset, 128, false);
    differ += compare("quant", kQuant, s->setup + pc->q_control_offset, 16, false);
    return differ;
}

static const TegraNVENCEntry *entry(const TegraNVENCSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++) {
        if (s->entries[i].method == method)
            return &s->entries[i];
    }
    return NULL;
}

// A P picture's methods as the driver's (SOURCE-H265): the reference's luma in the slot its list entry names, the ME
// predictors the picture before wrote in and the other buffer out, the picture index.
static void checkPMethods(const TegraNVENCSubmit *s, const HEVCEncPicture *p, uint32_t slot, const char *what)
{
    uint32_t refs = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const uint16_t m = s->entries[i].method;
        refs += m >= NVC5B7_SET_IN_REF_PIC0_LUMA && m < NVC5B7_SET_IN_REF_PIC0_CHROMA + 64;
    }
    const TegraNVENCEntry *ref = entry(s, (uint16_t)(NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * slot));
    const TegraNVENCEntry *in = entry(s, NVC5B7_SET_IN_MEPRED_DATA), *out = entry(s, NVC5B7_SET_OUT_MEPRED_DATA);
    const TegraNVENCEntry *recon = entry(s, NVC5B7_SET_OUT_REF_PIC_LUMA), *index = entry(s, NVC5B7_SET_PICTURE_INDEX);
    CHECK(refs == 1 && ref && ref->value == kHandles.ref && ref->offset == 0 && recon && recon->value == kHandles.recon,
          "%s: the reference in slot %u, the reconstruction", what, slot);
    CHECK(in && out && in->value == kHandles.mepred[~p->poc & 1] && out->value == kHandles.mepred[p->poc & 1],
          "%s: ME predictors", what);
    CHECK(index && index->value == p->picture_index, "%s: picture index", what);
}

// App 3 writes the SPS and PPS only into the stream's syntax registers; the engine codes what the MD control leaves it
// (SOURCE-H265). The stream must allow all of it: every enabled TU size from the largest CU, transform skip for 4x4
// TUs (its only engine control is a bias), and AMP when any AMP partition is enabled.
static void checkStreamAllowsEngine(const TegraNVENCSubmit *s, const char *what)
{
    const nvenc_h265_drv_pic_setup_s *setup = (const nvenc_h265_drv_pic_setup_s *)s->setup;
    const nvenc_h265_sps_data_s *sps = &setup->sps_data;
    const nvenc_h265_md_control_s *md =
        (const nvenc_h265_md_control_s *)(s->setup + setup->pic_control.md_control_offset);
    const uint32_t tus = md->tusize_4x4_enable | md->tusize_8x8_enable << 1 | md->tusize_16x16_enable << 2 |
                         md->tusize_32x32_enable << 3;
    CHECK(tus, "%s: no TU size enabled", what);
    if (!tus)
        return;
    const uint32_t smallest = (uint32_t)__builtin_ctz(tus);
    const uint32_t largest = 31u - (uint32_t)__builtin_clz(tus);
    CHECK(smallest >= sps->min_tu_size && largest <= sps->max_tu_size, "%s: enabled TUs outside the SPS's sizes", what);
    const uint32_t depth = sps->max_cu_size + 1u - smallest;
    CHECK(sps->max_transform_depth_inter >= depth && sps->max_transform_depth_intra >= depth,
          "%s: SPS transform depth %u inter, %u intra; a %u-pixel CU split to %u-pixel TUs needs %u", what,
          sps->max_transform_depth_inter, sps->max_transform_depth_intra, 8u << sps->max_cu_size, 4u << smallest,
          depth);
    CHECK(!md->tusize_4x4_enable || setup->pps_data.transform_skip_enabled_flag,
          "%s: 4x4 TUs enabled with transform skip off in the PPS", what);
    const bool amp = md->cu16x16_part_2NxnU_enable || md->cu16x16_part_2NxnD_enable || md->cu16x16_part_nLx2N_enable ||
                     md->cu16x16_part_nRx2N_enable || md->cu32x32_part_2NxnU_enable || md->cu32x32_part_2NxnD_enable ||
                     md->cu32x32_part_nLx2N_enable || md->cu32x32_part_nRx2N_enable || md->cu64x64_part_2NxnU_enable ||
                     md->cu64x64_part_2NxnD_enable || md->cu64x64_part_nLx2N_enable || md->cu64x64_part_nRx2N_enable;
    CHECK(!amp || sps->amp_enabled_flag, "%s: AMP partitions enabled with AMP off in the SPS", what);
    // Every CU size in the SPS range must keep intra prediction enabled with Planar/DC available (nvenc.bin 0x111f1..
    // 0x1133e: ip_search_mode still searches 8x8 intra on P pictures).
    static const char *const kIntraNames[3] = {"8x8", "16x16", "32x32"};
    const uint8_t intraEnable[3] = {md->intra_mode_8x8_enable, md->intra_mode_16x16_enable,
                                    md->intra_mode_32x32_enable};
    const uint32_t intraMask[3] = {md->h265_intra_luma8x8_mode_enable, md->h265_intra_luma16x16_mode_enable,
                                   md->h265_intra_luma32x32_mode_enable};
    for (uint32_t i = 0; i < 3; i++) {
        if (i < sps->min_cu_size || i > sps->max_cu_size)
            continue;
        CHECK(intraEnable[i] && (intraMask[i] & 0x3u) == 0x3u,
              "%s: intra %s disabled or missing Planar/DC (enable %u mask %#x) while in SPS CU range", what,
              kIntraNames[i], intraEnable[i], intraMask[i]);
    }
    CHECK((md->h265_intra_chroma_mode_enable & 0x1fu) != 0, "%s: no intra chroma mode enabled", what);
}

static void checkSize(uint32_t width, uint32_t height, const char *header, const char *slice)
{
    HEVCEncConfig c = {.width = width, .height = height, .fps_num = 30, .fps_den = 1};
    CHECK(hevcnvenc_configure(&c) == 0, "%ux%u: configure", width, height);
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 23};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &kHandles, &s);
    int differ = compareSetup(&s, header, slice);
    CHECK(!differ, "%ux%u: %d bytes differ from the driver's IDR setup", width, height, differ);
    CHECK(!entry(&s, NVC5B7_SET_IN_MEPRED_DATA) && !entry(&s, NVC5B7_SET_OUT_MEPRED_DATA),
          "%ux%u: ME predictors on an IDR (the driver gives none, libnvtvmr 0x3f370)", width, height);
    const char *why = check(&s);
    CHECK(!why, "%ux%u: the checker refused the IDR: %s", width, height, why ? why : "");
    checkStreamAllowsEngine(&s, "IDR");

    // The driver's first and second P pictures: its IDR setup with what they change (hevcnvenc_goldens.h).
    static char pHeader[2 * TEGRA_NVENC_H265_SETUP_HEADER + 1], pSlice[2 * 64 + 1], p2Header[sizeof(pHeader)];
    patched(header, kPSetup, sizeof(kPSetup) / sizeof(kPSetup[0]), pHeader, sizeof(pHeader));
    patched(slice, kPSlice, sizeof(kPSlice) / sizeof(kPSlice[0]), pSlice, sizeof(pSlice));
    patched(pHeader, kP2Setup, sizeof(kP2Setup) / sizeof(kP2Setup[0]), p2Header, sizeof(p2Header));
    const HEVCEncPicture p1 = {.poc = 1, .qp = 25, .picture_index = 1}, p2 = {.poc = 2, .qp = 25, .picture_index = 2};
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &p1, &kHandles, &s);
    differ = compareSetup(&s, pHeader, pSlice);
    CHECK(!differ, "%ux%u: %d bytes differ from the driver's first P setup", width, height, differ);
    checkPMethods(&s, &p1, 0, "first P");
    why = check(&s);
    CHECK(!why, "%ux%u: the checker refused the first P: %s", width, height, why ? why : "");
    checkStreamAllowsEngine(&s, "first P");
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &p2, &kHandles, &s);
    differ = compareSetup(&s, p2Header, pSlice);
    CHECK(!differ, "%ux%u: %d bytes differ from the driver's second P setup", width, height, differ);
    checkPMethods(&s, &p2, 1, "second P");
    why = check(&s);
    CHECK(!why, "%ux%u: the checker refused the second P: %s", width, height, why ? why : "");
}

typedef void (*Break)(TegraNVENCSubmit *s);

static nvenc_h265_drv_pic_setup_s *setupOf(TegraNVENCSubmit *s)
{
    return (nvenc_h265_drv_pic_setup_s *)s->setup;
}

static nvenc_h265_slice_control_s *sliceOf(TegraNVENCSubmit *s)
{
    return (nvenc_h265_slice_control_s *)(s->setup + setupOf(s)->pic_control.slice_control_offset);
}

static void insertBeforeExecute(TegraNVENCSubmit *s, TegraNVENCEntry e)
{
    s->entries[s->entry_count] = s->entries[s->entry_count - 1];
    s->entries[s->entry_count - 1] = e;
    s->entry_count++;
}

static void reloc(TegraNVENCSubmit *s, uint16_t method)
{
    insertBeforeExecute(s, (TegraNVENCEntry){method, TEGRA_NVENC_ENTRY_RELOC, 2, 0});
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
    sliceOf(s)->num_ctus = 1;
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
    reloc(s, NVC5B7_SET_IN_REF_PIC0_LUMA);
}

static void reconChromaMethod(TegraNVENCSubmit *s)
{
    reloc(s, NVC5B7_SET_OUT_REF_PIC_CHROMA);
}

static void refChromaMethod(TegraNVENCSubmit *s)
{
    reloc(s, NVC5B7_SET_IN_REF_PIC0_CHROMA);
}

static void noReconChromaOffset(TegraNVENCSubmit *s)
{
    setupOf(s)->outputpic_cfg.chroma_top_frm_offset = 0;
}

// Broken P pictures (the first P: its reference in slot 0).
static void pNoReference(TegraNVENCSubmit *s)
{
    for (uint32_t i = 0; i < s->entry_count; i++) {
        if (s->entries[i].method == NVC5B7_SET_IN_REF_PIC0_LUMA) {
            memmove(&s->entries[i], &s->entries[i + 1], (s->entry_count - i - 1) * sizeof(s->entries[0]));
            s->entry_count--;
            return;
        }
    }
}

static void pOtherSlot(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.l0[0] = 2;
}

static void pExtraSlot(TegraNVENCSubmit *s)
{
    reloc(s, NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * 5);
}

static void pField(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.l0[0] = 1;
}

static void pSecondEntry(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.l0[1] = 0;
}

static void pList1(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.l1[0] = 0;
}

static void pISlice(TegraNVENCSubmit *s)
{
    sliceOf(s)->slice_type = 2;
}

static void pIdrNal(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.nal_unit_type = 19;
}

static void pBPicture(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.pic_type = 1;
}

static void pExplicitRps(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.short_term_ref_pic_set_sps_flag = 0;
}

static void pNoPocTotal(TegraNVENCSubmit *s)
{
    setupOf(s)->pic_control.ref_pic_list_modification.NumPocTotalCurr = 0;
}

static void pTwoByOverride(TegraNVENCSubmit *s)
{
    sliceOf(s)->num_ref_idx_active_override_flag = 1;
    sliceOf(s)->num_ref_idx_l0_active_minus1 = 1;
}

static void checkRefusals(void)
{
    static const struct {
        const char *name;
        Break apply;
        bool p;  // applied to a P picture's submit, else an IDR's
    } kBreaks[] = {
        {"a P picture", pPicture, false},
        {"a reference in list 0", reference, false},
        {"temporal MVP", temporalMvp, false},
        {"SAO", sao, false},
        {"a misaligned ME control", meMisaligned, false},
        {"one CTB a slice", fewCtus, false},
        {"a bitstream past its buffer", noBitstream, false},
        {"another magic", magic, false},
        {"a width not of whole CTBs", oddWidth, false},
        {"two-pass RC", twoPass, false},
        {"a reference relocation", refRelocation, false},
        {"a reconstruction chroma method", reconChromaMethod, false},
        {"a reference chroma method", refChromaMethod, false},
        {"a reconstruction without its chroma offset", noReconChromaOffset, false},
        {"a P picture without its reference", pNoReference, true},
        {"a P picture naming a slot not relocated", pOtherSlot, true},
        {"a P picture with a slot relocated it does not name", pExtraSlot, true},
        {"a field reference", pField, true},
        {"a second list-0 entry", pSecondEntry, true},
        {"a list-1 entry", pList1, true},
        {"a P picture with an I slice", pISlice, true},
        {"a P picture with an IDR NAL type", pIdrNal, true},
        {"a B picture", pBPicture, true},
        {"an explicit reference picture set", pExplicitRps, true},
        {"a P picture with no reference counted", pNoPocTotal, true},
        {"two references by override", pTwoByOverride, true},
        {"a P reference chroma method", refChromaMethod, true},
    };

    HEVCEncConfig c = {.width = 1280, .height = 720};
    hevcnvenc_configure(&c);
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 30}, p = {.poc = 1, .qp = 32, .picture_index = 1};
    for (size_t k = 0; k < sizeof(kBreaks) / sizeof(kBreaks[0]); k++) {
        static TegraNVENCSubmit s;
        hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, kBreaks[k].p ? &p : &idr,
                              &kHandles, &s);
        kBreaks[k].apply(&s);
        CHECK(check(&s) != NULL, "the checker took %s", kBreaks[k].name);
    }
}

// A P picture's reference is bounded as a reference surface: one byte short of its chroma, it is refused.
static void checkReferenceBound(void)
{
    HEVCEncConfig c = {.width = 1280, .height = 720};
    hevcnvenc_configure(&c);
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture p = {.poc = 1, .qp = 30, .picture_index = 1};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &p, &kHandles, &s);
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
    for (uint32_t i = 0; i < TEGRA_NVENC_MAX_ENTRIES; i++) {
        const bool ref = i < s.entry_count && s.entries[i].method == NVC5B7_SET_IN_REF_PIC0_LUMA;
        avail[i] = ref ? l.recon.bytes : 64u << 20;
    }
    const char *why = tegra_nvenc_check(&s, avail);
    CHECK(!why, "a reference of recon.bytes: %s", why ? why : "");
    for (uint32_t i = 0; i < s.entry_count; i++) {
        if (s.entries[i].method == NVC5B7_SET_IN_REF_PIC0_LUMA)
            avail[i] = l.recon.bytes - 1;
    }
    CHECK(tegra_nvenc_check(&s, avail) != NULL, "the checker took a reference one byte short");
}

// With the input's chroma by method, the reconstruction's is still at its config offset: app 3 reads no other.
static void checkChromaMethodInput(void)
{
    HEVCEncConfig c = {.width = 1280, .height = 720};
    hevcnvenc_configure(&c);
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 30}, p = {.poc = 7, .qp = 30, .picture_index = 7};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_METHOD, &l, &idr, &kHandles, &s);
    const nvenc_h265_drv_pic_setup_s *q = setupOf(&s);
    CHECK(q->outputpic_cfg.chroma_top_frm_offset == l.recon.chroma_offset / 256 && !q->input_cfg.chroma_top_frm_offset,
          "chroma by method: the reconstruction's chroma offset");
    const char *why = check(&s);
    CHECK(!why, "chroma by method: the checker refused the IDR: %s", why ? why : "");
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_METHOD, &l, &p, &kHandles, &s);
    why = check(&s);
    CHECK(!why, "chroma by method: the checker refused a P: %s", why ? why : "");
}

// A P picture's reference as the SPS, PPS and slice header state it (NVENC writes the slice header from the setup):
// the SPS's one set, one picture back and used; one active reference by the PPS's default; the slice's NAL type, POC
// LSB and the SPS's set named.
static void checkPSyntax(const HEVCEncConfig *c, const HEVCParams *params, const H264EncLayout *l)
{
    const HEVCEncPicture p = {.poc = 300, .qp = 30, .picture_index = 300};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, l, &p, &kHandles, &s);
    const nvenc_h265_pic_control_s *pc = &setupOf(&s)->pic_control;
    const HEVCShortTermRPS *rps = &params->sps[0].st_rps[0];
    CHECK(rps->num_negative == 1 && !rps->num_positive && rps->delta[0][0] == -pc->temp_dist_l0[0] && rps->used[0][0] &&
              pc->short_term_ref_pic_set_sps_flag && !pc->short_term_ref_pic_set_idx &&
              pc->ref_pic_list_modification.NumPocTotalCurr == 1,
          "P: the SPS's reference picture set");
    CHECK(params->pps[0].num_ref_idx_l0_default == setupOf(&s)->pps_data.num_ref_idx_l0_active_minus1 + 1u &&
              !sliceOf(&s)->num_ref_idx_active_override_flag,
          "P: one active reference");
    uint8_t rbsp[64], nal[80];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    hevcenc_slice_header(c, &p, &b);
    h264bits_trailing(&b);
    const size_t size = h264bits_size(&b);
    nal[0] = (uint8_t)(hevcenc_nal_type(&p) << 1);
    nal[1] = 1;
    memcpy(nal + 2, rbsp, size);
    HEVCSliceHeader sh;
    CHECK(hevc_parse_slice_header(params, nal, size + 2, &sh) == 0 && sh.nal_type == pc->nal_unit_type &&
              sh.poc_lsb == pc->pic_order_cnt_lsb && sh.st_rps_from_sps == pc->short_term_ref_pic_set_sps_flag,
          "P: slice header NAL type %u POC LSB %u", sh.nal_type, sh.poc_lsb);
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
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 30};
    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &kHandles, &s);
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
    checkPSyntax(&c, &params, &l);
}

static void checkFast(void)
{
    HEVCEncConfig c = {.width = 1920, .height = 1080, .fast = true, .fps_num = 60, .fps_den = 1};
    CHECK(hevcnvenc_configure(&c) == 0, "fast: configure");
    CHECK(c.amp && c.max_tr_depth_inter == 3 && c.max_tr_depth_intra == 3, "fast: coding tree");
    uint8_t sps[192], pps[64];
    const size_t spsSize = hevcenc_write_sps(&c, sps, sizeof(sps)), ppsSize = hevcenc_write_pps(&c, pps, sizeof(pps));
    static HEVCParams params;
    memset(&params, 0, sizeof(params));
    CHECK(hevc_parse_sps(&params, sps, spsSize) == 0 && hevc_parse_pps(&params, pps, ppsSize) == 0,
          "fast: AVD parser refused SPS or PPS");
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_TILED16, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 23};
    const HEVCEncPicture p1 = {.poc = 1, .qp = 25, .picture_index = 1};
    const HEVCEncPicture p2 = {.poc = 2, .qp = 25, .picture_index = 2};
    static char pHeader[2 * TEGRA_NVENC_H265_SETUP_HEADER + 1], pSlice[2 * 64 + 1], p2Header[sizeof(pHeader)];
    patched(kHeader1920, kPSetup, sizeof(kPSetup) / sizeof(kPSetup[0]), pHeader, sizeof(pHeader));
    patched(kSlice1920, kPSlice, sizeof(kPSlice) / sizeof(kPSlice[0]), pSlice, sizeof(pSlice));
    patched(pHeader, kP2Setup, sizeof(kP2Setup) / sizeof(kP2Setup[0]), p2Header, sizeof(p2Header));

    static TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &kHandles, &s);
    const char *why = check(&s);
    CHECK(!why, "fast: checker refused IDR: %s", why ? why : "");
    int differ = compareSetup(&s, kHeader1920, kSlice1920);
    CHECK(!differ, "fast: %d bytes differ from T210 quality-level-0 IDR setup", differ);
    const nvenc_h265_drv_pic_setup_s *setup = setupOf(&s);
    CHECK(setup->sps_data.amp_enabled_flag && params.sps[0].amp, "fast: AMP on in setup and SPS");
    CHECK(setup->pps_data.transform_skip_enabled_flag && params.pps[0].transform_skip,
          "fast: transform skip on in setup and PPS");
    CHECK(params.sps[0].max_th_depth_inter == setup->sps_data.max_transform_depth_inter &&
              params.sps[0].max_th_depth_intra == setup->sps_data.max_transform_depth_intra,
          "fast: the SPS's transform depths are the setup's");
    checkStreamAllowsEngine(&s, "fast IDR");
    const TegraNVENCEntry *ctrl = entry(&s, NVC5B7_SET_CONTROL_PARAMS);
    CHECK(ctrl && (ctrl->value & (1u << 8)) && !(ctrl->value & (1u << 11)) && (ctrl->value & (1u << 12)),
          "fast: IDR control params (SLICE_STAT_ON off, FORCE_OUTPIC/GPTIMER_ON on)");

    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &p1, &kHandles, &s);
    why = check(&s);
    CHECK(!why, "fast: checker refused first P: %s", why ? why : "");
    differ = compareSetup(&s, pHeader, pSlice);
    CHECK(!differ, "fast: %d bytes differ from T210 quality-level-0 first P setup", differ);
    checkPMethods(&s, &p1, 0, "fast first P");
    checkStreamAllowsEngine(&s, "fast first P");
    ctrl = entry(&s, NVC5B7_SET_CONTROL_PARAMS);
    CHECK(ctrl && (ctrl->value & (1u << 8)) && !(ctrl->value & (1u << 11)) && (ctrl->value & (1u << 12)),
          "fast: P control params (SLICE_STAT_ON off, FORCE_OUTPIC/GPTIMER_ON on)");

    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, &l, &p2, &kHandles, &s);
    why = check(&s);
    CHECK(!why, "fast: checker refused second P: %s", why ? why : "");
    differ = compareSetup(&s, p2Header, pSlice);
    CHECK(!differ, "fast: %d bytes differ from T210 quality-level-0 second P setup", differ);
    checkPMethods(&s, &p2, 1, "fast second P");
    checkStreamAllowsEngine(&s, "fast second P");
    checkPSyntax(&c, &params, &l);
}

int main(void)
{
    checkSize(1280, 720, kHeader1280, kSlice1280);
    checkSize(1920, 1080, kHeader1920, kSlice1920);
    checkSize(640, 360, kHeader640, kSlice640);
    checkRefusals();
    checkReferenceBound();
    checkChromaMethodInput();
    checkSyntax();
    checkFast();
    if (gFailures) {
        printf("FAIL hevcnvenc: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS hevcnvenc\n");
    return 0;
}
