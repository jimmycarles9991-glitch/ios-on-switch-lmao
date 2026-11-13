// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC tile sessions on NVENC: HEIC grids. ImageIO hands an image to CMPhoto, which opens a VTTileCompressionSession
// ('hvc1', 512x512 tiles unless the image is smaller) and encodes the picture a tile at a time; HEIF keeps each tile
// as an item of its own in a grid, and a grid's tiles are all one size. So every tile here is one NVENC IDR picture of
// the session's tile size: the tile's region of the source copied into the input buffer, edge-replicated where the
// source ends, and answered with VTEncoderSessionEmitEncodedTile before EncodeTile returns. The source is CMPhoto's
// whole picture with the tile's origin, or its own copy of the tile at (0, 0) (VTTileCompressionPluginClass_encode,
// 24B119 0x1a6bb353c).
//
// The contract is that of Apple's AVE HEVC encoder (ave.videoencoder, 24B119): AVE_Session_HEVC_StartTileSession
// 0x2c4617800 states the size it codes tiles at (never under its smallest picture) with the source pixel buffer
// attributes (VTTileEncoderSessionSetTileAttributes 0x2c4617e34), then the same attributes for the canvas and the tile
// requirements: origins on even samples both ways (0x2c46180a0..0x2c4618100), and source padding, which AVE states
// on its newer devices (0x2c4618110). Without it CMPhoto copies every tile that runs past the picture into a padded
// buffer of its own first (__needsPixelBufferCopy 0x1a6bb59fc: EncoderSupportsSourcePadding, read from +0x40 of its
// state, 0x1a6bb58a4); with it the tile comes as the region the picture covers. VIC reads that region straight from
// CMPhoto's picture (AVEInput.c); the coded tile past it is what the buffer held, which the grid crops away.
// One hvcC serves every tile.
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
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
    (void)ref;
    return noErr;  // every tile is out before EncodeTile returns
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

// One IDR picture on NVENC from the tile's region of `source`; its length-prefixed NAL units land in e->avcc. 0, or why
// not.
static OSStatus codeTile(Encoder *e, CVPixelBufferRef source, const AVERegion *r, size_t *size, uint64_t *put)
{
    TegraNVENCSubmit *submit = malloc(sizeof(*submit));
    if (!submit)
        return kVTAllocationFailedErr;
    const HEVCEncPicture idr = {.idr = true, .poc = 0, .qp = tileQP(e)};
    hevcnvenc_fill_submit(&e->hevc, e->config.input_layout, e->info.chroma_mode, &e->layout, &idr, &e->handles, submit);
    if (!ave_input_put(e, source, r, submit)) {
        free(submit);
        return kVTVideoEncoderMalfunctionErr;
    }
    *put = mach_absolute_time();
    // A job that ends without writing its record must not leave the previous tile's record to be read as its own.
    memset(e->auxVA, 0, sizeof(nvenc_pic_stat_s));
    submit->clean[submit->clean_count++] = (TegraNVENCRange){e->handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
    TegraNVENCWait wait = {.timeout_us = 1000000, .invalidate_count = 1};
    wait.invalidate[0] = (TegraNVENCRange){e->handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
    const int submitted = ave_input_submit(e, submit, &wait.fence);
    free(submit);
    if (submitted || nvenc_wait(e->client, &wait))
        return kVTVideoEncoderMalfunctionErr;
    const nvenc_pic_stat_s *stat = (const nvenc_pic_stat_s *)e->auxVA;
    const uint32_t produced = (stat->total_bit_count + 7) / 8;
    // error_status is 2 in every record NVENC 5.0's H.265 app writes; its errors are ucode_error_status (AVEEncoder.c).
    if (stat->ucode_error_status || !produced || produced > e->layout.bitstream_bytes) {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gLog, "AVE: NVENC tile picture failed: error_status %u ucode %#x, %u bits", stat->error_status,
                         stat->ucode_error_status, stat->total_bit_count);
        return kVTVideoEncoderMalfunctionErr;
    }
    TegraNVENCWait bits = {.fence = wait.fence, .timeout_us = 1000000, .invalidate_count = 1};
    bits.invalidate[0] = (TegraNVENCRange){e->handles.bitstream, 0, produced, 0};
    if (nvenc_wait(e->client, &bits))
        return kVTVideoEncoderMalfunctionErr;
    *size = h264enc_annexb_to_avcc(e->bitstreamVA, produced, e->avcc, e->avccCap);
    return *size ? noErr : kVTVideoEncoderMalfunctionErr;
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

static OSStatus encodeTile(Encoder *e, CVPixelBufferRef source, const AVERegion *r, CMSampleBufferRef *out)
{
    const uint64_t began = mach_absolute_time();
    uint64_t copied = began;
    size_t size = 0;
    const OSStatus status = codeTile(e, source, r, &size, &copied);
    if (status) {
        e->errors++;
        return status;
    }
    e->copyTicks += copied - began;
    e->encodeTicks += mach_absolute_time() - copied;
    e->frames++;
    e->keyFrames++;
    e->tiles++;
    e->bytes += size;
    return createSample(e, size, out);
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
    if (!status && !e->configured && (status = ave_configure(e, source))) {
        e->failed = true;
        os_log_error(gLog, "AVE: tile session %dx%d in %{public}s could not start on NVENC (%d)", e->size.width,
                     e->size.height, getprogname(), (int)status);
    }
    CMSampleBufferRef sample = NULL;
    if (!status)
        status = encodeTile(e, source, &region, &sample);
    VTTileEncoderSession session = e->tileSession;
    pthread_mutex_unlock(&e->lock);
    // Outside the lock: VideoToolbox runs the client's output inside this call (0x18f20a7e4), and it may call back in.
    if (session)
        VTEncoderSessionEmitEncodedTile(session, tile, status, status ? kVTEncodeInfo_FrameDropped : 0, sample);
    if (sample)
        CFRelease(sample);
    return noErr;  // the tile was answered (with its status) above
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
