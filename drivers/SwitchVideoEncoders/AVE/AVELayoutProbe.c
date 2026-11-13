// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEncoderLayoutProbe (debug builds, H.264 frame sessions that do not reorder; HW Test encode.nvenc_input_layouts):
// the session's last picture coded again as one IDR from each input layout NVENC 5.0 might read, each in a buffer of
// its own written by the CPU (and two written by VIC: BL2, the route the plug-in ships, and BL16), returned as {layout:
// the IDR's AVCC sample} or {layout: the failure} for the test to decode back against its picture. The firmware gives
// block_height 0 and 2 the same code (TegraNVENC/nvidia/SOURCE), so exactly one of "pitch" and "bl2" can come back
// right, and NVIDIA's driver says "bl2". The probe leaves the session's next picture an IDR, since its pictures
// overwrote the reconstructions.
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdlib.h>
#include <string.h>
#include "AVEEncoder.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop

enum {
    kProbeQP = 10,  // fine enough that a picture read in the wrong layout cannot pass the decode-back check
};

typedef struct {
    const char *name;
    uint32_t layout;
    bool vic;
} ProbeCase;

static const ProbeCase kCases[] = {
    {"tiled16", TEGRA_NVENC_LAYOUT_TILED16, false}, {"pitch", TEGRA_NVENC_LAYOUT_PITCH, false},
    {"bl2", TEGRA_NVENC_LAYOUT_BL2, false},         {"bl4", TEGRA_NVENC_LAYOUT_BL4, false},
    {"bl8", TEGRA_NVENC_LAYOUT_BL8, false},         {"bl16", TEGRA_NVENC_LAYOUT_BL16, false},
    {"vic_bl2", TEGRA_NVENC_LAYOUT_BL2, true},      {"vic_bl16", TEGRA_NVENC_LAYOUT_BL16, true},
};

static CFTypeRef failure(int32_t code)
{
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &code);
}

static bool put(H264EncConfig *c, const H264EncSurface *s, uint8_t *va, CVPixelBufferRef image)
{
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly))
        return false;
    h264enc_put_region(c->input_layout, s, va, CVPixelBufferGetBaseAddressOfPlane(image, 0),
                       CVPixelBufferGetBytesPerRowOfPlane(image, 0), CVPixelBufferGetBaseAddressOfPlane(image, 1),
                       CVPixelBufferGetBytesPerRowOfPlane(image, 1), c->width, c->height, h264enc_width_mbs(c) * 16,
                       h264enc_height_mbs(c) * 16);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    return true;
}

// One IDR from `input` (a buffer laid out as c->input_layout) into e->avcc: its size, or the negative failure.
static int32_t code(Encoder *e, const H264EncConfig *c, const H264EncSurface *s, uint32_t input, bool vic,
                    CVPixelBufferRef image)
{
    H264EncLayout layout = e->layout;
    layout.input = *s;
    H264EncHandles handles = e->handles;
    handles.input = input;
    const H264EncPicture idr = {.type = H264ENC_IDR, .qp = kProbeQP, .picture_index = e->pictureIndex++};
    TegraNVENCSubmit *submit = malloc(sizeof(*submit));
    if (!submit)
        return -1;
    h264enc_fill_submit(c, &layout, &idr, &handles, submit);
    if (vic) {
        submit->clean_count = 0;
        submit->source = (TegraNVENCSource){
            .surface_id = IOSurfaceGetID(CVPixelBufferGetIOSurface(image)),
            .width = c->width,
            .height = c->height,
            .format = c->full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                    : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        };
    }
    memset(e->auxVA, 0, sizeof(nvenc_pic_stat_s));
    submit->clean[submit->clean_count++] = (TegraNVENCRange){e->handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
    TegraNVENCWait wait = {.timeout_us = 1000000, .invalidate_count = 1};
    wait.invalidate[0] = (TegraNVENCRange){e->handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
    const int submitted = nvenc_submit(e->client, submit, &wait.fence, NULL);
    free(submit);
    if (submitted)
        return -2;
    if (nvenc_wait(e->client, &wait))
        return -3;
    const nvenc_pic_stat_s *stat = (const nvenc_pic_stat_s *)e->auxVA;
    const uint32_t produced = (stat->total_bit_count + 7) / 8;
    if (stat->ucode_error_status || !produced || produced > e->layout.bitstream_bytes)
        return -4;
    TegraNVENCWait bits = {.fence = wait.fence, .timeout_us = 1000000, .invalidate_count = 1};
    bits.invalidate[0] = (TegraNVENCRange){e->handles.bitstream, 0, produced, 0};
    if (nvenc_wait(e->client, &bits))
        return -5;
    const size_t size = h264enc_annexb_to_avcc(e->bitstreamVA, produced, e->avcc, e->avccCap);
    return size ? (int32_t)size : -6;
}

CFDictionaryRef ave_copy_layout_probe(Encoder *e)
{
    CVPixelBufferRef image = e->probeSource;
    if (!e->configured || e->codec != kCMVideoCodecType_H264 || e->config.reorder || !image)
        return NULL;
    CFMutableDictionaryRef result = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const ProbeCase *k = &kCases[i];
        CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, k->name, kCFStringEncodingUTF8);
        H264EncConfig c = e->config;
        c.input_layout = k->layout;
        H264EncSurface s;
        h264enc_surface(k->layout, h264enc_width_mbs(&c) * 16, h264enc_height_mbs(&c) * 16, &s);
        uint32_t handle = 0;
        void *va = NULL;
        int32_t size = -7;
        if ((k->vic && !(e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT)) || !CVPixelBufferGetIOSurface(image)) {
            size = -8;
        } else if (!nvenc_alloc(e->client, s.bytes, &handle, &va)) {
            size = k->vic || put(&c, &s, va, image) ? code(e, &c, &s, handle, k->vic, image) : -9;
            nvenc_free(e->client, handle);
        }
        CFTypeRef value = size > 0 ? CFDataCreate(kCFAllocatorDefault, e->avcc, size) : failure(size);
        CFDictionarySetValue(result, name, value);
        CFRelease(value);
        CFRelease(name);
    }
    e->keyPending = true;
    return result;
}
#endif
