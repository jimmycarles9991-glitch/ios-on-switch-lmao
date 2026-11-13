// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for tegra_nvenc_direct_plan (TegraNVENCDirectPlan.h): the plug-in's own H.264 and H.265 submits, their
// input pointed at a native NVDEC picture the way TegraNVENCDirect.cpp does it, then the kernel's checker over the
// picture's backing. NVDEC's pictures (TegraVIC/SwitchNativePicture.h): 2-GOB blocks, rows a multiple of 32, chroma
// after pitch x rows. 720p and 1080p H.264 and 720p H.265 (coded 736 rows) are taken; a picture of other blocks, format
// or crop, and one with fewer rows than the coded picture, are not; and what the job does about each producer still
// writing the picture (tegra_nvenc_direct_producer). Prints "PASS nvencdirect" when all hold.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../../TegraNVENC/TegraNVENCCheck.h"
#include "../../TegraNVENC/TegraNVENCDirectPlan.h"
#include "../AVE/H264NVENC.h"
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

enum {
    NATIVE = 9,
    NV12_VIDEO = 0x34323076,
    NV12_FULL = 0x34323066,
};

static const H264EncHandles kHandles = {.input = 1, .recon = 2, .ref = 3, .bitstream = 4, .aux = 5, .mepred = {6, 7}};

typedef struct {
    struct switch_gpu_backing_layout layout;
    uint64_t bytes;
    uint32_t cropX, cropY, width, height;
} Native;

// A picture as NVDEC writes it: `rows` of `pitch` bytes of luma, then half as many of interleaved chroma.
static Native nvdecPicture(uint32_t width, uint32_t height, uint32_t pitch, uint32_t rows, uint32_t log2)
{
    Native n = {.cropX = 0, .cropY = 0, .width = width, .height = height};
    const uint64_t luma = (uint64_t)pitch * rows;
    n.layout.format = NV12_VIDEO;
    n.layout.plane_count = 2;
    n.layout.planes[0] =
        (struct switch_gpu_backing_plane){0, luma, pitch, rows, pitch, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, log2};
    n.layout.planes[1] = (struct switch_gpu_backing_plane){
        luma, luma / 2, pitch / 2, rows / 2, pitch, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, log2};
    n.bytes = luma + luma / 2;
    return n;
}

static const char *patchAndCheck(TegraNVENCSubmit *s, const H264EncLayout *l, const Native *n)
{
    s->source = (TegraNVENCSource){.surface_id = 1, .width = n->width, .height = n->height, .format = NV12_VIDEO};
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    TegraNVENCDirectPlan plan;
    const char *why =
        tegra_nvenc_direct_plan(&cfg, &s->source, &n->layout, n->cropX, n->cropY, n->width, n->height, &plan);
    if (why)
        return why;
    tegra_nvenc_direct_config(&plan, &cfg);
    memcpy(s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    for (uint32_t i = 0; i < s->entry_count; i++) {
        TegraNVENCEntry *e = &s->entries[i];
        if (e->method == NVC5B7_SET_IN_CUR_PIC)
            *e = (TegraNVENCEntry){e->method, TEGRA_NVENC_ENTRY_RELOC, NATIVE, (uint32_t)plan.luma};
        else if (e->method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U)
            *e = (TegraNVENCEntry){e->method, TEGRA_NVENC_ENTRY_RELOC, NATIVE, (uint32_t)plan.chroma};
    }
    const uint64_t sizes[10] = {0,
                                l->input.bytes,
                                l->recon.bytes,
                                l->recon.bytes,
                                l->bitstream_bytes,
                                l->aux_bytes,
                                l->mepred_bytes,
                                l->mepred_bytes,
                                0,
                                n->bytes};
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
            avail[i] = sizes[s->entries[i].value] - s->entries[i].offset;
    return tegra_nvenc_check(s, avail);
}

static const char *h264(uint32_t width, uint32_t height, const Native *n)
{
    H264EncConfig c = {.width = width,
                       .height = height,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .fps_num = 30,
                       .fps_den = 1,
                       .input_layout = TEGRA_NVENC_LAYOUT_BL2};
    if (h264enc_configure(&c))
        return "configure";
    H264EncLayout l;
    h264enc_layout(&c, &l);
    const H264EncPicture idr = {.type = H264ENC_IDR, .qp = 26};
    TegraNVENCSubmit s;
    h264enc_fill_submit(&c, &l, &idr, &kHandles, &s);
    return patchAndCheck(&s, &l, n);
}

static const char *h265(uint32_t width, uint32_t height, const Native *n)
{
    HEVCEncConfig c = {.width = width, .height = height, .fps_num = 30, .fps_den = 1};
    if (hevcnvenc_configure(&c))
        return "configure";
    H264EncLayout l;
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_BL2, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 26};
    TegraNVENCSubmit s;
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_BL2, TEGRA_NVENC_CHROMA_OFFSET, &l, &idr, &kHandles, &s);
    return patchAndCheck(&s, &l, n);
}

// The decoder's picture is still being written when NVENC's job is queued: NVDEC's and VIC's writes become waits on
// their syncpoints in the job's stream; a GPU producer sends the picture through VIC; a failed one fails the frame.
static void producers(void)
{
    struct switch_gpu_backing_token nvdec = {0x100000005ull, 1, SWITCH_GPU_ENGINE_NVDEC, 21};
    CHECK(tegra_nvenc_direct_producer(&nvdec, 0) == 1, "NVDEC still writing is waited for");
    CHECK(tegra_nvenc_direct_producer(&nvdec, 1) == 0, "NVDEC done needs nothing");
    CHECK(tegra_nvenc_direct_producer(&nvdec, -5) == -5, "NVDEC failed fails the frame");
    struct switch_gpu_backing_token vic = {9, 1, SWITCH_GPU_ENGINE_VIC, 22};
    CHECK(tegra_nvenc_direct_producer(&vic, 0) == 1, "VIC still writing is waited for");
    struct switch_gpu_backing_token own = {7, 1, SWITCH_GPU_ENGINE_NVENC, 23};
    CHECK(tegra_nvenc_direct_producer(&own, 0) == 0, "an earlier NVENC job is ordered on NVENC's channel");
    struct switch_gpu_backing_token gpu = {3, 2, SWITCH_GPU_ENGINE_GM20B, 5};
    CHECK(tegra_nvenc_direct_producer(&gpu, 0) == -11, "a GPU producer goes through VIC");
    CHECK(tegra_nvenc_direct_producer(&gpu, 1) == 0, "a GPU producer done needs nothing");
    struct switch_gpu_backing_token display = {3, 1, SWITCH_GPU_ENGINE_DISPLAY, 24};
    CHECK(tegra_nvenc_direct_producer(&display, 0) == -11, "another engine goes through VIC");
    struct switch_gpu_backing_token wide = {3, 1, SWITCH_GPU_ENGINE_NVDEC, 0x100};
    CHECK(tegra_nvenc_direct_producer(&wide, 0) == -11, "a syncpoint past host1x's 0xff goes through VIC");
}

int main(void)
{
    const Native p720 = nvdecPicture(1280, 720, 1280, 736, 1), p1080 = nvdecPicture(1920, 1080, 1920, 1088, 1);
    const char *why = h264(1280, 720, &p720);
    CHECK(!why, "H.264 720p from NVDEC's picture: %s", why);
    why = h264(1920, 1080, &p1080);
    CHECK(!why, "H.264 1080p from NVDEC's picture: %s", why);
    why = h265(1280, 720, &p720);
    CHECK(!why, "H.265 720p (736 coded rows) from NVDEC's picture: %s", why);

    Native wide = nvdecPicture(1280, 720, 1536, 736, 1);
    why = h264(1280, 720, &wide);
    CHECK(!why, "a pitch wider than the picture: %s", why);
    const Native gob1 = nvdecPicture(1280, 720, 1280, 736, 0);
    why = h264(1280, 720, &gob1);
    CHECK(why && !strcmp(why, "native block height NVENC has no code for"), "one-GOB blocks: %s", why ? why : "taken");
    Native full = p720;
    full.layout.format = NV12_FULL;
    why = h264(1280, 720, &full);
    CHECK(why && !strcmp(why, "native picture of another format"), "a full-range picture into video range: %s",
          why ? why : "taken");
    Native cropped = p720;
    cropped.cropY = 2;
    why = h264(1280, 720, &cropped);
    CHECK(why && !strcmp(why, "native picture cropped, or a region of it"), "a cropped picture: %s",
          why ? why : "taken");
    const Native shortRows = nvdecPicture(1280, 720, 1280, 720, 1);
    why = h265(1280, 720, &shortRows);
    CHECK(why && !strcmp(why, "surface too small"), "a picture of 720 rows for H.265's 736: %s", why ? why : "taken");
    Native oddPitch = nvdecPicture(1280, 720, 1312, 736, 1);
    why = h264(1280, 720, &oddPitch);
    CHECK(why && !strcmp(why, "native pitch NVENC does not address"), "a 1312-byte pitch: %s", why ? why : "taken");
    producers();
    printf("%s nvencdirect (%d failures)\n", gFailures ? "FAIL" : "PASS", gFailures);
    return gFailures ? 1 : 0;
}
