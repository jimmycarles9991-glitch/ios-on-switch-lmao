// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for H264NVENC.c's setups at sizes that are not whole macroblocks (1080p, the HW Test's): every surface
// config carries the coded size, the kernel's checker takes the IDR and P submits, and the display size is refused
// (NVENC 5.0's H.264 app fetches one macroblock row too few from it; TegraNVENC/nvidia/SOURCE). Both pictures name
// reference slot 0, which the checker bounds IDR or not. Prints "PASS h264nvenc" when all hold.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
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

static const H264EncHandles kHandles = {1, 2, 3, 4, 5, {6, 7}};

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

int main(void)
{
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
