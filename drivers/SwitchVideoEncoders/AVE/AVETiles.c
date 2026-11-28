// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC tile sessions on NVENC: HEIC grids. ImageIO hands an image to CMPhoto, which opens a VTTileCompressionSession
// ('hvc1', 512x512 tiles unless the image is smaller) and encodes the picture a tile at a time; HEIF keeps each tile
// as an item of its own in a grid, and a grid's tiles are all one size. So every tile here is one NVENC IDR picture of
// the session's tile size. When TegraNVENC completion notifications are active, up to AVE_TILE_SLOTS tiles pipeline
// asynchronously and emit in submission order on the completion queue before ave_complete_tiles returns.
//
// The contract is that of Apple's AVE HEVC encoder (ave.videoencoder, 24B119): AVE_Session_HEVC_StartTileSession
// 0x2c4617800 states the size it codes tiles at (never under its smallest picture) with the source pixel buffer
// attributes (VTTileEncoderSessionSetTileAttributes 0x2c4617e34), then the same attributes for the canvas and the tile
// requirements: origins on even samples both ways (0x2c46180a0..0x2c4618100), and source padding, which AVE states
// on its newer devices (0x2c4618110). One hvcC serves every tile.
#include <CoreVideo/CoreVideo.h>
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <mach/mach_time.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "AVEEncoder.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop

enum {
    kOffsetAlignment = 2,  // 4:2:0: a tile starts on a chroma sample
    kSmallestSide = 32,    // NVENC's smallest picture side (TegraNVENCCheck.h)
};

static void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

static CFDictionaryRef copyRequirements(void)
{
    CFMutableDictionaryRef requirements = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    setNumber(requirements, kVTTileEncoderRequirement_HorizontalOffsetAlignment, kOffsetAlignment);
    setNumber(requirements, kVTTileEncoderRequirement_VerticalOffsetAlignment, kOffsetAlignment);
    CFDictionarySetValue(requirements, kVTTileEncoderRequirement_EncoderSupportsSourcePadding, kCFBooleanTrue);
    return requirements;
}

static int32_t codedSide(int32_t side)
{
    return side < kSmallestSide ? kSmallestSide : (side + 1) & ~1;
}

OSStatus ave_start_tile_session(VTVideoEncoderRef ref, VTTileEncoderSession session, CFDictionaryRef specification,
                                VTInt32Size tileSize, CFAllocatorRef allocator)
{
    (void)specification, (void)allocator;
    Encoder *e = CMBaseObjectGetDerivedStorage(ref);
    if (tileSize.width < 1 || tileSize.height < 1)
        return kVTParameterErr;
    if (e->codec != kCMVideoCodecType_HEVC) {
        switch_refusal("AVE", "H.264 tile session", "tiles are coded as HEVC only; left to the next encoder");
        return kVTCouldNotFindVideoEncoderErr;
    }
    pthread_mutex_lock(&e->lock);
    e->size = (CMVideoDimensions){codedSide(tileSize.width), codedSide(tileSize.height)};
    OSStatus status = ave_open_engine(e);
    if (!status) {
        CFDictionaryRef attributes = ave_copy_input_attributes(e, 0, 0), requirements = copyRequirements();
        status =
            VTTileEncoderSessionSetTileAttributes(session, (VTInt32Size){e->size.width, e->size.height}, attributes);
        if (!status)
            status = VTTileEncoderSessionSetTileEncodeRequirements(session, attributes, requirements);
        CFRelease(attributes);
        CFRelease(requirements);
        if (status)
            os_log_error(gLog, "AVE: tile attributes refused in %{public}s: %d", getprogname(), (int)status);
    }
    if (!status)
        e->tileSession = session;
    pthread_mutex_unlock(&e->lock);
    return status;
}

OSStatus ave_prepare_to_encode_tiles(VTVideoEncoderRef ref, Boolean unused)
{
    (void)ref, (void)unused;
    return noErr;  // the stream starts with the first tile, whose source says full range or not
}

OSStatus ave_complete_tiles(VTVideoEncoderRef ref)
{
    Encoder *e = CMBaseObjectGetDerivedStorage(ref);
    pthread_mutex_lock(&e->lock);
    AVEEmitter *emitter = e->emitter;
    pthread_mutex_unlock(&e->lock);
    if (emitter)
        ave_emitter_flush_tiles(emitter);
    return noErr;
}

static bool isNV12(OSType format)
{
    return format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

// The part of the tile the source covers: the rest of the tile picture repeats its last row and column.
static OSStatus tileRegion(const Encoder *e, CVPixelBufferRef source, VTInt32Point origin, VTInt32Size size,
                           AVERegion *r)
{
    const OSType format = CVPixelBufferGetPixelFormatType(source);
    if (!isNV12(format) || CVPixelBufferGetPlaneCount(source) != 2 ||
        (e->configured && e->config.full_range != (format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange))) {
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("AVE", "tile source format"), format), "AVE",
                         "tile source format", "%#x: tiles take '420v' or '420f', one range per session", format);
        return kVTPixelTransferNotSupportedErr;
    }
    const int64_t width = (int64_t)CVPixelBufferGetWidth(source), height = (int64_t)CVPixelBufferGetHeight(source);
    if (origin.x < 0 || origin.y < 0 || (origin.x | origin.y) % kOffsetAlignment || origin.x >= width ||
        origin.y >= height || size.width < 1 || size.height < 1)
        return kVTParameterErr;
    int64_t w = width - origin.x, h = height - origin.y;
    w = w < size.width ? w : size.width;
    h = h < size.height ? h : size.height;
    *r = (AVERegion){(uint32_t)origin.x, (uint32_t)origin.y, (uint32_t)(w < e->size.width ? w : e->size.width),
                     (uint32_t)(h < e->size.height ? h : e->size.height)};
    return noErr;
}

// Constant quality from the Quality set last: CMPhoto sets properties again before each tile (0x1a6bb3484).
static uint8_t tileQP(const Encoder *e)
{
    RateControl rc;
    rc_init(&rc, 0, false, e->quality, 0, (uint32_t)e->size.width, (uint32_t)e->size.height);
    return (uint8_t)rc_qp(&rc, true);
}

static OSStatus createSample(const Encoder *e, size_t size, CMSampleBufferRef *out)
{
    CMBlockBufferRef block = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, size, kCFAllocatorDefault, NULL, 0,
                                                         size, kCMBlockBufferAssureMemoryNowFlag, &block);
    if (!status)
        status = CMBlockBufferReplaceDataBytes(e->avcc, block, 0, size);
    if (!status)
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, e->format, 1, 0, NULL, 1, &size, out);
    if (block)
        CFRelease(block);
    return status;
}

// Fallback single-slot tile path when completion notifications are unavailable. Called OFF e->lock with e->encoding set.
static OSStatus encodeTileSync(Encoder *e, CVPixelBufferRef source, const AVERegion *r, uint8_t qp,
                               CMSampleBufferRef *out)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t began = mach_absolute_time();
#endif
    TegraNVENCSubmit *submit = nvenc_submit_buffer(e->client);
    if (!submit)
        return kVTAllocationFailedErr;
    const HEVCEncPicture idr = {.idr = true, .poc = 0, .qp = qp};
    hevcnvenc_fill_submit(&e->hevc, e->config.input_layout, e->info.chroma_mode, &e->layout, &idr, &e->handles, submit);
    bool inputRefused = false;
    const bool putOk = ave_input_put_buffer(e, source, r, e->handles.input, e->inputVA, submit, &inputRefused);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t copied = began;
#endif
    int submitted = -1;
    uint32_t fence = 0, route = TEGRA_NVENC_INPUT_CPU;
    AVEJobStatus job = {0};
    bool waitedOk = false;
    if (putOk) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        copied = mach_absolute_time();
#endif
        memset(e->auxVA, 0, sizeof(nvenc_pic_stat_s));
        submit->clean[submit->clean_count++] = (TegraNVENCRange){e->handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
        submitted = ave_input_submit_raw(e->client, submit, &fence, &route);
        if (!submitted)
            waitedOk =
                ave_wait_job(e, fence, e->handles.aux, e->auxVA, e->handles.bitstream, e->layout.bitstream_bytes, &job);
    }
    const uint32_t produced = (job.total_bit_count + 7) / 8;
    OSStatus status = kVTVideoEncoderMalfunctionErr;
    size_t size = 0;
    if (waitedOk) {
        if (job.ucode_error_status || !produced || produced > e->layout.bitstream_bytes) {
            static int logged;
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                os_log_error(gLog, "AVE: NVENC tile picture failed: error_status %u ucode %#x, %u bits",
                             job.error_status, job.ucode_error_status, job.total_bit_count);
        } else {
            size = h264enc_annexb_to_avcc(e->bitstreamVA, produced, e->avcc, e->avccCap);
            if (size)
                status = createSample(e, size, out);
        }
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t finished = mach_absolute_time();
#endif
    pthread_mutex_lock(&e->lock);
    if (inputRefused)
        e->inputRefused++;
    if (putOk)
        ave_input_record_route(e, submit, submitted, route);
    if (status) {
        e->errors++;
    } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        e->copyTicks += copied - began;
        e->encodeTicks += finished - copied;
#endif
        e->frames++;
        e->keyFrames++;
        e->tiles++;
        e->bytes += size;
    }
    pthread_mutex_unlock(&e->lock);
    return status;
}

static OSStatus submitTileAsync(Encoder *e, AVEEmitter *emitter, VTEncoderTile tile, CVPixelBufferRef source,
                                const AVERegion *r, uint8_t qp, VTEncodeInfoFlags *info)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t began = mach_absolute_time();
#else
    const uint64_t began = 0;
#endif
    const int slotIndex = ave_emitter_acquire_tile_slot(emitter);
    if (slotIndex < 0)
        return noErr;
    TegraNVENCSubmit *submit = nvenc_tile_submit_buffer(e->client, (uint32_t)slotIndex);
    if (!submit) {
        ave_emitter_release_tile_slot(emitter, slotIndex);
        ave_tile_record_stats(e, kVTAllocationFailedErr, 0, 0, 0);
        ave_emitter_push_tile(emitter, tile, NULL, -1, 0, 0, 0, kVTAllocationFailedErr);
        return noErr;
    }
    const AVETileSlot *slot = &e->tileSlots[slotIndex];
    H264EncHandles handles = e->handles;
    handles.input = slot->inputHandle;
    handles.bitstream = slot->bitstreamHandle;
    handles.aux = slot->auxHandle;
    const HEVCEncPicture idr = {.idr = true, .poc = 0, .qp = qp};
    hevcnvenc_fill_submit(&e->hevc, e->config.input_layout, e->info.chroma_mode, &e->layout, &idr, &handles, submit);
    bool inputRefused = false;
    const bool putOk = ave_input_put_buffer(e, source, r, slot->inputHandle, slot->inputVA, submit, &inputRefused);
    uint64_t copied = began;
    int submitted = -1;
    uint32_t fence = 0, route = TEGRA_NVENC_INPUT_CPU;
    if (putOk) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        copied = mach_absolute_time();
#endif
        memset(slot->auxVA, 0, sizeof(nvenc_pic_stat_s));
        submit->clean[submit->clean_count++] = (TegraNVENCRange){slot->auxHandle, 0, sizeof(nvenc_pic_stat_s), 0};
        CVPixelBufferRetain(source);
        submitted = ave_input_submit_raw(e->client, submit, &fence, &route);
        if (submitted)
            CVPixelBufferRelease(source);
    }
    pthread_mutex_lock(&e->lock);
    if (inputRefused)
        e->inputRefused++;
    if (putOk)
        ave_input_record_route(e, submit, submitted, route);
    if (!putOk || submitted)
        e->errors++;
    pthread_mutex_unlock(&e->lock);
    if (!putOk || submitted) {
        ave_emitter_release_tile_slot(emitter, slotIndex);
        ave_emitter_push_tile(emitter, tile, NULL, -1, 0, 0, 0, kVTVideoEncoderMalfunctionErr);
        return noErr;
    }
    if (info)
        *info = kVTEncodeInfo_Asynchronous;
    ave_emitter_push_tile(emitter, tile, source, slotIndex, fence, began, copied, noErr);
    return noErr;
}

OSStatus ave_encode_tile(VTVideoEncoderRef ref, VTEncoderTile tile, CVImageBufferRef source, VTInt32Point origin,
                         VTInt32Size size, CFDictionaryRef options, VTEncodeInfoFlags *info)
{
    (void)options;
    Encoder *e = CMBaseObjectGetDerivedStorage(ref);
    if (info)
        *info = 0;
    pthread_mutex_lock(&e->lock);
    AVERegion region;
    OSStatus status = e->failed ? kVTVideoEncoderMalfunctionErr : tileRegion(e, source, origin, size, &region);
    if (!status && !e->configured) {
        ave_begin_encode(e);
        if (!e->failed && !e->configured && (status = ave_configure(e, source))) {
            e->failed = true;
            os_log_error(gLog, "AVE: tile session %dx%d in %{public}s could not start on NVENC (%d)", e->size.width,
                         e->size.height, getprogname(), (int)status);
        } else if (e->failed) {
            status = kVTVideoEncoderMalfunctionErr;
        }
        ave_end_encode(e);
    }
    AVEEmitter *emitter = e->emitter;
    const uint32_t tileSlots = e->tileSlotCount;
    const uint8_t qp = tileQP(e);
    VTTileEncoderSession session = e->tileSession;
    if (emitter && tileSlots > 0) {
        if (status)
            e->errors++;
        pthread_mutex_unlock(&e->lock);
        if (status) {
            ave_emitter_push_tile(emitter, tile, NULL, -1, 0, 0, 0, status);
            return noErr;
        }
        return submitTileAsync(e, emitter, tile, source, &region, qp, info);
    }
    if (status) {
        pthread_mutex_unlock(&e->lock);
        if (session)
            VTEncoderSessionEmitEncodedTile(session, tile, status, kVTEncodeInfo_FrameDropped, NULL);
        return noErr;
    }
    ave_begin_encode(e);
    pthread_mutex_unlock(&e->lock);
    CMSampleBufferRef sample = NULL;
    status = encodeTileSync(e, source, &region, qp, &sample);
    pthread_mutex_lock(&e->lock);
    session = e->tileSession;
    ave_end_encode(e);
    pthread_mutex_unlock(&e->lock);
    if (session)
        VTEncoderSessionEmitEncodedTile(session, tile, status, status ? kVTEncodeInfo_FrameDropped : 0, sample);
    if (sample)
        CFRelease(sample);
    return noErr;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
CFDictionaryRef ave_copy_routes(const Encoder *e)
{
    const int32_t counts[] = {(int32_t)e->frames,       (int32_t)e->tiles,      (int32_t)e->errors,
                              (int32_t)e->inputCopies,  (int32_t)e->inputVIC,   (int32_t)e->inputBounced,
                              (int32_t)e->inputRefused, (int32_t)e->inputDirect};
    const void *keys[] = {CFSTR("pictures"),  CFSTR("tiles"),         CFSTR("errors"),        CFSTR("input_cpu_copies"),
                          CFSTR("input_vic"), CFSTR("input_bounced"), CFSTR("input_refused"), CFSTR("input_direct")};

    enum {
        COUNT = sizeof(counts) / sizeof(counts[0])
    };

    const void *values[COUNT];
    for (int i = 0; i < COUNT; i++)
        values[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &counts[i]);
    CFDictionaryRef routes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, COUNT,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < COUNT; i++)
        CFRelease(values[i]);
    return routes;
}
#endif
