// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVE (H264H7.videoencoder) on a Mac: the plug-in itself (AVEEncoder.c, included for its factory) over
// a fake TegraNVENC client whose engine is a lossless stand-in for NVENC, registered in this process and driven by the
// encode check (EncodeCheck.c) through the Mac's VideoToolbox, which checks the plug-in's class layout, session calls,
// format description and sample buffers against a real VideoToolbox, and decodes the result with Apple's decoder.
//
// With TEGRA_NVENC_FEATURE_SURFACE_INPUT the fake kernel plays VIC too: a submit naming an IOSurface gets the surface's
// region written into its input buffer in the layout input_cfg states, before the engine reads it, and answers that
// it bounced the rows through the copy engine when they are off VIC's 256-byte grid.
// The fake engine is not NVENC: it checks every submit with the kernel's own checker (TegraNVENCCheck.h), then writes
// what the setup asks for the way NVENC would be asked to: the slice header from the setup's fields (type, frame_num,
// idr_pic_id, POC LSB, QP, entropy mode), and slice data that decodes exactly to the input picture it reads in the
// layout the setup declares: I_PCM macroblocks, and in P pictures P_Skip wherever the macroblock equals the reference
// the setup points at (so a wrong reference shows as a mismatch). Its reconstruction goes to the buffer OUT_REF_PIC
// names. CAVLC only (the check asks for it); the status record gets the bit count.
// Also: the SPS/PPS/avcC parse back with the decoder's own H264Syntax.c to what was configured; struct sizes match
// nvenc_drv.h's comments; the slice, ME, MD and quant controls, reference lists, slicing and history size of a
// 1280x720 High IDR and P picture are byte for byte those of NVIDIA's own T210 driver (driverControls) and the checker
// accepts them; the checker refuses each kind of bad submit derived from a good one; the layouts' offsets are a
// bijection; the rate control converges on a synthetic bits-versus-QP model; the HEVC tile slots (TileCheck.c, whose
// fake H.265 job is answered by tile_check_engine) over the same routes; the layout probe's IDRs (ProbeCheck.c).
// Prints "PASS ave ..." (test:ave_host_test).
#include "../AVE/AVEEncoder.c"
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stddef.h>
#include <stdio.h>
#include "EncodeCheck.h"
#include "ProbeCheck.h"
#include "ReorderCheck.h"
#include "TileCheck.h"
#include "../../SwitchVideoDecoders/AVD/H264Syntax.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"

extern const CFStringRef kVTVideoEncoderSpecification_EncoderID;

enum {
    MAX_HANDLES = 64
};

struct NVENCClient {
    uint8_t *va[MAX_HANDLES];
    uint64_t size[MAX_HANDLES];
};

static uint32_t gLayout = TEGRA_NVENC_LAYOUT_PITCH, gChroma = TEGRA_NVENC_CHROMA_OFFSET;
static bool gSurfaceInput;
static int gRefuseSurfaces;  // 1: every surface (kIOReturnUnsupported); 2: every other one, for now (NotReady)
static int gOpens, gSubmits, gRefused, gSkipped, gPcm, gIdr, gP, gB, gVIC;
static bool gRefuseB;       // the engine answers every B picture with an error, as NVENC would one it cannot code
static uint32_t gFailWait;  // the job whose Wait answers a failure: a producer of its input was cancelled
static const char *gRefusal;
static TegraNVENCSubmit gFirstP;  // a good P submit, for the refusal cases
static uint64_t gFirstPAvail[TEGRA_NVENC_MAX_ENTRIES];
static bool gHaveP;

bool nvenc_service_present(void)
{
    return true;
}

NVENCClient *nvenc_open(int *error)
{
    (void)error;
    gOpens++;
    return calloc(1, sizeof(NVENCClient));
}

void nvenc_close(NVENCClient *c)
{
    if (!c)
        return;
    for (int i = 0; i < MAX_HANDLES; i++)
        free(c->va[i]);
    free(c);
}

bool nvenc_info(NVENCClient *c, TegraNVENCInfo *info)
{
    (void)c;
    memset(info, 0, sizeof(*info));
    info->abi_version = TEGRA_NVENC_ABI_VERSION;
    info->nvenc_version = 0x50;
    info->codecs = 1u << TEGRA_NVENC_CODEC_H264 | 1u << TEGRA_NVENC_CODEC_H265;
    info->input_layout = gLayout;
    info->chroma_mode = gChroma;
    info->p_frames = 1;
    info->features = gSurfaceInput ? TEGRA_NVENC_FEATURE_SURFACE_INPUT : 0;
    info->surface_row_align = gSurfaceInput ? 256 : 0;
    return true;
}

int nvenc_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    NVENCClient *c = ctx;
    size = (size + 65535) & ~65535ull;  // the kernel's granule
    for (uint32_t h = 1; h < MAX_HANDLES; h++) {
        if (c->va[h])
            continue;
        if (!(c->va[h] = calloc(1, size)))
            return -1;
        c->size[h] = size;
        *handle = h;
        *va = c->va[h];
        return 0;
    }
    return -1;
}

void nvenc_free(void *ctx, uint32_t handle)
{
    NVENCClient *c = ctx;
    free(c->va[handle]);
    c->va[handle] = NULL;
}

int nvenc_wait(void *ctx, const TegraNVENCWait *w)
{
    (void)ctx;
    return gFailWait && w->fence == gFailWait ? kIOReturnError : 0;
}

static const TegraNVENCEntry *find(const TegraNVENCSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method)
            return &s->entries[i];
    return NULL;
}

// One macroblock's 384 samples (luma raster, then Cb, then Cr) from an NV12 surface in `layout`.
static void readMacroblock(const uint8_t *surface, uint32_t layout, uint32_t pitch, uint32_t chromaOffset, uint32_t mbx,
                           uint32_t mby, uint8_t out[384])
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            out[y * 16 + x] = surface[h264enc_surface_offset(layout, pitch, mbx * 16 + x, mby * 16 + y)];
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) {
            const uint8_t *c = surface + chromaOffset;
            out[256 + y * 8 + x] = c[h264enc_surface_offset(layout, pitch, mbx * 16 + 2 * x, mby * 8 + y)];
            out[320 + y * 8 + x] = c[h264enc_surface_offset(layout, pitch, mbx * 16 + 2 * x + 1, mby * 8 + y)];
        }
}

// The fake reads block_height as the GOB count it names.
static uint32_t layoutOf(const nvenc_h264_surface_cfg_s *cfg)
{
    if (cfg->tiled_16x16)
        return TEGRA_NVENC_LAYOUT_TILED16;
    for (uint32_t layout = 0; layout < TEGRA_NVENC_LAYOUTS; layout++)
        if (layout != TEGRA_NVENC_LAYOUT_TILED16 && tegra_nvenc_layout_gobs(layout) == cfg->block_height)
            return layout;
    return TEGRA_NVENC_LAYOUTS;
}

static int engine(NVENCClient *c, const TegraNVENCSubmit *s)
{
    const nvenc_h264_drv_pic_setup_s *p = (const nvenc_h264_drv_pic_setup_s *)s->setup;
    const nvenc_h264_pic_control_s *pc = &p->pic_control;
    const nvenc_h264_slice_control_s *slice = (const nvenc_h264_slice_control_s *)(s->setup + pc->slice_control_offset);
    const uint32_t width = p->input_cfg.frame_width_minus1 + 1u, height = p->input_cfg.frame_height_minus1 + 1u;
    const uint32_t wmbs = (width + 15) / 16, hmbs = (height + 15) / 16;
    const bool pPicture = pc->pic_type == H264ENC_P, idr = pc->pic_type == H264ENC_IDR;
    const bool bPicture = pc->pic_type == H264ENC_B;
    if (p->pps_data.entropy_coding_mode_flag)
        return -1;  // CAVLC only (see above)
    if (bPicture && gRefuseB) {
        const TegraNVENCEntry *record = find(s, NVC5B7_SET_OUT_ENC_STATUS);
        nvenc_pic_stat_s *refusal = (nvenc_pic_stat_s *)(c->va[record->value] + record->offset);
        memset(refusal, 0, sizeof(*refusal));
        refusal->error_status = 2;
        refusal->ucode_error_status = 0xe2640180u & 0x3fffffffu;  // the app's error, in the record's 30 bits
        return 0;
    }
    const TegraNVENCEntry *in = find(s, NVC5B7_SET_IN_CUR_PIC), *out = find(s, NVC5B7_SET_OUT_REF_PIC_LUMA);
    const TegraNVENCEntry *ref = find(s, NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * (((unsigned char)pc->l0[0] >> 1) & 15));
    const TegraNVENCEntry *bits = find(s, NVC5B7_SET_OUT_BITSTREAM);
    const TegraNVENCEntry *status = find(s, NVC5B7_SET_OUT_ENC_STATUS);
    const TegraNVENCEntry *inChroma = find(s, NVC5B7_SET_IN_CUR_PIC_CHROMA_U);
    // Where chroma is, under the chroma mode the fake was told the hardware proved.
    const uint32_t inOff =
        gChroma == TEGRA_NVENC_CHROMA_OFFSET ? p->input_cfg.chroma_top_frm_offset * 256 : inChroma->offset - in->offset;
    const uint32_t refOff = gChroma == TEGRA_NVENC_CHROMA_OFFSET ? p->outputpic_cfg.chroma_top_frm_offset * 256
                                                                 : find(s, NVC5B7_SET_OUT_REF_PIC_CHROMA)->offset;
    const uint8_t *input = c->va[in->value] + in->offset;
    uint8_t *recon = c->va[out->value] + out->offset;
    const uint8_t *reference = pPicture ? c->va[ref->value] + ref->offset : NULL;
    const uint32_t layout = layoutOf(&p->input_cfg), pitch = p->input_cfg.sfc_pitch,
                   rpitch = p->outputpic_cfg.sfc_pitch;

    const H264EncConfig config = {
        .width = width, .height = height, .profile_idc = p->sps_data.profile_idc, .cabac = false};
    const H264EncPicture picture = {
        .type = (uint8_t)pc->pic_type,
        .qp = (uint8_t)slice->qp_avr,
        .frame_num = pc->frame_num,
        .idr_pic_id = pc->idr_pic_id,
        .poc_lsb = pc->pic_order_cnt_lsb,
        .unreferenced = !pc->ref_pic_flag,
    };
    const size_t cap = (size_t)wmbs * hmbs * 400 + 4096;
    uint8_t *rbsp = malloc(cap);
    H264Bits b;
    h264bits_init(&b, rbsp, cap);
    h264enc_slice_header(&config, &picture, &b);
    uint32_t skip = 0;
    for (uint32_t mby = 0; mby < hmbs; mby++)
        for (uint32_t mbx = 0; mbx < wmbs; mbx++) {
            uint8_t mb[384], prev[384];
            readMacroblock(input, layout, pitch, inOff, mbx, mby, mb);
            if (pPicture) {
                readMacroblock(reference, TEGRA_NVENC_LAYOUT_PITCH, rpitch, refOff, mbx, mby, prev);
                if (!memcmp(mb, prev, sizeof(mb))) {
                    skip++;
                    gSkipped++;
                    continue;
                }
                h264bits_ue(&b, skip);  // mb_skip_run
                skip = 0;
            } else if (bPicture) {
                h264bits_ue(&b, 0);  // mb_skip_run: no B_Skip, every macroblock I_PCM
            }
            h264bits_ue(&b, pPicture ? 30 : bPicture ? 48 : 25);  // mb_type I_PCM (5 + 25 in a P slice, 23 + 25 in a B)
            h264bits_align_zero(&b);
            for (int i = 0; i < 384; i++)
                h264bits_u(&b, mb[i], 8);
            gPcm++;
            for (uint32_t y = 0; y < 16; y++)  // the reconstruction is the picture itself (pitch linear here)
                memcpy(recon + (size_t)(mby * 16 + y) * rpitch + mbx * 16, mb + y * 16, 16);
            for (uint32_t y = 0; y < 8; y++)
                for (uint32_t x = 0; x < 8; x++) {
                    uint8_t *cr = recon + refOff + (size_t)(mby * 8 + y) * rpitch + mbx * 16 + 2 * x;
                    cr[0] = mb[256 + y * 8 + x];
                    cr[1] = mb[320 + y * 8 + x];
                }
        }
    if (skip)
        h264bits_ue(&b, skip);  // the run that ends the slice
    // Skipped macroblocks keep the reference's samples (P_Skip with a zero vector: every neighbour is I_PCM or
    // skipped with vector 0).
    if (pPicture)
        for (uint32_t mby = 0; mby < hmbs; mby++)
            for (uint32_t mbx = 0; mbx < wmbs; mbx++) {
                uint8_t mb[384], prev[384];
                readMacroblock(input, layout, pitch, inOff, mbx, mby, mb);
                readMacroblock(reference, TEGRA_NVENC_LAYOUT_PITCH, rpitch, refOff, mbx, mby, prev);
                if (memcmp(mb, prev, sizeof(mb)))
                    continue;
                for (uint32_t y = 0; y < 16; y++)
                    memcpy(recon + (size_t)(mby * 16 + y) * rpitch + mbx * 16,
                           reference + (size_t)(mby * 16 + y) * rpitch + mbx * 16, 16);
                for (uint32_t y = 0; y < 8; y++)
                    memcpy(recon + refOff + (size_t)(mby * 8 + y) * rpitch + mbx * 16,
                           reference + refOff + (size_t)(mby * 8 + y) * rpitch + mbx * 16, 16);
            }
    h264bits_trailing(&b);
    uint8_t *bs = c->va[bits->value] + bits->offset;
    bs[0] = bs[1] = bs[2] = 0;
    bs[3] = 1;
    const size_t n = h264enc_nal(!pc->ref_pic_flag ? 0x01
                                 : idr             ? 0x65
                                                   : 0x61,
                                 rbsp, h264bits_size(&b), bs + 4, pc->bitstream_buf_size - 4);
    free(rbsp);
    if (!n)
        return -1;
    nvenc_pic_stat_s *stat = (nvenc_pic_stat_s *)(c->va[status->value] + status->offset);
    memset(stat, 0, sizeof(*stat));
    stat->error_status = 2;  // what NVENC 5.0's H.264 app writes in every record (AVEEncoder.c)
    stat->total_bit_count = (uint32_t)(n + 4) * 8;
    stat->num_slices = 1;
    stat->pic_type = (unsigned short)pc->pic_type;
    if (idr)
        gIdr++;
    if (pPicture)
        gP++;
    if (bPicture)
        gB++;
    return 0;
}

// What VIC does for a submit naming a surface (TegraNVENCSource.cpp): the region, 1:1, at the origin of the buffer
// IN_CUR_PIC and CHROMA_U relocate into, in input_cfg's layout. The route (TEGRA_NVENC_INPUT_*), or -1 for a submit the
// kernel would refuse.
static int fakeVIC(NVENCClient *c, const TegraNVENCSubmit *s)
{
    const nvenc_h264_surface_cfg_s *cfg =
        (const nvenc_h264_surface_cfg_s *)(s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg));
    const TegraNVENCEntry *luma = find(s, NVC5B7_SET_IN_CUR_PIC), *chroma = find(s, NVC5B7_SET_IN_CUR_PIC_CHROMA_U);
    const TegraNVENCSource *r = &s->source;
    IOSurfaceRef surface = IOSurfaceLookup(r->surface_id);
    if (cfg->tiled_16x16 || !luma || !chroma || luma->value != chroma->value || !surface ||
        r->x + r->width > IOSurfaceGetWidth(surface) || r->y + r->height > IOSurfaceGetHeight(surface)) {
        if (surface)
            CFRelease(surface);
        return -1;
    }
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(surface, 0), *uv = IOSurfaceGetBaseAddressOfPlane(surface, 1);
    const size_t ys = IOSurfaceGetBytesPerRowOfPlane(surface, 0), uvs = IOSurfaceGetBytesPerRowOfPlane(surface, 1);
    uint8_t *lumaOut = c->va[luma->value] + luma->offset;
    uint8_t *chromaOut = c->va[chroma->value] + chroma->offset + cfg->chroma_top_frm_offset * 256;
    const uint32_t layout = layoutOf(cfg), pitch = cfg->sfc_pitch;
    for (uint32_t row = 0; row < r->height; row++)
        for (uint32_t x = 0; x < r->width; x++) {
            lumaOut[h264enc_surface_offset(layout, pitch, x, row)] = y[(r->y + row) * ys + r->x + x];
            if (row < r->height / 2)
                chromaOut[h264enc_surface_offset(layout, pitch, x, row)] = uv[(r->y / 2 + row) * uvs + r->x + x];
        }
    const bool offGrid =
        IOSurfaceGetBytesPerRowOfPlane(surface, 0) % 256 || IOSurfaceGetBytesPerRowOfPlane(surface, 1) % 256;
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    CFRelease(surface);
    gVIC++;
    return offGrid ? TEGRA_NVENC_INPUT_BOUNCED : TEGRA_NVENC_INPUT_VIC;
}

int nvenc_submit(void *ctx, const TegraNVENCSubmit *s, uint32_t *fence, uint32_t *input)
{
    NVENCClient *c = ctx;
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (e->value >= MAX_HANDLES || !c->va[e->value] || e->offset > c->size[e->value])
            return -1;
        avail[i] = c->size[e->value] - e->offset;
    }
    const char *why = tegra_nvenc_check(s, avail);
    if (why) {
        gRefused++;
        gRefusal = why;
        return -1;
    }
    static int sourced;
    if (s->source.surface_id && gRefuseSurfaces == 2 && sourced++ % 2)
        return kIOReturnNotReady;
    const int route = !s->source.surface_id                    ? TEGRA_NVENC_INPUT_CPU
                      : !gSurfaceInput || gRefuseSurfaces == 1 ? -1
                                                               : fakeVIC(c, s);
    if (route < 0)
        return kIOReturnUnsupported;
    if (input)
        *input = (uint32_t)route;
    if (s->codec == TEGRA_NVENC_CODEC_H265) {
        if (tile_check_engine(s, c->va, gChroma))
            return -1;
        *fence = (uint32_t)++gSubmits;
        return 0;
    }
    const nvenc_h264_drv_pic_setup_s *p = (const nvenc_h264_drv_pic_setup_s *)s->setup;
    if (!gHaveP && p->pic_control.pic_type == H264ENC_P) {
        gFirstP = *s;
        memcpy(gFirstPAvail, avail, sizeof(avail));
        gHaveP = true;
    }
    if (engine(c, s))
        return -1;
    *fence = (uint32_t)++gSubmits;
    return 0;
}

static int failures;
#define EXPECT(cond, ...)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            failures++;                   \
        }                                 \
    } while (0)

static void parameterSets(void)
{
    H264EncConfig c = {.width = 1278,
                       .height = 718,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .full_range = true,
                       .colour_description = true,
                       .colour_primaries = 1,
                       .transfer = 1,
                       .matrix = 1,
                       .fps_num = 60,
                       .fps_den = 1};
    EXPECT(!h264enc_configure(&c), "configure 1278x718");
    EXPECT(c.level_idc == 32, "level for 720p60 is %u, expected 32", c.level_idc);
    uint8_t sps[160], pps[48];
    const size_t spsSize = h264enc_write_sps(&c, sps, sizeof(sps)), ppsSize = h264enc_write_pps(&c, pps, sizeof(pps));
    static H264ParamSets sets;
    uint8_t id = 0xff;
    EXPECT(spsSize && !h264_parse_sps(&sets, sps, spsSize, &id) && id == 0, "SPS parses");
    const H264SPS *s = &sets.sps[0];
    EXPECT(s->profile_idc == 100 && s->level_idc == 32 && s->width_mbs == 80 && s->height_map_units == 45 &&
               s->crop_right == 2 && s->crop_bottom == 2 && s->frame_mbs_only && s->log2_max_frame_num == 16 &&
               s->poc_type == 0 && s->log2_max_poc_lsb == 16 && s->max_num_ref_frames == 1 && s->full_range &&
               s->colour_description && s->matrix == 1 && s->bitstream_restriction && !s->max_num_reorder_frames,
           "SPS fields");
    EXPECT(ppsSize && !h264_parse_pps(&sets, pps, ppsSize, &id) && id == 0, "PPS parses");
    const H264PPS *q = &sets.pps[0];
    EXPECT(q->entropy_coding_mode && q->transform_8x8_mode && q->deblocking_filter_control_present &&
               q->num_ref_idx_active[0] == 1 && !q->weighted_pred,
           "PPS fields");
    uint8_t avcc[256];
    const size_t avccSize = h264enc_write_avcc(&c, avcc, sizeof(avcc));
    EXPECT(avccSize == 6 + 2 + spsSize + 1 + 2 + ppsSize + 4 && avcc[0] == 1 && avcc[1] == 100 && avcc[4] == 0xff &&
               avcc[5] == 0xe1 && !memcmp(avcc + 8, sps, spsSize),
           "avcC");
    // The slice header the setup asks for parses with the same parameter sets.
    const H264EncPicture pic = {.type = H264ENC_P, .qp = 30, .frame_num = 7, .poc_lsb = 14};
    uint8_t rbsp[64], nal[80];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264enc_slice_header(&c, &pic, &b);
    h264bits_trailing(&b);
    const size_t n = h264enc_nal(0x61, rbsp, h264bits_size(&b), nal, sizeof(nal));
    H264Slice sh;
    EXPECT(n && !h264_parse_slice(&sets, nal, n, &sh) && sh.slice_type == H264_SLICE_P && sh.frame_num == 7 &&
               sh.poc_lsb == 14 && sh.nal_ref_idc == 3,
           "slice header");
    H264EncConfig base = {.width = 640, .height = 480, .profile_idc = H264_PROFILE_BASELINE, .cabac = true};
    EXPECT(!h264enc_configure(&base) && !base.cabac && !base.transform8x8, "Baseline drops CABAC and 8x8");
    H264EncConfig odd = {.width = 641, .height = 480, .profile_idc = H264_PROFILE_HIGH};
    EXPECT(h264enc_configure(&odd), "odd width refused");
    H264EncConfig big = {.width = 4096, .height = 4096, .profile_idc = H264_PROFILE_HIGH};
    EXPECT(h264enc_configure(&big), "4096x4096 refused (more than 36864 macroblocks)");
}

static void layouts(void)
{
    enum {
        PITCH = 128,
        ROWS = 128  // a BL16 block
    };

    static uint8_t seen[PITCH * ROWS];
    for (uint32_t layout = 0; layout < TEGRA_NVENC_LAYOUTS; layout++) {
        memset(seen, 0, sizeof(seen));
        bool ok = true;
        for (uint32_t y = 0; y < ROWS; y++)
            for (uint32_t x = 0; x < PITCH; x++) {
                const size_t o = h264enc_surface_offset(layout, PITCH, x, y);
                if (o >= sizeof(seen) || seen[o]++)
                    ok = false;
            }
        EXPECT(ok, "layout %u is not a bijection on %ux%u", layout, PITCH, ROWS);
    }
    // NVDEC's GOB-height-2 formula (SwitchVideoDecoders/AVD/BlockLinear.h) is BL2's.
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 128; x++) {
            const uint32_t gob =
                ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
            const size_t nvdec = (y / 16) * 128 * 16 + (x / 64) * 1024 + ((y % 16) / 8) * 512 + gob;
            if (h264enc_surface_offset(TEGRA_NVENC_LAYOUT_BL2, 128, x, y) != nvdec) {
                EXPECT(false, "BL2 differs from NVDEC's block linear at %u,%u", x, y);
                return;
            }
        }
}

// NVIDIA's own T210 driver's H.264 picture setups (NVENC 5.0, 1280x720 High profile CABAC 8x8, QP 24), captured on a
// Switch and published by github.com/TomasUribe/switch-frame-tap, logs/m79-runE-setups/setup_3.bin (IDR) and
// setup_5.bin (P): their controls up to the last non-zero byte (every later byte of each block is 0), and the
// pic_control lists (l0, l1, temp_dist_l0/l1, dist_scale_factor). H264NVENC.c's controls must be these bytes exactly,
// which also proves the bitfields land where the driver's do (TegraNVENC/nvidia/SOURCE).
static const uint8_t kDriverMe[156] = {
    0x26, 0x40, 0x07, 0x00, 0x01, 0x40, 0x00, 0x08, 0xff, 0x1f, 0xff, 0x07, 0x00, 0x00, 0x90, 0x28, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0xa4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1c, 0x1c, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x3e,
    0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x3e, 0x7f, 0x3e, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x89, 0x1c, 0x1c, 0x7f, 0x1c, 0x1c, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6e, 0x04, 0xfe, 0x07, 0x49, 0x56, 0x00, 0x06, 0x00, 0x00, 0x88, 0x46,
};
static const uint8_t kDriverMd[2][64] = {
    {0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x03, 0x7f, 0x0f, 0x0f, 0x08, 0x00, 0x00, 0x00, 0x00,  // IDR
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xea, 0x9c, 0x00, 0x00, 0x8b, 0x08, 0x11, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0x03, 0x7f, 0x0f, 0x0f, 0x08, 0x00, 0x00, 0x00, 0x00,  // P
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xea, 0x9c, 0x00, 0x00, 0x8b, 0x08, 0x11, 0x00, 0x00, 0x00},
};
static const uint8_t kDriverQ[148] = {
    0x6b, 0x05, 0xff, 0xaa, 0xaa, 0xff, 0x0f, 0x00, 0x04, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x6e,
    0x03, 0x34, 0x03, 0xaa, 0x02, 0x6e, 0x03, 0x34, 0x03, 0xaa, 0x02, 0x00, 0x02, 0x34, 0x03, 0xaa, 0x02, 0x00, 0x02,
    0x9a, 0x01, 0xaa, 0x02, 0x00, 0x02, 0x9a, 0x01, 0x9a, 0x01, 0xaa, 0x02, 0x4a, 0x02, 0x22, 0x02, 0xc8, 0x01, 0x4a,
    0x02, 0x22, 0x02, 0xc8, 0x01, 0x56, 0x01, 0x22, 0x02, 0xc8, 0x01, 0x56, 0x01, 0x24, 0x01, 0xc8, 0x01, 0x56, 0x01,
    0x24, 0x01, 0x12, 0x01, 0xaa, 0x02, 0x56, 0x01, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa,
    0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02, 0xaa, 0x02,
    0xaa, 0x02, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56,
    0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01, 0x56, 0x01,
};
static const uint8_t kDriverSlice[12] = {0x10, 0x0e, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x33, 0x00};

// Block `b` (`size` bytes) is `expect` (`n` bytes) followed by zeros.
static bool sameBlock(const uint8_t *b, size_t size, const uint8_t *expect, size_t n)
{
    if (memcmp(b, expect, n))
        return false;
    for (size_t i = n; i < size; i++)
        if (b[i])
            return false;
    return true;
}

static void driverControls(void)
{
    H264EncConfig c = {.width = 1280,
                       .height = 720,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .fps_num = 60,
                       .fps_den = 1,
                       .input_layout = TEGRA_NVENC_LAYOUT_TILED16,
                       .chroma_mode = TEGRA_NVENC_CHROMA_OFFSET};
    EXPECT(!h264enc_configure(&c), "configure 1280x720");
    H264EncLayout l;
    h264enc_layout(&c, &l);
    EXPECT(l.history_bytes == 706560, "history at 1280x720 is %u bytes, NVIDIA's driver gives 706560", l.history_bytes);
    const H264EncHandles h = {.input = 1, .recon = 2, .ref = 3, .bitstream = 4, .aux = 5, .mepred = {6, 7}};
    const uint64_t sizes[8] = {0,           l.input.bytes,  l.recon.bytes, l.recon.bytes, l.bitstream_bytes,
                               l.aux_bytes, l.mepred_bytes, l.mepred_bytes};
    for (int k = 0; k < 2; k++) {
        const H264EncPicture pic = {.type = k ? H264ENC_P : H264ENC_IDR, .qp = 24, .frame_num = (uint32_t)k};
        static TegraNVENCSubmit s;
        h264enc_fill_submit(&c, &l, &pic, &h, &s);
        const nvenc_h264_drv_pic_setup_s *p = (const nvenc_h264_drv_pic_setup_s *)s.setup;
        const nvenc_h264_pic_control_s *pc = &p->pic_control;
        const char *name = k ? "P" : "IDR";
        EXPECT(
            sameBlock(s.setup + pc->me_control_offset, sizeof(nvenc_h264_me_control_s), kDriverMe, sizeof(kDriverMe)),
            "%s: ME control differs from NVIDIA's T210 driver's", name);
        EXPECT(sameBlock(s.setup + pc->md_control_offset, sizeof(nvenc_h264_md_control_s), kDriverMd[k],
                         sizeof(kDriverMd[k])),
               "%s: MD control differs from NVIDIA's T210 driver's", name);
        EXPECT(
            sameBlock(s.setup + pc->q_control_offset, sizeof(nvenc_h264_quant_control_s), kDriverQ, sizeof(kDriverQ)),
            "%s: quant control differs from NVIDIA's T210 driver's", name);
        EXPECT(sameBlock(s.setup + pc->slice_control_offset, sizeof(nvenc_h264_slice_control_s), kDriverSlice,
                         sizeof(kDriverSlice)),
               "%s: slice control differs from NVIDIA's T210 driver's", name);
        // l0/l1 -2 past the references, temporal distances and scale factors -1 but a P picture's first.
        uint8_t lists[160];
        memset(lists, 0xfe, 16);
        memset(lists + 16, 0xff, 144);
        if (k) {
            lists[0] = 0;
            lists[16] = 1;
            for (int r = 0; r < 8; r++)
                lists[32 + 16 * r] = 0x00, lists[33 + 16 * r] = 0x01;
        }
        EXPECT(!memcmp(pc->l0, lists, sizeof(lists)), "%s: reference lists differ from NVIDIA's T210 driver's", name);
        // The flags word (pic_struct .. cabac_zero_word_enable): 0x9c for the driver's IDR, 0x90 for its P.
        EXPECT(((const uint8_t *)pc)[0xe0] == (k ? 0x90 : 0x9c) && pc->slice_mode == 0 && pc->max_slice_size == 3600 &&
                   pc->hist_buf_size == 706560,
               "%s: slicing or history differ from NVIDIA's T210 driver's (flags %#x, max_slice_size %u, history %u)",
               name, ((const uint8_t *)pc)[0xe0], pc->max_slice_size, pc->hist_buf_size);
        uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
        for (uint32_t i = 0; i < s.entry_count; i++)
            if (s.entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
                avail[i] = sizes[s.entries[i].value] - s.entries[i].offset;
        const char *why = tegra_nvenc_check(&s, avail);
        EXPECT(!why, "%s at 1280x720: the checker refuses it: %s", name, why ? why : "");
        // The methods of NVIDIA's T210 driver's jobs (switch-frame-tap m79-runE; every one of its 68 captured jobs
        // names reference slot 0): the plug-in's, plus the two the kernel adds (the setup slot, the RC process state);
        // the kernel writes 0 in every other address method, as that driver leaves them.
        static const uint16_t kDriverMethods[] = {
            NVC5B7_SET_APPLICATION_ID,      NVC5B7_SET_CONTROL_PARAMS,   NVC5B7_SET_PICTURE_INDEX,
            NVC5B7_SET_IN_DRV_PIC_SETUP,    NVC5B7_SET_OUT_ENC_STATUS,   NVC5B7_SET_IO_RC_PROCESS,
            NVC5B7_SET_OUT_BITSTREAM,       NVC5B7_SET_IOHISTORY,        NVC5B7_SET_IN_CUR_PIC,
            NVC5B7_SET_IN_CUR_PIC_CHROMA_U, NVC5B7_SET_OUT_REF_PIC_LUMA, NVC5B7_SET_IN_MEPRED_DATA,
            NVC5B7_SET_OUT_MEPRED_DATA,     NVC5B7_SET_IN_REF_PIC0_LUMA, NVC5B7_EXECUTE,
        };
        const uint32_t driverCount = sizeof(kDriverMethods) / sizeof(kDriverMethods[0]);
        uint32_t matched = 2;  // the kernel's two
        bool extra = false;
        for (uint32_t i = 0; i < s.entry_count; i++) {
            bool found = false;
            for (uint32_t j = 0; j < sizeof(kDriverMethods) / sizeof(kDriverMethods[0]); j++)
                found |= s.entries[i].method == kDriverMethods[j];
            matched += found;
            extra |= !found;
        }
        EXPECT(!extra && matched == driverCount, "%s: methods differ from NVIDIA's T210 driver's (%u of %u, extra %d)",
               name, matched, driverCount, extra);
        uint32_t control = 0;
        for (uint32_t i = 0; i < s.entry_count; i++)
            if (s.entries[i].method == NVC5B7_SET_CONTROL_PARAMS)
                control = s.entries[i].value;
        EXPECT((control & 0xffff) == 0x1103, "%s: control params %#x, NVIDIA's driver sends 0x1103 below its RC mode",
               name, control);
        // The predictors alternate: this picture reads the buffer the one before wrote.
        uint32_t in = 0, out = 0;
        for (uint32_t i = 0; i < s.entry_count; i++) {
            if (s.entries[i].method == NVC5B7_SET_IN_MEPRED_DATA)
                in = s.entries[i].value;
            if (s.entries[i].method == NVC5B7_SET_OUT_MEPRED_DATA)
                out = s.entries[i].value;
        }
        EXPECT(in && out && in != out, "%s: ME predictors in %u out %u", name, in, out);
    }
    // The history the checker asks for: one row of 128-byte records (nvenc_drv.h 2.3) below NVIDIA's T210 size, which
    // is 192 bytes a macroblock and a row more; never less than the row.
    EXPECT(tegra_nvenc_history_bytes(64, 64) == 4 * 5 * 192 &&
               tegra_nvenc_history_bytes(1920, 1080) == 120 * 69 * 192 &&
               tegra_nvenc_history_bytes(32, 32) == (2 + 16) * 128,
           "tegra_nvenc_history_bytes");
}

static void sizes(void)
{
    EXPECT(sizeof(nvenc_h264_drv_pic_setup_s) == 512 && sizeof(nvenc_h264_pic_control_s) == 276 &&
               sizeof(nvenc_h264_rc_s) == 88 && sizeof(nvenc_h264_me_control_s) == 192 &&
               sizeof(nvenc_h264_md_control_s) == 128 && sizeof(nvenc_h264_quant_control_s) == 192 &&
               sizeof(nvenc_h264_slice_control_s) == 128 && sizeof(nvenc_pic_stat_s) == 128 &&
               sizeof(nvenc_h264_coloc_mb_s) == 64 && offsetof(nvenc_h264_drv_pic_setup_s, gpTimer_timeout_val) == 508,
           "nvenc_drv.h struct sizes differ from its comments");
}

// Each mutation of a good P submit must be refused, for its own reason.
static void refusals(void)
{
    if (!gHaveP) {
        EXPECT(false, "no P picture reached the engine");
        return;
    }

    static const struct {
        const char *name, *why;
    } cases[] = {
        {"magic", "magic"},
        {"no status", "missing relocation"},
        {"bitstream bigger than its buffer", "bitstream"},
        {"history of 128 bytes", "history"},
        {"ME-only", "ME-only"},
        {"B picture", "picture structure or type"},
        {"list 0 names an unrelocated slot", "reference slot"},
        {"MPEC statistics", "control params"},
        {"GP timer off", "control params"},
        {"control past the setup", "control offset"},
        {"execute not last", "execute not last"},
        {"input buffer too small", "surface too small"},
        {"stats FIFO", "picture features"},
        {"two slices", "control counts"},
        {"application id after another method", "application id not first"},
        {"slice_mode 1 (hardware-chained slices)", "slice mode"},
        {"max_slice_size below the macroblock count", "slice mode"},
        {"unused list-1 entry -1 instead of the driver's -2", "list 1"},
        {"unused list-0 entry -1 instead of the driver's -2", "list 0 length"},
        {"history of one 128-byte row (nvenc_drv.h 2.3), below the T210 driver's size", "history"},
        {"direct prediction", "mode decision"},
        {"ME predictors in without out", "ME predictors in without out, or out without in"},
        {"ME predictor buffer too small", "ME predictor buffers"},
        {"slice of one macroblock more than the picture", "slice control"},
        {"block height 1, which the firmware reads as 0 (no GOB count of its own)", "block height"},
        {"pitch past the firmware's 9-bit field of 16-byte units", "surface pitch"},
        {"32-GOB blocks in a buffer laid out with whole 64-row blocks", "surface too small"},
    };

    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        TegraNVENCSubmit s = gFirstP;
        uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
        memcpy(avail, gFirstPAvail, sizeof(avail));
        nvenc_h264_drv_pic_setup_s *p = (nvenc_h264_drv_pic_setup_s *)s.setup;
        nvenc_h264_pic_control_s *pc = &p->pic_control;
        switch (k) {
        case 0:
            p->magic = 0xc9b70006u;
            break;
        case 1:
            for (uint32_t i = 0; i < s.entry_count; i++)  // moved to an unused reference slot
                if (s.entries[i].method == NVC5B7_SET_OUT_ENC_STATUS)
                    s.entries[i].method = NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * 5;
            break;
        case 2:
            pc->bitstream_buf_size = 0x7fffff00u;
            break;
        case 3:
            pc->hist_buf_size = 128;
            break;
        case 4:
            ((nvenc_h264_me_control_s *)(s.setup + pc->me_control_offset))->me_only_mode = 1;
            break;
        case 5:
            pc->pic_type = 1;
            break;
        case 6:
            pc->l0[0] = 3;
            break;
        case 7:
        case 8:
            for (uint32_t i = 0; i < s.entry_count; i++)
                if (s.entries[i].method == NVC5B7_SET_CONTROL_PARAMS)
                    s.entries[i].value = k == 7 ? s.entries[i].value | (1u << 15) : s.entries[i].value & ~(1u << 12);
            break;
        case 9:
            pc->q_control_offset = s.setup_bytes - 8;
            break;
        case 10: {
            const TegraNVENCEntry last = s.entries[s.entry_count - 1];
            s.entries[s.entry_count - 1] = s.entries[0];
            s.entries[0] = last;
            const uint64_t a = avail[s.entry_count - 1];
            avail[s.entry_count - 1] = avail[0];
            avail[0] = a;
            break;
        }
        case 11:
            for (uint32_t i = 0; i < s.entry_count; i++)
                if (s.entries[i].method == NVC5B7_SET_IN_CUR_PIC)
                    avail[i] = 4096;
            break;
        case 12:
            pc->stats_fifo_enable = 1;
            break;
        case 13:
            pc->num_forced_slices_minus1 = 1;
            break;
        case 14: {
            const TegraNVENCEntry first = s.entries[0];
            s.entries[0] = s.entries[1];
            s.entries[1] = first;
            const uint64_t a = avail[0];
            avail[0] = avail[1];
            avail[1] = a;
            break;
        }
        case 15:
            pc->slice_mode = 1;
            break;
        case 16:
            pc->max_slice_size = 1;
            break;
        case 17:
            pc->l1[3] = -1;
            break;
        case 18:
            pc->l0[1] = -1;
            break;
        case 19: {
            const uint32_t width = p->input_cfg.frame_width_minus1 + 1u, height = p->input_cfg.frame_height_minus1 + 1u;
            pc->hist_buf_size = (((width - 1 + 16) / 16) + 16) * 128;
            EXPECT(pc->hist_buf_size < tegra_nvenc_history_bytes(width, height), "the clip's row is the larger");
            break;
        }
        case 20:
            ((nvenc_h264_md_control_s *)(s.setup + pc->md_control_offset))->bdirect_mode = 1;
            break;
        case 21:
            for (uint32_t i = 0; i < s.entry_count; i++)  // to an unused reference slot
                if (s.entries[i].method == NVC5B7_SET_OUT_MEPRED_DATA)
                    s.entries[i].method = NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * 6;
            break;
        case 22:
            for (uint32_t i = 0; i < s.entry_count; i++)
                if (s.entries[i].method == NVC5B7_SET_IN_MEPRED_DATA)
                    avail[i] = 256;
            break;
        case 23:
            ((nvenc_h264_slice_control_s *)(s.setup + pc->slice_control_offset))->num_mb++;
            break;
        case 24:
            p->input_cfg.block_height = 1;
            break;
        case 25:
            p->input_cfg.sfc_pitch = p->input_cfg.sfc_pitch_chroma = 8192;
            break;
        case 26:
            p->input_cfg.block_height = 32;
            break;
        }
        const char *why = tegra_nvenc_check(&s, avail);
        EXPECT(why && !strcmp(why, cases[k].why), "%s: checker said \"%s\", expected \"%s\"", cases[k].name,
               why ? why : "accepted", cases[k].why);
    }
}

// The controller against a synthetic encoder: bytes = complexity * 2^((26 - QP) / 6), complexity jumping mid-way.
static void rateControl(void)
{
    RateControl rc;
    rc_init(&rc, 4e6, false, -1, 30, 1280, 720);
    double total = 0, late = 0;
    for (int i = 0; i < 600; i++) {
        const bool key = i % 60 == 0;
        const double complexity = (i < 300 ? 20000.0 : 60000.0) * (key ? 4 : 1);
        const double bytes = complexity * pow(2, (26.0 - rc_qp(&rc, key)) / 6);
        rc_update(&rc, bytes, key);
        total += bytes;
        if (i >= 450)
            late += bytes;
    }
    const double rate = total * 8 / 20, lateRate = late * 8 / 5;
    EXPECT(rate > 3.2e6 && rate < 4.8e6 && lateRate > 3.2e6 && lateRate < 4.8e6,
           "VBR 4 Mb/s came out at %.2f Mb/s overall, %.2f Mb/s after the jump", rate / 1e6, lateRate / 1e6);
    rc_init(&rc, 0, false, 0.8, 30, 1280, 720);
    EXPECT(rc_qp(&rc, false) == 22 && rc_qp(&rc, true) == 20, "constant quality 0.8 -> QP %d", rc_qp(&rc, false));
}

int main(void)
{
    gLog = os_log_create("com.brunostjohn.switch.ave", "host-test");
    sizes();
    driverControls();
    parameterSets();
    layouts();
    rateControl();
    // A class ID of our own, which the sessions name below: the Mac's own H.264 encoders would otherwise win.
    // No IsHardwareAccelerated: in a macOS VM VideoToolbox hides hardware-flagged encoders other than its
    // paravirtualized ones. The encode check reads the plug-in's own UsingHardwareAcceleratedVideoEncoder.
    const void *keys[] = {CFSTR("CMClassImplementationID")};
    const void *values[] = {CFSTR("com.brunostjohn.switch.ave-host")};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus registered = VTRegisterVideoEncoderWithInfo(kCMVideoCodecType_H264, info, createInstance);
    if (!registered)
        registered = VTRegisterVideoEncoderWithInfo(kCMVideoCodecType_HEVC, info, createInstance);
    CFRelease(info);
    if (registered) {
        printf("FAIL ave: VTRegisterVideoEncoderWithInfo %d\n", (int)registered);
        return 1;
    }
    const void *specKeys[] = {kVTVideoEncoderSpecification_EncoderID};
    const void *specValues[] = {CFSTR("com.brunostjohn.switch.ave-host")};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);

    // The CPU copy into the layout the kernel states (a kernel without surface input), VIC writing the surface into
    // BL2 (from the session's pool, and from rows off VIC's grid: VideoToolbox moves a frame session's onto the grid
    // the plug-in asked for, a tile session hands them over and the kernel bounces them), and a kernel refusing every
    // surface or every other one: those frames fail, none is copied.
    static const struct {
        uint32_t layout, chroma;
        bool surfaceInput;
        int refuse;
        bool ok, clientRows;         // whether every frame and tile is to come out; sources off VIC's grid
        const char *frames, *tiles;  // the routes each check must report
    } runs[] = {
        {TEGRA_NVENC_LAYOUT_PITCH, TEGRA_NVENC_CHROMA_OFFSET, false, 0, true, false,
         "inputs 0 by VIC 0 bounced 30 copied 0 refused", "inputs 0 by VIC 0 bounced 4 copied 0 refused"},
        {TEGRA_NVENC_LAYOUT_BL2, TEGRA_NVENC_CHROMA_METHOD, false, 0, true, false,
         "inputs 0 by VIC 0 bounced 30 copied 0 refused", "inputs 0 by VIC 0 bounced 4 copied 0 refused"},
        {TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, false, 0, true, false,
         "inputs 0 by VIC 0 bounced 30 copied 0 refused", "inputs 0 by VIC 0 bounced 4 copied 0 refused"},
        {TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, true, 0, true, false,
         "inputs 30 by VIC 0 bounced 0 copied 0 refused", "inputs 4 by VIC 0 bounced 0 copied 0 refused"},
        {TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, true, 0, true, true,
         "inputs 30 by VIC 0 bounced 0 copied 0 refused", "inputs 4 by VIC 3 bounced 0 copied 0 refused"},
        {TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, true, 1, false, false,
         "inputs 0 by VIC 0 bounced 0 copied 30 refused", "inputs 0 by VIC 0 bounced 0 copied 4 refused"},
        {TEGRA_NVENC_LAYOUT_TILED16, TEGRA_NVENC_CHROMA_OFFSET, true, 2, false, false,
         "inputs 15 by VIC 0 bounced 0 copied 15 refused", "inputs 2 by VIC 0 bounced 0 copied 2 refused"},
    };

    for (unsigned r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        gLayout = runs[r].layout;
        gChroma = runs[r].chroma;
        gSurfaceInput = runs[r].surfaceInput;
        gRefuseSurfaces = runs[r].refuse;
        encode_check_client_buffers = runs[r].clientRows;
        const int submits = gSubmits, refused = gRefused, vic = gVIC;
        char line[512];
        const bool ok = encode_check(spec, kVTH264EntropyMode_CAVLC, 99.0, line, sizeof(line));
        printf("layout %u chroma %u%s: %s (submits %d, refused %d%s%s)\n", gLayout, gChroma,
               gSurfaceInput ? (gRefuseSurfaces      ? " surfaces refused"
                                : runs[r].clientRows ? " surfaces off-grid"
                                                     : " surfaces")
                             : "",
               line, gSubmits - submits, gRefused - refused, gRefusal ? ": " : "", gRefusal ? gRefusal : "");
        EXPECT(ok == runs[r].ok && gRefused == refused && strstr(line, runs[r].frames) &&
                   (!runs[r].ok || gSubmits - submits == 30) &&
                   (gVIC - vic == 30 || !strstr(runs[r].frames, "inputs 30 by VIC")),
               "encode check, layout %u, surfaces %d refused %d", gLayout, gSurfaceInput, gRefuseSurfaces);
    }
    // The Wait of one frame answers its input's failure (TegraNVENCInputs.h: VIC's input job, or a native picture's
    // producer, was cancelled and its syncpoint released): that frame fails, the next is coded as an IDR and the
    // session goes on. The check decodes only a complete session, so nothing is decoded here.
    gLayout = TEGRA_NVENC_LAYOUT_TILED16;
    gChroma = TEGRA_NVENC_CHROMA_OFFSET;
    gSurfaceInput = true;
    gRefuseSurfaces = 0;
    encode_check_client_buffers = false;
    gFailWait = (uint32_t)gSubmits + 6;
    char failedLine[512];
    encode_check(spec, kVTH264EntropyMode_CAVLC, 99.0, failedLine, sizeof(failedLine));
    gFailWait = 0;
    printf("a frame whose input's producer failed: %s\n", failedLine);
    EXPECT(strstr(failedLine, "29/30 frames encoded (1 errors)") && strstr(failedLine, "sync samples 0 6 "),
           "a failed Wait fails its frame, the next is an IDR and the session goes on");
    // AllowFrameReordering unset (true), MaxFrameDelayCount 1, AllowFrameReordering false; held frames coded later
    // from their own surfaces by the shipped route (the clip's CoreVideo rows are off VIC's grid: bounced).
    static const int kReorder[3][3] = {{-1, -1, 2}, {-1, 1, 1}, {0, -1, 0}};
    gLayout = TEGRA_NVENC_LAYOUT_TILED16;
    gChroma = TEGRA_NVENC_CHROMA_OFFSET;
    gSurfaceInput = true;
    gRefuseSurfaces = 0;
    encode_check_client_buffers = false;
    const int vicBefore = gVIC;
    for (unsigned r = 0; r < 3; r++) {
        const int refused = gRefused;
        char line[512];
        const bool ok = reorder_check(spec, kReorder[r][0], kReorder[r][1], kReorder[r][2], line, sizeof(line));
        printf("reordering %d, frame delay %d: %s (refused %d%s%s)\n", kReorder[r][0], kReorder[r][1], line,
               gRefused - refused, gRefusal ? ": " : "", gRefusal ? gRefusal : "");
        EXPECT(ok && gRefused == refused, "reorder check, %d B pictures", kReorder[r][2]);
    }
    gRefuseB = true;
    char refusalLine[256];
    const bool refusalOk = reorder_refusal_check(spec, refusalLine, sizeof(refusalLine));
    gRefuseB = false;
    printf("B pictures refused: %s\n", refusalLine);
    EXPECT(refusalOk, "a refused B picture is coded as an I picture, B pictures turn off and the session goes on");
    EXPECT(gVIC - vicBefore >= 90, "reordering sessions' pictures written by VIC, %d", gVIC - vicBefore);
    char probeLine[256];
    const bool probed = probe_check(spec, probeLine, sizeof(probeLine));
    printf("%s\n", probeLine);
    EXPECT(probed, "SwitchEncoderLayoutProbe");
    CFRelease(spec);
    for (unsigned r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        gLayout = runs[r].layout;
        gChroma = runs[r].chroma;
        gSurfaceInput = runs[r].surfaceInput;
        gRefuseSurfaces = runs[r].refuse;
        tile_check_row_alignment = runs[r].clientRows ? 64 : 256;
        const int refused = gRefused;
        char line[512];
        const bool ok = tile_check(CFSTR("com.brunostjohn.switch.ave-host"), line, sizeof(line));
        printf("tiles, layout %u chroma %u%s: %s (refused %d%s%s)\n", gLayout, gChroma,
               gSurfaceInput ? (gRefuseSurfaces      ? " surfaces refused"
                                : runs[r].clientRows ? " surfaces off-grid"
                                                     : " surfaces")
                             : "",
               line, gRefused - refused, gRefusal ? ": " : "", gRefusal ? gRefusal : "");
        EXPECT(ok == runs[r].ok && gRefused == refused && strstr(line, runs[r].tiles),
               "tile check, layout %u, surfaces %d refused %d", gLayout, gSurfaceInput, gRefuseSurfaces);
    }
    gSurfaceInput = false;
    gRefuseSurfaces = 0;
    encode_check_client_buffers = false;
    tile_check_row_alignment = 256;
    refusals();
    printf("I_PCM macroblocks %d, skipped %d, IDR %d, P %d, B %d, opens %d\n", gPcm, gSkipped, gIdr, gP, gB, gOpens);
    EXPECT(gSkipped > 0 && gP > 0 && gB > 0 && gIdr >= 12, "the clip exercised P_Skip, B pictures and key frames");
    printf("%s ave: plug-in on VideoToolbox over a lossless fake NVENC (%d failures)\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
