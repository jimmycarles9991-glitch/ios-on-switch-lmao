// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC tile sessions on NVDEC: grid HEIC (iPhone photos at full size, the system wallpapers), which ImageIO hands
// CMPhoto and CMPhoto decodes through VTTileDecompressionSession, one tile picture (512x512 in practice) at a time into
// one canvas pixel buffer it allocated from the requirements below. Each tile is one NVDEC job; VIC writes the picture
// straight into its place in the canvas (TegraNVDECPresent with dst_x/dst_y), mapping only the rows it covers.
//
// The contract is that of Apple's A10X decoder (H264H8.videodecoder AppleD5500WrapperHEVCDecoderStartTileSession /
// DecodeTile, 21H461): requirements set once at the start, the destination and offset of each tile taken from
// DecodeTile, each tile answered with VTDecoderSessionEmitDecodedTile before DecodeTile returns: unlike a frame, a tile
// waits for its job, whose status its answer carries. What NVDEC and VIC
// cannot take is refused at the start (sizes outside NVDEC's HEVC range, VIC's present off, Main 10 on a kernel without
// it), and VideoToolbox then takes the next decoder. Main 10 tiles go into an 'x420'/'xf20' canvas: VIC places their
// 16-bit samples as bytes of a row twice as wide, so such a canvas is at most half VIC's widest surface.
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include "AVDDecoder.h"

enum {
    kTileFloor = 144,     // NVDEC's smallest HEVC picture
    kTileCeiling = 4096,  // its largest
    kCanvasSide = 8192,   // VIC's largest surface
    kRowAlignment = 256,  // VIC's pitch and plane alignment
    kCanvasRowBytes = 16384,
};

static CFDictionaryRef copyCanvasAttributes(bool fullRange, bool tenBit)
{
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    const OSType format = tenBit ? (fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                              : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                                 : (fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                              : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    avd_set_number(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)format);
    avd_set_number(attributes, kCVPixelBufferBytesPerRowAlignmentKey, kRowAlignment);
    avd_set_number(attributes, kCVPixelBufferPlaneAlignmentKey, kRowAlignment);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    return attributes;
}

// A tile's left edge is where VIC's plane base goes: its byte offset in a row must be a multiple of VIC's alignment.
static CFDictionaryRef copyTileRequirements(bool tenBit)
{
    CFMutableDictionaryRef requirements = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    avd_set_number(requirements, kVTTileDecoderRequirement_TileByteAlignment, kRowAlignment);
    avd_set_number(requirements, kVTTileDecoderRequirement_MinimumTileWidth, kTileFloor);
    avd_set_number(requirements, kVTTileDecoderRequirement_MinimumTileHeight, kTileFloor);
    avd_set_number(requirements, kVTTileDecoderRequirement_MaximumTileWidth, kTileCeiling);
    avd_set_number(requirements, kVTTileDecoderRequirement_MaximumTileHeight, kTileCeiling);
    avd_set_number(requirements, kVTTileDecoderRequirement_TileWidthPixelAlignment, 2);
    avd_set_number(requirements, kVTTileDecoderRequirement_TileHeightPixelAlignment, 2);
    avd_set_number(requirements, kVTTileDecoderRequirement_MaximumCanvasWidth, tenBit ? kCanvasSide / 2 : kCanvasSide);
    avd_set_number(requirements, kVTTileDecoderRequirement_MaximumCanvasHeight, kCanvasSide);
    avd_set_number(requirements, kVTTileDecoderRequirement_MaximumCanvasRowBytes, kCanvasRowBytes);
    avd_set_number(requirements, kVTTileDecoderRequirement_MinimumDecodableWidth, kTileFloor);
    avd_set_number(requirements, kVTTileDecoderRequirement_MinimumDecodableHeight, kTileFloor);
    CFDictionarySetValue(requirements, kVTTileDecoderRequirement_DecoderCanClipEdges, kCFBooleanTrue);
    return requirements;
}

// Buffers for the tile size, or why NVDEC and VIC cannot take this session.
static const char *prepare(Decoder *d, const HEVCSPS *sps)
{
    if (!d->present)
        return "VIC cannot place pictures this boot (TegraNVDECInfo.features)";
    if (!sps)
        return "not Main or Main Still Picture 4:2:0";
    if (sps->bit_depth == 10 && !(nvdec_codecs(d->client) & TEGRA_NVDEC_HEVC_MAIN10))
        return "Main 10, which this kernel's NVDEC does not decode";
    if (sps->width > kTileCeiling || sps->height > kTileCeiling || sps->width < kTileFloor || sps->height < kTileFloor)
        return "tile size outside NVDEC's HEVC range";
    if (hevc_decoder_configure(d->hevc))
        return "no NVDEC memory for the tile's buffers";
    return NULL;
}

OSStatus avd_start_tile_session(VTVideoDecoderRef ref, VTTileDecoderSession session, CMVideoFormatDescriptionRef format)
{
    Decoder *d = CMBaseObjectGetDerivedStorage(ref);
    if (!d->hevc)
        return kVTVideoDecoderUnsupportedDataFormatErr;
    pthread_mutex_lock(&d->lock);
    if (d->avcC)
        CFRelease(d->avcC);
    d->avcC = avd_copy_atom(format, CFSTR("hvcC"));
    const HEVCSPS *sps = d->avcC && !avd_load_hvcc(d, d->avcC) ? hevc_decoder_sps(d->hevc) : NULL;
    const char *why = prepare(d, sps);
    if (why) {
        pthread_mutex_unlock(&d->lock);
        switch_refusal("AVD", why, "tile session left to the next decoder");
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    const bool tenBit = sps->bit_depth == 10;
    hevc_decoder_allow_10bit(d->hevc, tenBit);
    d->tileSession = session;
    d->width = (uint32_t)(sps->width - sps->crop_left - sps->crop_right);
    d->height = (uint32_t)(sps->height - sps->crop_top - sps->crop_bottom);
    CFDictionaryRef attributes = copyCanvasAttributes(sps->full_range, tenBit),
                    requirements = copyTileRequirements(tenBit);
    const OSStatus status = VTTileDecoderSessionSetTileDecodeRequirements(session, attributes, requirements);
    CFRelease(attributes);
    CFRelease(requirements);
    pthread_mutex_unlock(&d->lock);
    if (status)
        os_log_error(gAVDLog, "AVD: tile requirements refused in %{public}s: %d", getprogname(), (int)status);
    else
        os_log(gAVDLog, "AVD: tile session %ux%u tiles in %{public}s on NVDEC, placed by VIC", d->width, d->height,
               getprogname());
    return status;
}

void *avd_tile_decoded(Decoder *d, const H264Picture *picture)
{
    Output *out = avd_emitter_output(d->emitter, picture->token, picture->job, picture->first_field_job);
    if (!out)
        return NULL;
    if (d->tile.canvas)
        avd_place_picture(d, out, picture, d->tile.canvas, d->tile.x, d->tile.y);
    else
        avd_emitter_fail(d->emitter, out, kVTVideoDecoderMalfunctionErr);
    return out;
}

void avd_tile_emit(Decoder *d, Output *out)
{
    avd_emitter_settle_output(d->emitter, out);
    VTDecoderSessionEmitDecodedTile(d->tileSession, (int64_t)(intptr_t)out->frame, out->status, 0, NULL);
    d->tile.answered = true;
    free(out);
}

OSStatus avd_decode_tile(VTVideoDecoderRef ref, int64_t frameID, CMSampleBufferRef sample, VTInt32Point tile,
                         VTInt32Size size, CVPixelBufferRef canvas, VTInt32Point offset, VTDecodeFrameFlags flags,
                         VTDecodeInfoFlags *info)
{
    (void)tile, (void)size, (void)flags;
    Decoder *d = CMBaseObjectGetDerivedStorage(ref);
    if (info)
        *info = 0;
    const uint8_t *data = NULL;
    uint8_t *copy = NULL;
    size_t bytes = 0;
    const bool readable = avd_sample_bytes(sample, &data, &bytes, &copy);
    pthread_mutex_lock(&d->lock);
    d->tile =
        (AVDTile){frameID, canvas, offset.x < 0 ? 0 : (uint32_t)offset.x, offset.y < 0 ? 0 : (uint32_t)offset.y, false};
    const uint64_t began = mach_absolute_time(), placed = d->copyTicks;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    avd_frame_begin(&d->timing, began);
#endif
    const int ret = readable && offset.x >= 0 && offset.y >= 0
                        ? hevc_decoder_decode(d->hevc, data, bytes, d->lengthSize, (void *)(intptr_t)frameID, true)
                        : -1;
    if (ret == 1) {
        hevc_decoder_flush(d->hevc);
        d->frames++;
        d->decodeTicks += mach_absolute_time() - began - (d->copyTicks - placed);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        avd_frame_end(&d->timing, mach_absolute_time());
#endif
    }
    if (!d->tile.answered) {
        const OSStatus status = ret <= -2 ? kVTVideoDecoderMalfunctionErr : ret < 0 ? kVTVideoDecoderBadDataErr : noErr;
        VTDecoderSessionEmitDecodedTile(d->tileSession, frameID, status, kVTDecodeInfo_FrameDropped, NULL);
    }
    d->tile.canvas = NULL;
    pthread_mutex_unlock(&d->lock);
    free(copy);
    return noErr;
}
