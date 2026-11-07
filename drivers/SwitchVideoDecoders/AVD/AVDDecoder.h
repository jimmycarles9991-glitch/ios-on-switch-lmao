// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's instance state, shared by its frame sessions (AVDDecoder.c) and tile sessions (AVDTiles.c).
#ifndef AVD_DECODER_H
#define AVD_DECODER_H
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "H264NVDEC.h"
#include "HEVCNVDEC.h"
#include "NVDECClient.h"
#include "VP9NVDEC.h"
#include "VTDecoderSPI.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

// The tile being decoded: its frame ID, the canvas it goes into and where.
typedef struct {
    int64_t frame;
    CVPixelBufferRef canvas;
    uint32_t x, y;
    bool answered;
} AVDTile;

typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    VTTileDecoderSession tileSession;  // a tile session (grid images) instead of a frame session
    AVDTile tile;
    NVDECClient *client;
    FourCharCode codec;  // 'avc1', 'vp09' or 'hvc1'
    H264Decoder *h264;
    VP9Decoder *vp9;
    HEVCDecoder *hevc;
    VTVideoDecoderRef software;  // the session runs on Apple's software decoder (AVDDecoder.c)
    CFDataRef avcC;              // or vpcC, hvcC
    unsigned lengthSize;
    OSType pixelFormat;
    CFTypeRef matrix, primaries, transfer;
    uint32_t width, height, frames, errors;
    uint64_t decodeTicks, copyTicks;  // copyTicks: placing pictures in pixel buffers (VIC's or the CPU's copy)
    bool present;                     // VIC copies the pictures (until it refuses one)
    uint32_t presented, copied;
} Decoder;

typedef struct {
    CVPixelBufferRef image;
    VTVideoDecoderFrame frame;
    OSStatus status;
} Output;

extern os_log_t gAVDLog;

void avd_set_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value);
CFDataRef avd_copy_atom(CMVideoFormatDescriptionRef format, CFStringRef name);
int avd_load_hvcc(Decoder *d, CFDataRef hvcC);
// The sample's bytes, contiguous: *data points into the sample or at *copy (the caller frees it).
bool avd_sample_bytes(CMSampleBufferRef sample, const uint8_t **data, size_t *size, uint8_t **copy);
// The picture into `image` with its top-left at (x, y), by VIC, else by the CPU; clipped to the image.
void avd_place_picture(Decoder *d, const H264Picture *picture, CVPixelBufferRef image, uint32_t x, uint32_t y);

// AVDTiles.c: the decoder class's version 3 slots.
OSStatus avd_start_tile_session(VTVideoDecoderRef ref, VTTileDecoderSession session,
                                CMVideoFormatDescriptionRef format);
OSStatus avd_decode_tile(VTVideoDecoderRef ref, int64_t frameID, CMSampleBufferRef sample, VTInt32Point tile,
                         VTInt32Size size, CVPixelBufferRef canvas, VTInt32Point offset, VTDecodeFrameFlags flags,
                         VTDecodeInfoFlags *info);
// A decoded tile picture into its canvas, and its answer (both on the decoding thread, inside avd_decode_tile).
void *avd_tile_decoded(Decoder *d, const H264Picture *picture);
void avd_tile_emit(Decoder *d, Output *out);

#endif
