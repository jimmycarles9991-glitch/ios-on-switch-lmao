// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for HEVCEncSyntax.c: the VPS, SPS, PPS and slice segment headers it writes, parsed back by AVD's
// HEVCSyntax.c (the decoder's own parser) field by field, and the three parameter sets taken by CoreMedia
// (CMVideoFormatDescriptionCreateFromHEVCParameterSets), whose dimensions must be the displayed size. Each config
// also gets the level Table A.8 gives it. Prints "PASS hevcenc" when all hold (hevcenc_host_test).
#include <CoreMedia/CoreMedia.h>
#include <stdio.h>
#include <string.h>
#include "../../SwitchVideoDecoders/AVD/HEVCSyntax.h"
#include "../AVE/HEVCEncSyntax.h"

static int gFailures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            gFailures++;                  \
        }                                 \
    } while (0)

typedef struct {
    const char *name;
    HEVCEncConfig config;
    uint8_t level;
} Case;

static void checkSps(const Case *t, const HEVCSPS *s)
{
    const HEVCEncConfig *c = &t->config;
    CHECK(s->profile_idc == 1 && s->level_idc == t->level && s->chroma_format_idc == 1 && s->bit_depth == 8,
          "%s: profile %u level %u chroma %u depth %u", t->name, s->profile_idc, s->level_idc, s->chroma_format_idc,
          s->bit_depth);
    CHECK(s->width - s->crop_left - s->crop_right == c->width &&
              s->height - s->crop_top - s->crop_bottom == c->height && s->width % 8 == 0 && s->height % 8 == 0,
          "%s: coded %ux%u cropped to %ux%u", t->name, s->width, s->height, s->width - s->crop_left - s->crop_right,
          s->height - s->crop_top - s->crop_bottom);
    CHECK(s->log2_min_cb == c->log2_min_cb && s->log2_min_cb + s->log2_diff_max_min_cb == c->log2_ctb &&
              s->log2_min_tb == c->log2_min_tb && s->log2_min_tb + s->log2_diff_max_min_tb == c->log2_max_tb &&
              s->max_th_depth_inter == c->max_tr_depth_inter && s->max_th_depth_intra == c->max_tr_depth_intra,
          "%s: coding tree sizes", t->name);
    CHECK(s->amp == c->amp && s->sao == c->sao && s->temporal_mvp == c->temporal_mvp &&
              s->strong_intra_smoothing == c->strong_intra_smoothing && !s->pcm && !s->scaling_list_enabled &&
              !s->long_term_present,
          "%s: tools", t->name);
    CHECK(s->log2_max_poc_lsb == HEVCENC_LOG2_MAX_POC_LSB && s->max_dec_pic_buffering == 2 && s->max_num_reorder == 0,
          "%s: POC LSB %u bits, DPB %u, reorder %u", t->name, s->log2_max_poc_lsb, s->max_dec_pic_buffering,
          s->max_num_reorder);
    CHECK(s->num_short_term_rps == 1 && s->st_rps[0].num_negative == 1 && s->st_rps[0].num_positive == 0 &&
              s->st_rps[0].delta[0][0] == -1 && s->st_rps[0].used[0][0],
          "%s: the reference picture set", t->name);
    CHECK(s->full_range == c->full_range && s->colour_description == c->colour_description &&
              (!c->colour_description ||
               (s->colour_primaries == c->colour_primaries && s->transfer == c->transfer && s->matrix == c->matrix)),
          "%s: video signal type", t->name);
}

static void checkSlice(const Case *t, const HEVCParams *params, const HEVCEncPicture *p)
{
    uint8_t rbsp[64], nal[80];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    hevcenc_slice_header(&t->config, p, &b);
    h264bits_trailing(&b);  // byte_alignment(): the same bits
    const size_t size = h264bits_size(&b);
    nal[0] = (uint8_t)(hevcenc_nal_type(p) << 1);
    nal[1] = 1;
    memcpy(nal + 2, rbsp, size);
    HEVCSliceHeader sh;
    const int ret = hevc_parse_slice_header(params, nal, size + 2, &sh);
    CHECK(ret == 0 && sh.first_slice && sh.pps_id == 0 && sh.nal_type == hevcenc_nal_type(p) &&
              sh.slice_type == (p->idr ? HEVC_SLICE_I : HEVC_SLICE_P),
          "%s: %s slice header (parse %d, type %u, slice %u)", t->name, p->idr ? "IDR" : "P", ret, sh.nal_type,
          sh.slice_type);
    if (!p->idr)
        CHECK(sh.poc_lsb == p->poc && sh.st_rps_from_sps && sh.st_rps.num_negative == 1 &&
                  sh.st_rps.delta[0][0] == -1 && !sh.num_lt,
              "%s: P slice POC %u and reference set", t->name, sh.poc_lsb);
}

static void checkCase(const Case *t)
{
    HEVCEncConfig c = t->config;
    CHECK(hevcenc_configure(&c) == 0 && c.level_idc == t->level, "%s: configure, level %u (want %u)", t->name,
          c.level_idc, t->level);
    Case configured = *t;
    configured.config = c;
    uint8_t vps[128], sps[256], pps[64];
    const size_t vpsSize = hevcenc_write_vps(&c, vps, sizeof(vps));
    const size_t spsSize = hevcenc_write_sps(&c, sps, sizeof(sps));
    const size_t ppsSize = hevcenc_write_pps(&c, pps, sizeof(pps));
    CHECK(vpsSize && spsSize && ppsSize && (vps[0] >> 1) == HEVC_NAL_VPS && (sps[0] >> 1) == HEVC_NAL_SPS &&
              (pps[0] >> 1) == HEVC_NAL_PPS,
          "%s: parameter sets written", t->name);
    static HEVCParams params;
    memset(&params, 0, sizeof(params));
    CHECK(hevc_parse_sps(&params, sps, spsSize) == 0 && hevc_parse_pps(&params, pps, ppsSize) == 0,
          "%s: AVD's parser refused the SPS or PPS", t->name);
    checkSps(&configured, &params.sps[0]);
    const HEVCPPS *q = &params.pps[0];
    CHECK(q->valid && !q->tiles && !q->cu_qp_delta && q->init_qp_minus26 == 0 && !q->dependent_slice_segments &&
              q->num_ref_idx_l0_default == 1 && !q->lists_modification_present && !q->range_extension,
          "%s: PPS fields", t->name);
    const HEVCEncPicture idr = {.idr = true, .qp = 30}, p = {.poc = 1, .qp = 28}, far = {.poc = 70000, .qp = 24};
    checkSlice(&configured, &params, &idr);
    checkSlice(&configured, &params, &p);
    HEVCEncPicture wrapped = far;
    wrapped.poc = far.poc & ((1u << HEVCENC_LOG2_MAX_POC_LSB) - 1);
    checkSlice(&configured, &params, &wrapped);

    const uint8_t *sets[3] = {vps, sps, pps};
    const size_t sizes[3] = {vpsSize, spsSize, ppsSize};
    CMFormatDescriptionRef format = NULL;
    const OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, sets, sizes, 4, NULL, &format);
    const CMVideoDimensions d = format ? CMVideoFormatDescriptionGetDimensions(format) : (CMVideoDimensions){0, 0};
    CHECK(!status && (uint32_t)d.width == c.width && (uint32_t)d.height == c.height, "%s: CoreMedia %d, %dx%d", t->name,
          (int)status, d.width, d.height);
    if (format)
        CFRelease(format);
    uint8_t hvcc[512];
    const size_t hvccSize = hevcenc_write_hvcc(&c, hvcc, sizeof(hvcc));
    CHECK(hvccSize > 23 && hvcc[0] == 1 && hvcc[12] == c.level_idc && hvcc[22] == 3 && (hvcc[21] & 3) == 3,
          "%s: hvcC (%zu bytes)", t->name, hvccSize);
    printf("hevcenc: %s %ux%u level %u.%u: VPS %zu, SPS %zu, PPS %zu bytes, hvcC %zu\n", t->name, c.width, c.height,
           c.level_idc / 30, c.level_idc % 30 / 3, vpsSize, spsSize, ppsSize, hvccSize);
}

int main(void)
{
    static const Case kCases[] = {
        {"1080p60", {.width = 1920, .height = 1080, .fps_num = 60, .fps_den = 1}, 123},
        {"720p30 tools",
         {.width = 1280,
          .height = 720,
          .fps_num = 30,
          .fps_den = 1,
          .amp = true,
          .sao = true,
          .temporal_mvp = true,
          .strong_intra_smoothing = true,
          .full_range = true,
          .colour_description = true,
          .colour_primaries = 1,
          .transfer = 1,
          .matrix = 1,
          .max_tr_depth_inter = 1,
          .max_tr_depth_intra = 1},
         93},
        {"2160p30", {.width = 3840, .height = 2160, .fps_num = 30, .fps_den = 1}, 150},
        {"642x362 ctb16", {.width = 642, .height = 362, .log2_ctb = 4}, 90},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
        checkCase(&kCases[i]);
    HEVCEncConfig bad = {.width = 641, .height = 360};
    CHECK(hevcenc_configure(&bad) == -1, "an odd width was taken");
    if (gFailures) {
        printf("FAIL hevcenc: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS hevcenc\n");
    return 0;
}
