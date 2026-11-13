// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TileCheck.h. The client side declares VideoToolbox's tile session SPI as CMPhoto calls it (24B119
// VTTileCompressionPluginClass_prepare 0x1a6bb0c54 and _encode 0x1a6bb353c); the output's arguments are the ones
// VTEncoderSessionEmitEncodedTile passes (0x18f20a7e4: source refcon, tile origin, tile size, status, flags, sample).
#include "TileCheck.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AVE/H264NVENC.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../../TegraNVENC/nvidia/clc5b7.h"
#include "../../TegraNVENC/TegraNVENCCheckCommon.h"

typedef struct OpaqueVTTileCompressionSession *VTTileCompressionSessionRef;
typedef void (*TileOutput)(void *refcon, void *sourceRefCon, VTInt32Point origin, VTInt32Size size, OSStatus status,
                           VTEncodeInfoFlags flags, CMSampleBufferRef sample);
extern OSStatus VTTileCompressionSessionCreate(CFAllocatorRef allocator, VTInt32Size tileSize, CMVideoCodecType codec,
                                               CFDictionaryRef encoderSpecification, CFDictionaryRef sourceAttributes,
                                               CFAllocatorRef compressedDataAllocator, TileOutput output, void *refcon,
                                               VTTileCompressionSessionRef *out);
extern OSStatus VTTileCompressionSessionPrepareToEncodeTiles(VTTileCompressionSessionRef session, Boolean unused,
                                                             VTInt32Size *tileSize);
extern OSStatus VTTileCompressionSessionEncodeTile(VTTileCompressionSessionRef session, CVImageBufferRef source,
                                                   VTInt32Point origin, VTInt32Size size, CFDictionaryRef options,
                                                   void *sourceRefCon, VTEncodeInfoFlags *info);
extern OSStatus VTTileCompressionSessionCompleteTiles(VTTileCompressionSessionRef session);
extern OSStatus VTTileCompressionSessionCopyProperty(VTTileCompressionSessionRef session, CFStringRef key,
                                                     CFAllocatorRef allocator, void *value);
extern void VTTileCompressionSessionInvalidate(VTTileCompressionSessionRef session);
extern const CFStringRef kVTCompressionPropertyKey_TileEncoderRequirements;
extern const CFStringRef kVTTileEncoderRequirement_HorizontalOffsetAlignment;
extern const CFStringRef kVTTileEncoderRequirement_EncoderSupportsSourcePadding;

enum {
    W = 960,  // rows of 960 bytes are off VIC's 256-byte grid at 64-byte alignment (tile_check_row_alignment)
    H = 700,
    TILE = 512,
    TILES = 4,
    COPIED = 3,  // this tile comes as its own TILE x TILE copy at (0, 0)
};

static uint8_t *gInputs[TILES];  // what each H.265 job read, raster: TILE x TILE luma, then TILE x TILE/2 CbCr
static bool gSourced[TILES];     // the job named a surface: only its region was written (TegraNVENCSource)
static int gJobs;

static uint32_t layoutOf(const nvenc_h265_surface_cfg_s *cfg)
{
    if (cfg->tiled_16x16)
        return TEGRA_NVENC_LAYOUT_TILED16;
    for (uint32_t layout = 0; layout < TEGRA_NVENC_LAYOUTS; layout++)
        if (layout != TEGRA_NVENC_LAYOUT_TILED16 && tegra_nvenc_layout_gobs(layout) == cfg->block_height)
            return layout;
    return TEGRA_NVENC_LAYOUTS;
}

static const TegraNVENCEntry *find(const TegraNVENCSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].method == method)
            return &s->entries[i];
    return NULL;
}

int tile_check_engine(const TegraNVENCSubmit *s, uint8_t *const *buffers, uint32_t chromaMode)
{
    const nvenc_h265_drv_pic_setup_s *p = (const nvenc_h265_drv_pic_setup_s *)s->setup;
    const TegraNVENCEntry *in = find(s, NVC5B7_SET_IN_CUR_PIC), *inChroma = find(s, NVC5B7_SET_IN_CUR_PIC_CHROMA_U);
    const TegraNVENCEntry *bits = find(s, NVC5B7_SET_OUT_BITSTREAM), *status = find(s, NVC5B7_SET_OUT_ENC_STATUS);
    const uint32_t width = p->input_cfg.frame_width_minus1 + 1u, height = p->input_cfg.frame_height_minus1 + 1u;
    if (!in || !inChroma || !bits || !status || gJobs >= TILES || width != TILE || height != TILE)
        return -1;
    const uint8_t *input = buffers[in->value] + in->offset;
    const uint32_t chroma = chromaMode == TEGRA_NVENC_CHROMA_OFFSET ? p->input_cfg.chroma_top_frm_offset * 256
                                                                    : inChroma->offset - in->offset;
    const uint32_t layout = layoutOf(&p->input_cfg), pitch = p->input_cfg.sfc_pitch;
    uint8_t *raster = malloc(TILE * TILE * 3 / 2);
    for (uint32_t y = 0; y < TILE; y++)
        for (uint32_t x = 0; x < TILE; x++) {
            raster[y * TILE + x] = input[h264enc_surface_offset(layout, pitch, x, y)];
            if (y < TILE / 2)
                raster[TILE * TILE + y * TILE + x] = input[chroma + h264enc_surface_offset(layout, pitch, x, y)];
        }
    gInputs[gJobs] = raster;
    gSourced[gJobs] = s->source.surface_id != 0;
    const uint8_t slice[] = {0, 0, 0, 1, 0x26, 0x01, 0xaf, (uint8_t)gJobs, 0x80};  // IDR_W_RADL, placeholder data
    memcpy(buffers[bits->value] + bits->offset, slice, sizeof(slice));
    nvenc_pic_stat_s *stat = (nvenc_pic_stat_s *)(buffers[status->value] + status->offset);
    memset(stat, 0, sizeof(*stat));
    stat->error_status = 2;
    stat->total_bit_count = sizeof(slice) * 8;
    gJobs++;
    return 0;
}

static uint8_t lumaAt(uint32_t x, uint32_t y)
{
    return (uint8_t)(x * 3 + y * 5);
}

static uint8_t chromaAt(uint32_t pair, uint32_t row, uint32_t cr)
{
    return (uint8_t)(cr ? pair + row * 11 + 64 : pair * 7 + row + 128);
}

uint32_t tile_check_row_alignment = 256;

static CVPixelBufferRef makeSource(uint32_t width, uint32_t height)
{
    CFDictionaryRef surface =
        CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const int32_t rowAlignment = (int32_t)tile_check_row_alignment;
    CFNumberRef alignment = CFNumberCreate(NULL, kCFNumberSInt32Type, &rowAlignment);
    const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferBytesPerRowAlignmentKey};
    const void *values[] = {surface, alignment};
    CFDictionaryRef attributes =
        CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(alignment);
    CVPixelBufferRef buffer = NULL;
    CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &buffer);
    CFRelease(attributes);
    CFRelease(surface);
    return buffer;
}

// The picture at (x0, y0) of the W x H source, `width` x `height` of it, into `buffer` at its top left.
static void fill(CVPixelBufferRef buffer, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height)
{
    CVPixelBufferLockBaseAddress(buffer, 0);
    uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0),
            *chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            luma[y * lumaStride + x] = lumaAt(x0 + x, y0 + y);
            chroma[y / 2 * chromaStride + x] = chromaAt((x0 + x) / 2, (y0 + y) / 2, x & 1);
        }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
}

// The input tile `i` should have read: its region of the source (the copy's own TILE x TILE for COPIED, whose
// samples past the source are the copy's), the last row and column repeated past the region where the CPU copied it.
static bool inputMatches(int i, const uint8_t *raster)
{
    const uint32_t x0 = (uint32_t)(i & 1) * TILE, y0 = (uint32_t)(i >> 1) * TILE;
    const uint32_t width = i == COPIED ? TILE : (W - x0 < TILE ? W - x0 : TILE);
    const uint32_t height = i == COPIED ? TILE : (H - y0 < TILE ? H - y0 : TILE);
    for (uint32_t y = 0; y < (gSourced[i] ? height : TILE); y++)
        for (uint32_t x = 0; x < (gSourced[i] ? width : TILE); x++) {
            const uint32_t sx = x < width ? x : width - 1, sy = y < height ? y : height - 1;
            if (raster[y * TILE + x] != lumaAt(x0 + sx, y0 + sy))
                return false;
            const uint32_t cx = x / 2 < (width + 1) / 2 ? x / 2 : (width + 1) / 2 - 1;
            const uint32_t cy = y < (height + 1) / 2 ? y : (height + 1) / 2 - 1;
            const uint32_t chromaRows = gSourced[i] ? (height + 1) / 2 : TILE / 2;
            if (y < chromaRows && raster[TILE * TILE + y * TILE + x] != chromaAt(x0 / 2 + cx, y0 / 2 + cy, x & 1))
                return false;
        }
    return true;
}

typedef struct {
    int count, failed;
    CMSampleBufferRef samples[TILES];
    VTInt32Point origins[TILES];
    VTInt32Size sizes[TILES];
} Outputs;

static void output(void *refcon, void *sourceRefCon, VTInt32Point origin, VTInt32Size size, OSStatus status,
                   VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    Outputs *o = refcon;
    const intptr_t i = (intptr_t)sourceRefCon;
    if (status || (flags & kVTEncodeInfo_FrameDropped) || !sample || i < 0 || i >= TILES || o->samples[i]) {
        o->failed++;
        return;
    }
    o->samples[i] = (CMSampleBufferRef)CFRetain(sample);
    o->origins[i] = origin;
    o->sizes[i] = size;
    o->count++;
}

static int32_t numberIn(CFTypeRef dict, CFStringRef key)
{
    int32_t v = -1;
    CFTypeRef n = dict && CFGetTypeID(dict) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(dict, key) : NULL;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberSInt32Type, &v);
    return v;
}

static CFTypeRef copyProperty(VTTileCompressionSessionRef session, CFStringRef key)
{
    CFTypeRef value = NULL;
    return VTTileCompressionSessionCopyProperty(session, key, NULL, &value) ? NULL : value;
}

static bool setProperties(VTTileCompressionSessionRef session)
{
    const int32_t one = 1, frames = TILES;
    const double quality = 0.9;
    CFNumberRef oneN = CFNumberCreate(NULL, kCFNumberSInt32Type, &one),
                framesN = CFNumberCreate(NULL, kCFNumberSInt32Type, &frames);
    CFNumberRef qualityN = CFNumberCreate(NULL, kCFNumberDoubleType, &quality);
    // CMPhoto's batch (VTTileCompressionPluginClass_prepare 0x1a6bb0c64..0x1a6bb0d64), and a key no encoder lists,
    // which VideoToolbox must leave out rather than fail the batch.
    const void *keys[] = {kVTCompressionPropertyKey_AllowTemporalCompression,
                          kVTCompressionPropertyKey_AllowFrameReordering,
                          kVTCompressionPropertyKey_MaxKeyFrameInterval,
                          kVTCompressionPropertyKey_SourceFrameCount,
                          kVTCompressionPropertyKey_ProfileLevel,
                          kVTCompressionPropertyKey_Quality,
                          kVTCompressionPropertyKey_RealTime,
                          kVTCompressionPropertyKey_ColorPrimaries,
                          CFSTR("SwitchNoEncoderListsThis")};
    const void *values[] = {kCFBooleanFalse,
                            kCFBooleanFalse,
                            oneN,
                            framesN,
                            CFSTR("HEVC_MainStill_AutoLevel"),
                            qualityN,
                            kCFBooleanFalse,
                            kCVImageBufferColorPrimaries_ITU_R_709_2,
                            kCFBooleanTrue};
    CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, values, sizeof(keys) / sizeof(keys[0]),
                                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = VTSessionSetProperties(session, properties);
    CFRelease(properties);
    CFRelease(oneN);
    CFRelease(framesN);
    CFRelease(qualityN);
    return status == noErr;
}

static VTInt32Point originOf(int i)
{
    return i == COPIED ? (VTInt32Point){0, 0} : (VTInt32Point){(i & 1) * TILE, (i >> 1) * TILE};
}

static VTInt32Size sizeOf(int i)
{
    const VTInt32Point at = originOf(i);
    return i == COPIED ? (VTInt32Size){TILE, TILE}
                       : (VTInt32Size){W - at.x < TILE ? W - at.x : TILE, H - at.y < TILE ? H - at.y : TILE};
}

static int encodeTiles(VTTileCompressionSessionRef session, CVPixelBufferRef source, CVPixelBufferRef copy)
{
    int failed = 0;
    for (int i = 0; i < TILES; i++)
        failed += VTTileCompressionSessionEncodeTile(session, i == COPIED ? copy : source, originOf(i), sizeOf(i), NULL,
                                                     (void *)(intptr_t)i, NULL) != noErr;
    return failed + (VTTileCompressionSessionCompleteTiles(session) != noErr);
}

// Every sample: the tile's own origin and size back, one sync sample of the 4-byte-length placeholder slice, the
// session's one format description, the tile size, three parameter sets in its hvcC.
static int badSamples(const Outputs *o)
{
    int bad = 0;
    for (int i = 0; i < TILES; i++) {
        CMSampleBufferRef s = o->samples[i];
        if (!s || o->origins[i].x != originOf(i).x || o->origins[i].y != originOf(i).y ||
            o->sizes[i].width != sizeOf(i).width || o->sizes[i].height != sizeOf(i).height) {
            bad++;
            continue;
        }
        CMVideoFormatDescriptionRef format = CMSampleBufferGetFormatDescription(s);
        const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(format);
        size_t sets = 0;
        int nalLength = 0;
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, NULL, NULL, &sets, &nalLength);
        uint8_t bytes[9] = {0};
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(s);
        const bool data = CMSampleBufferGetTotalSampleSize(s) == 9 && block &&
                          !CMBlockBufferCopyDataBytes(block, 0, 9, bytes) && bytes[3] == 5 && bytes[4] == 0x26 &&
                          bytes[7] == (uint8_t)i;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(s, false);
        const bool notSync = attachments && CFArrayGetCount(attachments) &&
                             CFDictionaryGetValue(CFArrayGetValueAtIndex(attachments, 0),
                                                  kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue;
        bad += d.width != TILE || d.height != TILE || sets != 3 || nalLength != 4 || !data || notSync ||
               !CFEqual(format, CMSampleBufferGetFormatDescription(o->samples[0]));
    }
    return bad;
}

bool tile_check(CFStringRef encoderID, char *line, size_t size)
{
    gJobs = 0;
    const void *specKeys[] = {kVTVideoEncoderSpecification_EncoderID};
    const void *specValues[] = {encoderID};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    Outputs outputs = {0};
    VTTileCompressionSessionRef session = NULL;
    OSStatus status = VTTileCompressionSessionCreate(NULL, (VTInt32Size){TILE, TILE}, kCMVideoCodecType_HEVC, spec,
                                                     NULL, NULL, output, &outputs, &session);
    CFRelease(spec);
    if (status || !session) {
        snprintf(line, size, "VTTileCompressionSessionCreate %d", (int)status);
        return false;
    }
    const bool propertiesSet = setProperties(session);
    CFTypeRef attributes = copyProperty(session, kVTCompressionPropertyKey_VideoEncoderPixelBufferAttributes);
    CFTypeRef requirements = copyProperty(session, kVTCompressionPropertyKey_TileEncoderRequirements);
    const bool hasFormats = attributes && CFGetTypeID(attributes) == CFDictionaryGetTypeID() &&
                            CFDictionaryContainsKey(attributes, kCVPixelBufferPixelFormatTypeKey);
    const int32_t alignment = numberIn(requirements, kVTTileEncoderRequirement_HorizontalOffsetAlignment);
    const bool padding =
        requirements && CFGetTypeID(requirements) == CFDictionaryGetTypeID() &&
        CFDictionaryGetValue(requirements, kVTTileEncoderRequirement_EncoderSupportsSourcePadding) == kCFBooleanTrue;
    VTInt32Size prepared = {0, 0};
    VTTileCompressionSessionPrepareToEncodeTiles(session, false, &prepared);

    CVPixelBufferRef source = makeSource(W, H), copy = makeSource(TILE, TILE);
    fill(source, 0, 0, W, H);
    fill(copy, TILE, TILE, TILE, TILE);
    const int refused = encodeTiles(session, source, copy);

    CFTypeRef routes = copyProperty(session, CFSTR("SwitchEncoderRoutes"));
    CFTypeRef hardware = copyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder);
    int wrongInput = gJobs != TILES;
    for (int i = 0; i < gJobs; i++) {
        wrongInput += !inputMatches(i, gInputs[i]);
        free(gInputs[i]);
    }
    const int badOutput = badSamples(&outputs);
    const int32_t tiles = numberIn(routes, CFSTR("tiles")), errors = numberIn(routes, CFSTR("errors"));
    const int32_t byVIC = numberIn(routes, CFSTR("input_vic")), copied = numberIn(routes, CFSTR("input_cpu_copies"));
    const int32_t refusedInputs = numberIn(routes, CFSTR("input_refused"));
    const int32_t bounced = numberIn(routes, CFSTR("input_bounced"));
    const bool ok = propertiesSet && hasFormats && alignment == 2 && padding && prepared.width == TILE &&
                    prepared.height == TILE && !refused && outputs.count == TILES && !outputs.failed && !wrongInput &&
                    !badOutput && tiles == TILES && errors == 0 && hardware == kCFBooleanTrue;
    snprintf(line, size,
             "%s %d/%d tiles out (%d failed, %d refused), batch set %s, attributes %s, offset alignment %d, source "
             "padding %s, prepared %dx%d, inputs wrong %d, samples wrong %d, routes tiles %d errors %d, hardware %s, "
             "inputs %d by VIC %d bounced %d copied %d refused",
             ok ? "ok" : "FAIL", outputs.count, TILES, outputs.failed, refused, propertiesSet ? "ok" : "refused",
             hasFormats ? "ok" : "missing", alignment, padding ? "yes" : "no", prepared.width, prepared.height,
             wrongInput, badOutput, tiles, errors, hardware == kCFBooleanTrue ? "yes" : "no", byVIC, bounced, copied,
             refusedInputs);
    for (int i = 0; i < TILES; i++)
        if (outputs.samples[i])
            CFRelease(outputs.samples[i]);
    CFTypeRef owned[] = {attributes, requirements, routes, hardware};
    for (unsigned i = 0; i < sizeof(owned) / sizeof(owned[0]); i++)
        if (owned[i])
            CFRelease(owned[i]);
    CVPixelBufferRelease(source);
    CVPixelBufferRelease(copy);
    VTTileCompressionSessionInvalidate(session);
    CFRelease(session);
    return ok;
}
