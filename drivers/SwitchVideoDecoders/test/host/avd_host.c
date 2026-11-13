// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVD.videodecoder's VideoToolbox side, on a Mac: the plug-in itself (AVDDecoder.c, included for its
// factory) over a fake TegraNVDEC client, registered in this process and driven by the clip check (ClipCheck.c)
// through VideoToolbox, which checks the plug-in's class layout and session calls against a real VideoToolbox.
//   engine: the fake client hands out buffers and checks every submit with the kernel's checker; its engine writes
//           nothing, so pictures come out black: the test wants all 30 out of a session that says hardware.
//   full:   the fake client refuses every buffer, so the instance runs Apple's software decoder: the test wants 30/30
//           frames matching x264 from a session that says software.
//   vp9:    VP9Clip.h as 'vp09' (kVTVideoDecoderSpecification_AllowVP9Decode) on the fake engine: every superframe and
//           frame through VP9NVDEC and the kernel's checker, all 16 shown frames out of a session that says hardware.
//   present: the engine session again with TegraNVDECPresent on: every picture handed to the fake VIC, whose pool
//           buffer must be one VIC addresses (rows and the chroma plane on 256 bytes) and which copies it as VIC would;
//           no wait invalidates the surface, and the CPU path's pictures come out.
//   tiles:  clip A's first picture as four tiles of one canvas through VTTileDecompressionSession (AVDTiles.c): the
//           canvas made from the tile requirements, each tile presented at its offset, each answered; with VIC off the
//           tile session is refused and VideoToolbox finds no other decoder by that name.
//   hevc:   HEVCClip.h's Main clips A and V and Main 10 clip T as 'hvc1' on the fake engine: every picture through
//           HEVCNVDEC and the kernel's checker, all out of a hardware session; clip T again with VIC's present on,
//           every picture handed over as 16-bit samples into an 'x420' buffer; then with every buffer refused, Apple's
//           software HEVC decoder in it, matching FFmpeg.
//   mp4v:   MPEG4Clip.h's clip A (Advanced Simple) as 'mp4v' with its 'esds' (mpeg_session.c) on the fake engine:
//           all 18 VOPs through MPEG4NVDEC and the kernel's checker out of a hardware session; then with every buffer
//           refused, clip B (Simple: the Mac's MPEG-4 decoder refuses clip A) on Apple's software decoder in the
//           instance.
//   mp2v:   MPEG2Clip.h's MPEG-2 clip A as 'mp2v' and its MPEG-1 clip C as 'mp1v', described without extensions as
//           MediaToolbox's MPEG-2 stream readers describe them, every picture through MPEG2NVDEC and the kernel's
//           checker out of a hardware session; asynchronously, every frame from the completions in decode order, and
//           with three jobs the engine cancels, those three frames out with an error and the rest decoded.
//   alpha:  clip A's first picture through sessions opened as MuxedAlphaDecoder opens its layer sessions (AVDAlpha.c):
//           asked nothing, a '420v' picture and no plane; asked for planes 0 or 2, a 'v0a8' picture, that plane
//           answered and the picture handed to VIC for those planes.
// Prints "PASS avd ..." when all hold (avd_host_test).
#include "../../AVD/AVDDecoder.c"
#include "../../AVD/ClipCheck.h"
#include <stdatomic.h>
#include <stdio.h>
#include "../HEVCClip.h"
#include "mpeg_session.h"
#include "../../../TegraNVDEC/TegraNVDECCheck.h"

// The fake engine's completions: each job's, on the session's queue a little after its Submit, as NVDEC reports a job
// it finished while the decode thread went on. Held by every completion still to come, so a client closed meanwhile
// is not touched.
typedef struct {
    atomic_int references;
    atomic_bool stopped;
    dispatch_queue_t queue;
    NVDECDone done;
    void *context;
} FakeListener;

struct NVDECClient {
    uint8_t *va[64];
    uint64_t size[64];
    FakeListener *listener;
};

static bool gFull, gPresentOn, gNativeOn, gEarlyCompletions;
static uint32_t gFailFrom, gFailCount;  // jobs the fake engine cancels: a stalled one and those queued behind it
static int gOpens, gSubmits, gRefused, gPresents, gPresentBad, gWaitRanges, gWidePresents, gWaits, gSurfaceWaits;
static uint32_t gLastDstX, gLastDstY;
static unsigned gNativeExports, gRecycled, gAlphaPresents[2];

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

static const char kOnQueue = 0;

static void listenerRelease(FakeListener *l)
{
    if (atomic_fetch_sub(&l->references, 1) != 1)
        return;
    dispatch_release(l->queue);
    free(l);
}

typedef struct {
    FakeListener *listener;
    uint32_t fence;
} FakeCompletion;

static void fakeCompleted(void *context)
{
    FakeCompletion *c = context;
    const bool cancelled = gFailFrom && c->fence - gFailFrom < gFailCount;
    if (!atomic_load(&c->listener->stopped))
        c->listener->done(c->listener->context, c->fence, cancelled ? -5 : 0, 0, 0);
    listenerRelease(c->listener);
    free(c);
}

static void nothing(void *context)
{
    (void)context;
}

bool nvdec_listen(NVDECClient *c, dispatch_queue_t queue, NVDECDone done, void *context)
{
    FakeListener *l = calloc(1, sizeof(*l));
    if (!l)
        return false;
    dispatch_retain(queue);
    *l = (FakeListener){.queue = queue, .done = done, .context = context};
    atomic_init(&l->references, 1);
    dispatch_queue_set_specific(queue, &kOnQueue, l, NULL);
    c->listener = l;
    return true;
}

bool nvdec_completions_on_port(const NVDECClient *c)
{
    return c->listener != NULL;
}

bool nvdec_buffer_stats(const NVDECClient *c, SwitchNVDECBufferStats *stats)  // a release kernel's answer
{
    (void)c, (void)stats;
    return false;
}

void nvdec_stop_listening(NVDECClient *c)
{
    FakeListener *l = c->listener;
    if (!l)
        return;
    c->listener = NULL;
    atomic_store(&l->stopped, true);
    if (dispatch_get_specific(&kOnQueue) != l)
        dispatch_sync_f(l->queue, NULL, nothing);
    listenerRelease(l);
}

void *nvdec_buffer_va(const NVDECClient *c, uint32_t handle)
{
    return handle < 64 ? c->va[handle] : NULL;
}

void nvdec_close(NVDECClient *c)
{
    if (!c)
        return;
    nvdec_stop_listening(c);
    for (int i = 0; i < 64; i++)
        free(c->va[i]);
    free(c);
}

uint32_t nvdec_jobs_in_flight(const NVDECClient *c)
{
    (void)c;
    return 8;
}

uint32_t nvdec_codecs(const NVDECClient *c)
{
    (void)c;
    return 1u << TEGRA_NVDEC_CODEC_H264 | 1u << TEGRA_NVDEC_CODEC_VP9 | 1u << TEGRA_NVDEC_CODEC_HEVC |
           TEGRA_NVDEC_HEVC_MAIN10 | 1u << TEGRA_NVDEC_CODEC_MPEG4 | 1u << TEGRA_NVDEC_CODEC_MPEG12;
}

bool nvdec_can_present(const NVDECClient *c)
{
    (void)c;
    return gPresentOn;
}

// The fake VIC: TegraNVDECUserClient's geometry checks and TegraVICConfig.h's address rules for the destination, then
// the copy (BlockLinear.c, as VIC's pass-through would leave it). 16-bit samples ('x420') go as bytes, as the kernel
// sends them to VIC: an 8-bit picture of twice the width.
bool switch_nvdec_can_export(const NVDECClient *client)
{
    (void)client;
    return gNativeOn;
}

int switch_nvdec_export(NVDECClient *client, const TegraNVDECPresent *picture)
{
    if (!gNativeOn || picture->sample_bytes != 1 || picture->dst_x || picture->dst_y || picture->handle >= 64 ||
        !client->va[picture->handle])
        return kIOReturnUnsupported;
    ++gNativeExports;
    return 0;
}

int switch_nvdec_recycle(void *client, uint32_t handle, uint64_t bytes, void **address)
{
    NVDECClient *owner = client;
    if (!gNativeOn)
        return 0;
    if (!handle || handle >= 64 || bytes != owner->size[handle])
        return -1;
    void *replacement = calloc(1, bytes);
    if (!replacement)
        return -1;
    free(owner->va[handle]);
    owner->va[handle] = replacement;
    *address = replacement;
    ++gRecycled;
    return 0;
}

// A 'v0a8' surface takes plane 0 (its Y and CbCr planes) or plane 2 (the picture's luma alone into its alpha plane).
static bool alphaPresent(NVDECClient *c, const TegraNVDECPresent *p, IOSurfaceRef s)
{
    const uint32_t first = p->plane;
    if (IOSurfaceGetPlaneCount(s) != 3 || (first != 0 && first != 2) || p->sample_bytes != 1)
        return false;
    for (uint32_t k = 0; k < 3; k++)
        if (IOSurfaceGetBytesPerRowOfPlane(s, k) % 256 ||
            ((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, k) - (uintptr_t)IOSurfaceGetBaseAddress(s)) % 256)
            return false;
    IOSurfaceLock(s, 0, NULL);
    const uint8_t *picture = c->va[p->handle];
    block_linear_copy(IOSurfaceGetBaseAddressOfPlane(s, first), IOSurfaceGetBytesPerRowOfPlane(s, first), picture,
                      p->pitch, p->crop_x, p->crop_y, p->width, p->height);
    if (!first)
        block_linear_copy(IOSurfaceGetBaseAddressOfPlane(s, 1), IOSurfaceGetBytesPerRowOfPlane(s, 1),
                          picture + p->chroma_offset, p->pitch, p->crop_x, p->crop_y / 2, p->width, p->height / 2);
    IOSurfaceUnlock(s, 0, NULL);
    gAlphaPresents[first ? 1 : 0]++;
    return true;
}

int nvdec_present(NVDECClient *c, const TegraNVDECPresent *p)
{
    gPresents++;
    const uint64_t luma = (uint64_t)p->pitch * p->rows;
    const uint32_t bytes = p->sample_bytes;
    IOSurfaceRef s = IOSurfaceLookup(p->surface_id);
    const OSType format = s ? IOSurfaceGetPixelFormat(s) : 0;
    bool ok = p->handle < 64 && c->va[p->handle] && p->pitch && !(p->pitch % 64) && p->rows && !(p->rows % 32) &&
              p->block_height_log2 == 1 && !(p->chroma_offset % 256) && p->chroma_offset >= luma &&
              p->chroma_offset + luma / 2 <= c->size[p->handle] && s;
    if (ok && format == kCVPixelFormatType_420YpCbCr8VideoRange_8A_TriPlanar) {
        ok = alphaPresent(c, p, s);
        if (!ok)
            gPresentBad++;
        CFRelease(s);
        return ok ? 0 : kIOReturnUnsupported;
    }
    ok = ok && !p->plane &&
         (bytes == 2 ? format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                           format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                     : bytes == 1);
    if (ok) {
        const uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(s);
        ok = !(IOSurfaceGetBytesPerRowOfPlane(s, 0) % 256) && !(IOSurfaceGetBytesPerRowOfPlane(s, 1) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 0) - base) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 1) - base) % 256) && !(p->dst_x * bytes % 256) &&
             !(p->dst_y & 1) && p->dst_x + p->width <= IOSurfaceGetWidth(s) &&
             p->dst_y + p->height <= IOSurfaceGetHeight(s) && !((p->width | p->height) & 1) &&
             (p->crop_x + p->width) * bytes <= p->pitch;
    }
    if (ok) {
        IOSurfaceLock(s, 0, NULL);
        const uint8_t *picture = c->va[p->handle];
        const size_t lumaPitch = IOSurfaceGetBytesPerRowOfPlane(s, 0),
                     chromaPitch = IOSurfaceGetBytesPerRowOfPlane(s, 1);
        block_linear_copy((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0) + p->dst_y * lumaPitch + p->dst_x * bytes,
                          lumaPitch, picture, p->pitch, p->crop_x * bytes, p->crop_y, p->width * bytes, p->height);
        block_linear_copy((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1) + p->dst_y / 2 * chromaPitch +
                              p->dst_x * bytes,
                          chromaPitch, picture + p->chroma_offset, p->pitch, p->crop_x * bytes, p->crop_y / 2,
                          p->width * bytes, p->height / 2);
        if (bytes == 2)
            gWidePresents++;
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

int nvdec_submit(void *ctx, const TegraNVDECSubmit *s, TegraNVDECFence *reply)
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
    *reply = (TegraNVDECFence){(uint32_t)++gSubmits, 0};
    FakeCompletion *completion = c->listener ? malloc(sizeof(*completion)) : NULL;
    if (completion) {
        *completion = (FakeCompletion){c->listener, reply->fence};
        atomic_fetch_add(&c->listener->references, 1);
        if (gEarlyCompletions)  // the job retires before its Submit returns to the plug-in
            dispatch_sync_f(c->listener->queue, completion, fakeCompleted);
        else
            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), c->listener->queue, completion,
                             fakeCompleted);
    }
    return 0;
}

int nvdec_wait(void *ctx, const TegraNVDECWait *w)
{
    (void)ctx;
    gWaits++;
    gWaitRanges += (int)w->invalidate_count;
    for (uint32_t i = 0; i < w->invalidate_count; i++)
        gSurfaceWaits += w->invalidate[i].length > NVDEC_STATUS_BYTES;
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

extern OSStatus VTDecompressionSessionCreateWithOptions(CFAllocatorRef, CMVideoFormatDescriptionRef, CFDictionaryRef,
                                                        CFDictionaryRef, const VTDecompressionOutputCallbackRecord *,
                                                        CFDictionaryRef, VTDecompressionSessionRef *);

// One layer session as MuxedAlphaDecoder opens it (session options SelectPixelFormatWithAlpha and
// WriteDirectlyToPlanesOfTargetCVPixelBuffer = `plane`; -1: neither), clip A's first picture through it with VIC on.
static bool alphaCheck(CFDictionaryRef spec, int32_t plane, char *line, size_t size)
{
    static uint8_t sampleBytes[sizeof(kHEVCClipAData) + 64];
    size_t bytes = 0;
    CMVideoFormatDescriptionRef format = firstPicture(sampleBytes, &bytes);
    CFMutableDictionaryRef options =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (plane >= 0) {
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &plane);
        CFDictionarySetValue(options, CFSTR("SelectPixelFormatWithAlpha"), kCFBooleanTrue);
        CFDictionarySetValue(options, CFSTR("WriteDirectlyToPlanesOfTargetCVPixelBuffer"), number);
        CFRelease(number);
    }
    gPresentOn = true;
    const unsigned before[2] = {gAlphaPresents[0], gAlphaPresents[1]};
    VTDecompressionSessionRef session = NULL;
    const OSStatus created =
        format ? VTDecompressionSessionCreateWithOptions(NULL, format, spec, NULL, NULL, options, &session) : -1;
    __block OSType out = 0;
    __block OSStatus decoded = -1;
    int32_t answer = -1;
    if (!created) {
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, sampleBytes, bytes, kCFAllocatorNull, NULL, 0, bytes, 0, &block);
        CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &bytes, &sample);
        VTDecompressionSessionDecodeFrameWithOutputHandler(
            session, sample, 0, NULL,
            ^(OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime duration) {
                (void)flags, (void)pts, (void)duration;
                decoded = status;
                out = image ? CVPixelBufferGetPixelFormatType(image) : 0;
            });
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        CFTypeRef value = NULL;
        VTSessionCopyProperty(session, CFSTR("WriteDirectlyToPlanesOfTargetCVPixelBuffer"), NULL, &value);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(value, kCFNumberSInt32Type, &answer);
        if (value)
            CFRelease(value);
        CFRelease(sample), CFRelease(block);
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
    }
    gPresentOn = false;
    CFRelease(options);
    if (format)
        CFRelease(format);
    const unsigned colour = gAlphaPresents[0] - before[0], alpha = gAlphaPresents[1] - before[1];
    snprintf(line, size,
             "alpha (plane %d): create %d, decoded %d into '%.4s', answered plane %d, presents %u colour %u alpha",
             (int)plane, (int)created, (int)decoded, (const char *)&(uint32_t){CFSwapInt32HostToBig(out)}, (int)answer,
             colour, alpha);
    return !created && !decoded && answer == plane &&
           (plane < 0 ? out == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && !colour && !alpha
                      : out == kCVPixelFormatType_420YpCbCr8VideoRange_8A_TriPlanar && colour == (plane == 0) &&
                            alpha == (plane == 2));
}

int main(void)
{
    gAVDLog = os_log_create("com.brunostjohn.switch.avd", "host-test");
    setvbuf(stdout, NULL, _IONBF, 0);  // a hung session still shows how far it got
    // A class ID of our own makes the registry entries "com.brunostjohn.switch.avd-host.avc1" (.vp09, .hvc1), which the
    // sessions name below: the Mac's own decoders would otherwise win the selection.
    const void *keys[] = {CFSTR("CMClassImplementationID")}, *values[] = {CFSTR("com.brunostjohn.switch.avd-host")};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const FourCharCode codecs[] = {kCMVideoCodecType_H264,       kCMVideoCodecType_VP9,
                                   kCMVideoCodecType_HEVC,       kCMVideoCodecType_MPEG4Video,
                                   kCMVideoCodecType_MPEG2Video, kCMVideoCodecType_MPEG1Video};
    OSStatus registered = noErr;
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]) && !registered; i++)
        registered = VTRegisterVideoDecoderWithInfo(codecs[i], info, avd_create_instance);
    CFRelease(info);
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
    const int presentSubmits = gSubmits, presentWaits = gWaitRanges, presentSurfaceWaits = gSurfaceWaits;
    clip_check(ClipCheckHardware, spec, present, sizeof(present));
    gPresentOn = false;
    // ClipCheck decodes synchronously: each frame's status waited for, never its surface.
    const bool presentOk = strstr(present, "hardware: hardware decoder") && strstr(present, " 30 out (") &&
                           gSubmits - presentSubmits == 30 && gPresents == 30 && !gPresentBad &&
                           gWaitRanges - presentWaits <= 30 && gSurfaceWaits == presentSurfaceWaits;
    printf("present: %s (presents %d, refused %d, wait ranges %d)\n", present, gPresents, gPresentBad,
           gWaitRanges - presentWaits);
    gFull = true;
    clip_check(ClipCheckHardware, spec, full, sizeof(full));
    gFull = false;
    char vp9[512];
    const int vp9Submits = gSubmits;
    const void *vp9Keys[] = {kVTVideoDecoderSpecification_DecoderID},
               *vp9Values[] = {CFSTR("com.brunostjohn.switch.avd-host.vp09")};
    CFDictionaryRef vp9Spec = CFDictionaryCreate(NULL, vp9Keys, vp9Values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckVP9, vp9Spec, vp9, sizeof(vp9));
    printf("vp9: %s (submits %d)\n", vp9, gSubmits - vp9Submits);
    CFRelease(vp9Spec);
    // HEVC: clips A and V, then the Main 10 clip T, through HEVCNVDEC and the kernel
    // checker (the fake engine writes nothing, so no frame matches: all must come out, from a hardware session); then
    // with every buffer refused, Apple's software HEVC decoder inside the instance, which must match FFmpeg.
    char hevc[512], hevc10[512], hevcFull[512];
    const int hevcSubmits = gSubmits;
    const void *hevcKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *hevcValues[] = {CFSTR("com.brunostjohn.switch.avd-host.hvc1")};
    CFDictionaryRef hevcSpec = CFDictionaryCreate(NULL, hevcKeys, hevcValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckHEVC, hevcSpec, hevc, sizeof(hevc));
    printf("hevc: %s\n", hevc);
    const int hevcMain = gSubmits - hevcSubmits;
    clip_check(ClipCheckHEVC10, hevcSpec, hevc10, sizeof(hevc10));
    printf("hevc10: %s\n", hevc10);
    const int hevcTen = gSubmits - hevcSubmits - hevcMain;
    // Main 10 with VIC's present on: every picture handed over as 16-bit samples into an 'x420' buffer.
    char hevc10Vic[512];
    gPresentOn = true;
    const int widePresents = gWidePresents, presentsBefore = gPresents, badBefore = gPresentBad;
    clip_check(ClipCheckHEVC10, hevcSpec, hevc10Vic, sizeof(hevc10Vic));
    gPresentOn = false;
    const bool hevc10VicOk = strstr(hevc10Vic, "hevc10: hardware decoder") && strstr(hevc10Vic, "T 0/10") &&
                             !strstr(hevc10Vic, " out,") && gWidePresents - widePresents == 10 &&
                             gPresents - presentsBefore == 10 && gPresentBad == badBefore;
    printf("hevc10 vic: %s (16-bit presents %d)\n", hevc10Vic, gWidePresents - widePresents);
    gFull = true;
    clip_check(ClipCheckHEVC, hevcSpec, hevcFull, sizeof(hevcFull));
    gFull = false;
    char tiles[256], tilesOff[256];
    const bool tilesOk =
        tileCheck(hevcSpec, true, tiles, sizeof(tiles)) && tileCheck(hevcSpec, false, tilesOff, sizeof(tilesOff));
    printf("%s\n%s\n", tiles, tilesOff);
    bool alphaOk = true;
    for (int32_t plane = -1; plane <= 2; plane += plane < 0 ? 1 : 2) {
        char alpha[256];
        alphaOk &= alphaCheck(hevcSpec, plane, alpha, sizeof(alpha));
        printf("%s\n", alpha);
    }
    CFRelease(hevcSpec);
    printf("hevc: %s (submits %d)\nhevc10: %s (submits %d)\nhevc full: %s\n", hevc, hevcMain, hevc10, hevcTen,
           hevcFull);
    const bool hevcOk = strstr(hevc, "hevc: hardware decoder") && strstr(hevc, "A 0/20") && strstr(hevc, "V 0/14") &&
                        !strstr(hevc, " out,") && hevcMain == 34 && !gRefused &&
                        strstr(hevc10, "hevc10: hardware decoder") && strstr(hevc10, "T 0/10") &&
                        !strstr(hevc10, " out,") && hevcTen == 10 && strstr(hevcFull, "software decoder") &&
                        strstr(hevcFull, "A 20/20, V 14/14") && hevc10VicOk;
    printf("engine: %s (opens %d, submits %d, refused %d)\nfull: %s (opens %d)\n", engine, engineOpens, engineSubmits,
           gRefused, full, gOpens - engineOpens);
    char mp4v[256], mp4vFull[256];
    const int mp4vSubmits = gSubmits, mp4vRefused = gRefused;
    const void *mp4vKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *mp4vValues[] = {CFSTR("com.brunostjohn.switch.avd-host.mp4v")};
    CFDictionaryRef mp4vSpec = CFDictionaryCreate(NULL, mp4vKeys, mp4vValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    const bool mp4vEngine = mpeg4_session_check(mp4vSpec, false, true, mp4v, sizeof(mp4v)) &&
                            gSubmits - mp4vSubmits == 18 && gRefused == mp4vRefused;
    gFull = true;
    const bool mp4vSoftware = mpeg4_session_check(mp4vSpec, true, false, mp4vFull, sizeof(mp4vFull));
    gFull = false;
    CFRelease(mp4vSpec);
    printf("%s (submits %d)\nmp4v full: %s\n", mp4v, gSubmits - mp4vSubmits, mp4vFull);
    char mp2v[256], mp1v[256];
    const int mp2vSubmits = gSubmits;
    const void *mp2vKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *mp2vValues[] = {CFSTR("com.brunostjohn.switch.avd-host.mp2v")},
               *mp1vValues[] = {CFSTR("com.brunostjohn.switch.avd-host.mp1v")};
    CFDictionaryRef mp2vSpec = CFDictionaryCreate(NULL, mp2vKeys, mp2vValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef mp1vSpec = CFDictionaryCreate(NULL, mp2vKeys, mp1vValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    const bool mpeg2Engine = mpeg2_session_check(mp2vSpec, false, mp2v, sizeof(mp2v)) &&
                             mpeg2_session_check(mp1vSpec, true, mp1v, sizeof(mp1v)) &&
                             gSubmits - mp2vSubmits == 18 + 15 && gRefused == mp4vRefused;
    // Asynchronous decode with VIC placing every picture: the fake engine reports each job 20 ms after its Submit, and
    // the decode thread never waits; every frame comes out of the completions, in decode order.
    static const char *const kAsyncKeys[] = {"picture_waits", "sync_waits", "in_flight_max", "frames"};
    int32_t asyncRoutes[4];
    char async[256];
    const int asyncWaits = gWaits;
    gPresentOn = true;
    const bool asyncRun = mpeg2_session_order(mp2vSpec, kVTDecodeFrame_EnableAsynchronousDecompression, kAsyncKeys,
                                              asyncRoutes, 4, async, sizeof(async));
    gPresentOn = false;
    // Again with each completion delivered before the plug-in's Submit call returns, ahead of the job's registration.
    char early[256];
    int32_t earlyRoutes[4];
    gPresentOn = gEarlyCompletions = true;
    const bool earlyRun = mpeg2_session_order(mp2vSpec, kVTDecodeFrame_EnableAsynchronousDecompression, kAsyncKeys,
                                              earlyRoutes, 4, early, sizeof(early));
    gPresentOn = gEarlyCompletions = false;
    const bool asyncOk = asyncRun && earlyRun && gWaits == asyncWaits && !asyncRoutes[0] && !asyncRoutes[1] &&
                         asyncRoutes[2] >= 2 && asyncRoutes[3] == 18 && !earlyRoutes[0] && !earlyRoutes[1] &&
                         earlyRoutes[3] == 18;
    // Again with a decode NVDEC cancels at its bound and the two queued behind it (TegraNVDECCancel.cpp): their
    // completions come with the failure, those three frames come out failed and the session goes on.
    char cancelled[256];
    int32_t cancelledRoutes[4];
    gPresentOn = true;
    gFailFrom = (uint32_t)gSubmits + 6;
    gFailCount = 3;
    mpeg2_session_order(mp2vSpec, kVTDecodeFrame_EnableAsynchronousDecompression, kAsyncKeys, cancelledRoutes, 4,
                        cancelled, sizeof(cancelled));
    gPresentOn = false;
    gFailFrom = gFailCount = 0;
    const bool cancelledOk =
        strstr(cancelled, "hardware decoder, 15 of 18 out, 3 errors, 0 refused, 0 out of decode order") &&
        !cancelledRoutes[0] && !cancelledRoutes[1];
    printf("async, a cancelled decode and the two behind it: %s\n", cancelled);
    printf("async %s (decode-thread waits %d; SwitchPictureRoutes picture_waits %d, sync_waits %d, in_flight_max %d, "
           "frames %d)\nasync, completions before Submit returns: %s\n",
           async, gWaits - asyncWaits, asyncRoutes[0], asyncRoutes[1], asyncRoutes[2], asyncRoutes[3], early);
    CFRelease(mp2vSpec);
    CFRelease(mp1vSpec);
    printf("%s\n%s (submits %d)\n", mp2v, mp1v, gSubmits - mp2vSubmits);
    char native[512];
    gPresentOn = gNativeOn = true;
    const unsigned exportsBefore = gNativeExports, recycleBefore = gRecycled;
    const int copiesBefore = gPresents, nativeWaits = gWaitRanges, nativeSurfaceWaits = gSurfaceWaits;
    clip_check(ClipCheckHardware, spec, native, sizeof(native));
    gPresentOn = gNativeOn = false;
    CFRelease(spec);
    const bool nativeOk = strstr(native, "hardware: hardware decoder") && strstr(native, " 30 out (") &&
                          gNativeExports - exportsBefore == 30 && gRecycled - recycleBefore == 30 &&
                          gPresents == copiesBefore && gWaitRanges - nativeWaits <= 30 &&
                          gSurfaceWaits == nativeSurfaceWaits;
    printf("native: %s (exports %u, recycled %u, pitch copies %d)\n", native, gNativeExports - exportsBefore,
           gRecycled - recycleBefore, gPresents - copiesBefore);
    const bool ok = engineOpens >= 1 && engineSubmits == 30 && !gRefused &&
                    strstr(engine, "hardware: hardware decoder") && strstr(engine, " 30 out (") &&
                    gOpens > engineOpens && strstr(full, "hardware: software decoder, 30/30 frames match x264") &&
                    strstr(vp9, "vp9: hardware decoder") && strstr(vp9, " 16 out (") &&
                    hevcSubmits - vp9Submits == 18 && presentOk && hevcOk && tilesOk && alphaOk && nativeOk &&
                    mp4vEngine && mp4vSoftware && mpeg2Engine && asyncOk && cancelledOk;
    printf("%s avd: plug-in on VideoToolbox (H.264, VP9, HEVC, HEVC with alpha, MPEG-4 Part 2 and MPEG-1/2 on a fake "
           "NVDEC, then Apple's software H.264, HEVC and MPEG-4 Part 2 in the instance)\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
