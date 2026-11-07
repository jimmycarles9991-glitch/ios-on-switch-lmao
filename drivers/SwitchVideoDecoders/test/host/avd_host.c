// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVD.videodecoder's VideoToolbox side, on a Mac: the plug-in itself (AVDDecoder.c, included for its
// factory) over a fake TegraNVDEC client, registered in this process and driven by the clip check (ClipCheck.c)
// through VideoToolbox, which checks the plug-in's class layout and session calls against a real VideoToolbox.
//   engine: the fake client hands out buffers and checks every submit with the kernel's checker; its engine writes
//           nothing, so pictures come out black: the test wants all 30 out of a session that says hardware.
//   full:   the fake client refuses every buffer, so the instance runs Apple's software decoder: the test wants 30/30
//           frames matching x264 from a session that says software.
//   vp9:    VP9Clip.h as 'vp09' (kVTVideoDecoderSpecification_AllowVP9Decode) on AVD's private check registration
//           (registerCheckDecoder: not flagged hardware, named com.brunostjohn.switch.avd.check.vp09, as the device's boot check
//           uses it) and the fake engine: every superframe and frame through VP9NVDEC and the kernel's checker, all 16
//           shown frames out of a session that says hardware.
//   present: the engine session again with TegraNVDECPresent on: every picture handed to the fake VIC, whose pool
//           buffer must be one VIC addresses (rows and the chroma plane on 256 bytes) and which copies it as VIC would;
//           no wait invalidates the surface, and the CPU path's pictures come out.
//   gate:   the private registration outside an open check (gCheckOpen false): no instance, so no session by its name.
//   tiles:  clip A's first picture as four tiles of one canvas through VTTileDecompressionSession (AVDTiles.c): the
//           canvas made from the tile requirements, each tile presented at its offset, each answered; with VIC off the
//           tile session is refused and VideoToolbox finds no other decoder by that name.
//   hevc:   HEVCClip.h's Main clips A and V and Main 10 clip T as 'hvc1' on the private check registration (named
//           com.brunostjohn.switch.avd.check.hvc1) and the fake engine: every picture through HEVCNVDEC and the kernel's checker,
//           all out of a hardware session; then with every buffer refused, Apple's software HEVC decoder in it,
//           matching FFmpeg.
// Prints "PASS avd ..." when all hold (avd_host_test).
#include "../../AVD/AVDDecoder.c"
#include <stdio.h>
#include "../HEVCClip.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"

struct NVDECClient {
    uint8_t *va[64];
    uint64_t size[64];
};

static bool gFull, gPresentOn;
static int gOpens, gSubmits, gRefused, gPresents, gPresentBad, gWaitRanges;
static uint32_t gLastDstX, gLastDstY;

bool nvdec_service_present(void)
{
    return true;
}

NVDECClient *nvdec_open(int *error)
{
    (void)error;
    gOpens++;
    return calloc(1, sizeof(NVDECClient));
}

void nvdec_close(NVDECClient *c)
{
    if (!c)
        return;
    for (int i = 0; i < 64; i++)
        free(c->va[i]);
    free(c);
}

uint32_t nvdec_codecs(const NVDECClient *c)
{
    (void)c;
    return 1u << TEGRA_NVDEC_CODEC_H264 | 1u << TEGRA_NVDEC_CODEC_VP9 | 1u << TEGRA_NVDEC_CODEC_HEVC |
           TEGRA_NVDEC_HEVC_MAIN10;
}

uint32_t nvdec_codecs_now(NVDECClient *c)
{
    return nvdec_codecs(c);
}

bool nvdec_can_present(const NVDECClient *c)
{
    (void)c;
    return gPresentOn;
}

// The fake VIC: TegraNVDECUserClient's geometry checks and TegraVICConfig.h's address rules for the destination, then
// the copy (BlockLinear.c, as VIC's pass-through would leave it).
int nvdec_present(NVDECClient *c, const TegraNVDECPresent *p)
{
    gPresents++;
    const uint64_t luma = (uint64_t)p->pitch * p->rows;
    IOSurfaceRef s = IOSurfaceLookup(p->surface_id);
    bool ok = p->handle < 64 && c->va[p->handle] && p->pitch && !(p->pitch % 64) && p->rows && !(p->rows % 32) &&
              p->block_height_log2 == 1 && !(p->chroma_offset % 256) && p->chroma_offset >= luma &&
              p->chroma_offset + luma / 2 <= c->size[p->handle] && s;
    if (ok) {
        const uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(s);
        ok = !(IOSurfaceGetBytesPerRowOfPlane(s, 0) % 256) && !(IOSurfaceGetBytesPerRowOfPlane(s, 1) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 0) - base) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 1) - base) % 256) && !(p->dst_x % 256) &&
             !(p->dst_y & 1) && p->dst_x + p->width <= IOSurfaceGetWidth(s) &&
             p->dst_y + p->height <= IOSurfaceGetHeight(s) && !((p->width | p->height) & 1);
    }
    if (ok) {
        IOSurfaceLock(s, 0, NULL);
        const uint8_t *picture = c->va[p->handle];
        const size_t lumaPitch = IOSurfaceGetBytesPerRowOfPlane(s, 0),
                     chromaPitch = IOSurfaceGetBytesPerRowOfPlane(s, 1);
        block_linear_copy((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0) + p->dst_y * lumaPitch + p->dst_x, lumaPitch,
                          picture, p->pitch, p->crop_x, p->crop_y, p->width, p->height);
        block_linear_copy((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1) + p->dst_y / 2 * chromaPitch + p->dst_x,
                          chromaPitch, picture + p->chroma_offset, p->pitch, p->crop_x, p->crop_y / 2, p->width,
                          p->height / 2);
        IOSurfaceUnlock(s, 0, NULL);
        gLastDstX = p->dst_x;
        gLastDstY = p->dst_y;
    } else {
        gPresentBad++;
    }
    if (s)
        CFRelease(s);
    return ok ? 0 : kIOReturnUnsupported;
}

uint32_t nvdec_check_claim(NVDECClient *c, bool *claimed)
{
    (void)c;
    *claimed = false;
    return TEGRA_NVDEC_CHECK_PASSED;
}

int nvdec_check_report(NVDECClient *c, bool vp9Ok, bool hevcOk, bool hevc10Ok)
{
    (void)c, (void)vp9Ok, (void)hevcOk, (void)hevc10Ok;
    return 0;
}

uint32_t nvdec_check_wait(NVDECClient *c)
{
    (void)c;
    return TEGRA_NVDEC_CHECK_PASSED;
}

int nvdec_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    NVDECClient *c = ctx;
    if (gFull)
        return -1;
    for (uint32_t h = 1; h < 64; h++) {
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

void nvdec_free(void *ctx, uint32_t handle)
{
    NVDECClient *c = ctx;
    free(c->va[handle]);
    c->va[handle] = NULL;
}

int nvdec_submit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    NVDECClient *c = ctx;
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (e->value >= 64 || !c->va[e->value] || e->offset > c->size[e->value])
            return -1;
        avail[i] = c->size[e->value] - e->offset;
    }
    if (tegra_nvdec_check(s, avail)) {
        gRefused++;
        return -1;
    }
    *fence = (uint32_t)++gSubmits;
    return 0;
}

int nvdec_wait(void *ctx, const TegraNVDECWait *w)
{
    (void)ctx;
    gWaitRanges += (int)w->invalidate_count;
    return 0;
}

typedef void (*TileOutput)(void *refcon, void *sourceRefCon, VTInt32Point tile, VTInt32Size size,
                           CVPixelBufferRef image, VTInt32Point offset, OSStatus status, VTDecodeInfoFlags info);
extern OSStatus VTTileDecompressionSessionCreate(CFAllocatorRef allocator, CMVideoFormatDescriptionRef format,
                                                 CFDictionaryRef specification, TileOutput output, void *refcon,
                                                 void **session);
extern OSStatus VTTileDecompressionSessionDecodeTile(void *session, CMSampleBufferRef sample, VTInt32Point tile,
                                                     VTInt32Size size, CVPixelBufferRef canvas, VTInt32Point offset,
                                                     VTDecodeFrameFlags flags, void *sourceRefCon,
                                                     VTDecodeInfoFlags *info);
extern void VTTileDecompressionSessionInvalidate(void *session);

typedef struct {
    int answered, good;
    uint32_t offsets[4][2];
} TileAnswers;

static void tileOutput(void *refcon, void *sourceRefCon, VTInt32Point tile, VTInt32Size size, CVPixelBufferRef image,
                       VTInt32Point offset, OSStatus status, VTDecodeInfoFlags info)
{
    (void)tile, (void)size, (void)image, (void)info;
    TileAnswers *a = refcon;
    const int i = (int)(intptr_t)sourceRefCon;
    if (i >= 0 && i < 4) {
        a->offsets[i][0] = (uint32_t)offset.x;
        a->offsets[i][1] = (uint32_t)offset.y;
    }
    a->answered++;
    a->good += !status;
}

// Clip A's parameter sets and its first picture as one length-prefixed sample.
static CMVideoFormatDescriptionRef firstPicture(uint8_t *sample, size_t *size)
{
    const HEVCClip *clip = &kHEVCClips[0];
    const uint8_t *sets[3] = {NULL, NULL, NULL};
    size_t setSizes[3] = {0, 0, 0}, used = 0;
    int pictures = 0;
    for (size_t i = 0; i + 3 <= clip->size && pictures < 2; i++) {
        if (clip->data[i] || clip->data[i + 1] || clip->data[i + 2] != 1)
            continue;
        const uint8_t *nal = clip->data + i + 3;
        size_t end = i + 3;
        while (end + 3 <= clip->size && (clip->data[end] || clip->data[end + 1] || clip->data[end + 2] != 1))
            end++;
        if (end + 3 > clip->size)
            end = clip->size;
        while (end > i + 3 && !clip->data[end - 1])
            end--;
        const size_t length = (size_t)(clip->data + end - nal);
        const int type = (nal[0] >> 1) & 0x3f;
        if (type >= 32 && type <= 34 && !sets[type - 32]) {
            sets[type - 32] = nal, setSizes[type - 32] = length;
        } else if (type < 32) {
            pictures += (nal[2] & 0x80) != 0;
            if (pictures == 1) {
                sample[used] = (uint8_t)(length >> 24), sample[used + 1] = (uint8_t)(length >> 16);
                sample[used + 2] = (uint8_t)(length >> 8), sample[used + 3] = (uint8_t)length;
                memcpy(sample + used + 4, nal, length);
                used += length + 4;
            }
        }
        i = end - 1;
    }
    *size = used;
    CMVideoFormatDescriptionRef format = NULL;
    if (used && sets[0] && sets[1] && sets[2])
        CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, sets, setSizes, 4, NULL, &format);
    return format;
}

// Four 288x160 tiles at x 0 and 512 (VIC's 256-byte tile alignment), y 0 and 160, into an 800x320 canvas.
static bool tileCheck(CFDictionaryRef spec, bool vic, char *line, size_t size)
{
    static uint8_t sampleBytes[sizeof(kHEVCClipAData) + 64];
    size_t bytes = 0;
    CMVideoFormatDescriptionRef format = firstPicture(sampleBytes, &bytes);
    if (!format) {
        snprintf(line, size, "tiles: no format description");
        return false;
    }
    gPresentOn = vic;
    TileAnswers answers = {0};
    void *session = NULL;
    const int submits = gSubmits, presents = gPresents;
    const OSStatus created = VTTileDecompressionSessionCreate(NULL, format, spec, tileOutput, &answers, &session);
    bool ok = vic ? !created : created != 0;
    if (!created) {
        CVPixelBufferRef canvas = NULL;
        const void *keys[] = {kCVPixelBufferBytesPerRowAlignmentKey, kCVPixelBufferPlaneAlignmentKey,
                              kCVPixelBufferIOSurfacePropertiesKey};
        const int32_t align = 256;
        CFNumberRef alignment = CFNumberCreate(NULL, kCFNumberSInt32Type, &align);
        CFDictionaryRef surfaceProps =
            CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *values[] = {alignment, alignment, surfaceProps};
        CFDictionaryRef attrs =
            CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferCreate(NULL, 800, 320, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &canvas);
        CFRelease(attrs), CFRelease(surfaceProps), CFRelease(alignment);
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, sampleBytes, bytes, kCFAllocatorNull, NULL, 0, bytes, 0, &block);
        CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &bytes, &sample);
        for (int i = 0; i < 4 && canvas && sample; i++) {
            const VTInt32Point at = {(i & 1) * 512, (i >> 1) * 160}, tile = {i & 1, i >> 1};
            const VTInt32Size tileSize = {288, 160};
            VTDecodeInfoFlags info = 0;
            ok &= !VTTileDecompressionSessionDecodeTile(session, sample, tile, tileSize, canvas, at, 0,
                                                        (void *)(intptr_t)i, &info);
        }
        for (int i = 0; i < 4; i++)
            ok &= answers.offsets[i][0] == (uint32_t)((i & 1) * 512) &&
                  answers.offsets[i][1] == (uint32_t)((i >> 1) * 160);
        ok &= answers.answered == 4 && answers.good == 4 && gSubmits - submits == 4 && gPresents - presents == 4 &&
              gLastDstX == 512 && gLastDstY == 160;
        if (sample)
            CFRelease(sample);
        if (block)
            CFRelease(block);
        if (canvas)
            CVPixelBufferRelease(canvas);
        VTTileDecompressionSessionInvalidate(session);
        CFRelease(session);
    } else {
        ok &= gSubmits == submits;
    }
    gPresentOn = false;
    CFRelease(format);
    snprintf(line, size, "tiles (VIC %s): create %d, %d answered, %d good, %d submits, %d presents, last at %u,%u",
             vic ? "on" : "off", (int)created, answers.answered, answers.good, gSubmits - submits, gPresents - presents,
             gLastDstX, gLastDstY);
    return ok;
}

int main(void)
{
    gAVDLog = os_log_create("com.brunostjohn.switch.avd", "host-test");
    setvbuf(stdout, NULL, _IONBF, 0);  // a hung session still shows how far it got
    // A class ID of our own makes the registry entry "com.brunostjohn.switch.avd-host.avc1", which the sessions name below: the
    // Mac's own H.264 decoders would otherwise win the selection.
    const void *keys[] = {CFSTR("CMClassImplementationID")}, *values[] = {CFSTR("com.brunostjohn.switch.avd-host")};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus registered = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, createInstance) ||
                                registerCheckDecoder(kCMVideoCodecType_VP9) ||
                                registerCheckDecoder(kCMVideoCodecType_HEVC);
    CFRelease(info);
    // This process is the check (its private registration makes instances only while one is open).
    __atomic_store_n(&gClaimer, true, __ATOMIC_RELEASE);
    __atomic_store_n(&gCheckOpen, true, __ATOMIC_RELEASE);
    if (registered) {
        printf("FAIL avd: VTRegisterVideoDecoderWithInfo %d\n", (int)registered);
        return 1;
    }
    const void *specKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *specValues[] = {CFSTR("com.brunostjohn.switch.avd-host.avc1")};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    char engine[512], full[512], present[512];
    clip_check(ClipCheckHardware, spec, engine, sizeof(engine));
    const int engineOpens = gOpens, engineSubmits = gSubmits;
    // VIC's present: the same session with the feature on.
    gPresentOn = true;
    const int presentSubmits = gSubmits, presentWaits = gWaitRanges;
    clip_check(ClipCheckHardware, spec, present, sizeof(present));
    gPresentOn = false;
    const bool presentOk = strstr(present, "hardware: hardware decoder") && strstr(present, " 30 out (") &&
                           gSubmits - presentSubmits == 30 && gPresents == 30 && !gPresentBad &&
                           gWaitRanges - presentWaits == 30;  // the status alone, no surface
    printf("present: %s (presents %d, refused %d, wait ranges %d)\n", present, gPresents, gPresentBad,
           gWaitRanges - presentWaits);
    gFull = true;
    clip_check(ClipCheckHardware, spec, full, sizeof(full));
    CFRelease(spec);
    gFull = false;
    char vp9[512];
    const int vp9Submits = gSubmits;
    const void *vp9Keys[] = {kVTVideoDecoderSpecification_DecoderID}, *vp9Values[] = {kCheckDecoderID};
    CFDictionaryRef vp9Spec = CFDictionaryCreate(NULL, vp9Keys, vp9Values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckVP9, vp9Spec, vp9, sizeof(vp9));
    printf("vp9: %s (submits %d)\n", vp9, gSubmits - vp9Submits);
    // Outside an open check the private registration makes no instance: VideoToolbox finds no decoder by that name.
    char closed[512];
    __atomic_store_n(&gCheckOpen, false, __ATOMIC_RELEASE);
    const int closedSubmits = gSubmits;
    clip_check(ClipCheckVP9, vp9Spec, closed, sizeof(closed));
    const bool gated = strstr(closed, "VTDecompressionSessionCreate failed") && gSubmits == closedSubmits;
    printf("vp9 outside the check: %s\n", closed);
    __atomic_store_n(&gCheckOpen, true, __ATOMIC_RELEASE);
    CFRelease(vp9Spec);
    // HEVC on the private check registration: clips A and V, then the Main 10 clip T, through HEVCNVDEC and the kernel
    // checker (the fake engine writes nothing, so no frame matches: all must come out, from a hardware session); then
    // with every buffer refused, Apple's software HEVC decoder inside the instance, which must match FFmpeg.
    char hevc[512], hevc10[512], hevcFull[512];
    const int hevcSubmits = gSubmits;
    const void *hevcKeys[] = {kVTVideoDecoderSpecification_DecoderID}, *hevcValues[] = {kCheckHEVCDecoderID};
    CFDictionaryRef hevcSpec = CFDictionaryCreate(NULL, hevcKeys, hevcValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckHEVC, hevcSpec, hevc, sizeof(hevc));
    printf("hevc: %s\n", hevc);
    const int hevcMain = gSubmits - hevcSubmits;
    clip_check(ClipCheckHEVC10, hevcSpec, hevc10, sizeof(hevc10));
    printf("hevc10: %s\n", hevc10);
    const int hevcTen = gSubmits - hevcSubmits - hevcMain;
    gFull = true;
    clip_check(ClipCheckHEVC, hevcSpec, hevcFull, sizeof(hevcFull));
    gFull = false;
    char tiles[256], tilesOff[256];
    const bool tilesOk =
        tileCheck(hevcSpec, true, tiles, sizeof(tiles)) && tileCheck(hevcSpec, false, tilesOff, sizeof(tilesOff));
    printf("%s\n%s\n", tiles, tilesOff);
    CFRelease(hevcSpec);
    printf("hevc: %s (submits %d)\nhevc10: %s (submits %d)\nhevc full: %s\n", hevc, hevcMain, hevc10, hevcTen,
           hevcFull);
    const bool hevcOk = strstr(hevc, "hevc: hardware decoder") && strstr(hevc, "A 0/20") && strstr(hevc, "V 0/14") &&
                        !strstr(hevc, " out,") && hevcMain == 34 && !gRefused &&
                        strstr(hevc10, "hevc10: hardware decoder") && strstr(hevc10, "T 0/10") &&
                        !strstr(hevc10, " out,") && hevcTen == 10 && strstr(hevcFull, "software decoder") &&
                        strstr(hevcFull, "A 20/20, V 14/14");
    printf("engine: %s (opens %d, submits %d, refused %d)\nfull: %s (opens %d)\n", engine, engineOpens, engineSubmits,
           gRefused, full, gOpens - engineOpens);
    const bool ok = engineOpens >= 1 && engineSubmits == 30 && !gRefused &&
                    strstr(engine, "hardware: hardware decoder") && strstr(engine, " 30 out (") &&
                    gOpens > engineOpens && strstr(full, "hardware: software decoder, 30/30 frames match x264") &&
                    strstr(vp9, "vp9: hardware decoder") && strstr(vp9, " 16 out (") &&
                    hevcSubmits - vp9Submits == 18 && gated && presentOk && hevcOk && tilesOk;
    printf("%s avd: plug-in on VideoToolbox (H.264, VP9 and HEVC on a fake NVDEC, then Apple's software H.264 and "
           "HEVC in the instance)\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
