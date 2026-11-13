// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's instance state, shared by its frame sessions (AVDDecoder.c) and tile sessions (AVDTiles.c).
#ifndef AVD_DECODER_H
#define AVD_DECODER_H
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "AVDEmitter.h"
#include "AVDFrameTiming.h"
#include "H264NVDEC.h"
#include "HEVCNVDEC.h"
#include "MPEG2NVDEC.h"
#include "MPEG4NVDEC.h"
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
    AVDEmitter *emitter;
    FourCharCode codec;  // 'avc1', 'vp09', 'hvc1', 'mp4v', 'mp2v' or 'mp1v'
    H264Decoder *h264;
    VP9Decoder *vp9;
    HEVCDecoder *hevc;
    MPEG4Decoder *mpeg4;
    MPEG2Decoder *mpeg2;
    VTVideoDecoderRef software;  // the session runs on Apple's software decoder (AVDDecoder.c)
    CFDataRef avcC;              // or vpcC, hvcC, esds
    unsigned lengthSize;
    OSType pixelFormat;
    CFTypeRef matrix, primaries, transfer, chromaTop, chromaBottom;
    uint32_t width, height, frames;
    uint64_t decodeTicks, copyTicks;  // copyTicks: placing pictures in pixel buffers (VIC's or the CPU's copy)
    bool present;
    uint32_t presented, exported, copied;
    // VideoToolbox's HEVC-with-alpha contract (AVDAlpha.c): what MuxedAlphaDecoder asked before the start, and what
    // the session does: 'v0a8' buffers, and each picture written from firstPlane into the target VideoToolbox gives.
    bool alphaAsked, triPlanar, intoTarget;
    int32_t planeAsked;
    uint32_t firstPlane;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint32_t intoTargetCount;
    AVDFrameTiming timing;
#endif
} Decoder;

extern os_log_t gAVDLog;

// The factory every entry registers (AVDRegister.c).
OSStatus avd_create_instance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out);

void avd_set_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value);
CFDataRef avd_copy_atom(CMVideoFormatDescriptionRef format, CFStringRef name);
int avd_load_hvcc(Decoder *d, CFDataRef hvcC);
const char *avd_codec_name(FourCharCode codec);
// The session's NV12 pool at d->width x d->height ('x420'/'xf20' when tenBit), rows aligned for VIC.
void avd_set_attributes(Decoder *d, VTVideoDecoderSession session, bool fullRange, bool tenBit, OSStatus *status);
// The session on Apple's software decoder for d->codec, `why` logged once; NVDEC's state must be gone already.
OSStatus avd_start_software(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format,
                            const char *why);
// AVDMPEG.c: an 'mp4v' session, an 'mp2v' or 'mp1v' session, and a picture's field attachments (AVD_FIELDS_*).
OSStatus avd_start_mpeg4(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format);
OSStatus avd_start_mpeg2(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format);
Boolean avd_mpeg2_accepts(const Decoder *d, CMVideoFormatDescriptionRef format);
void avd_tag_fields(CVPixelBufferRef image, uint8_t fields);
// The sample's bytes, contiguous: *data points into the sample or at *copy (the caller frees it).
bool avd_sample_bytes(CMSampleBufferRef sample, const uint8_t **data, size_t *size, uint8_t **copy);
// The picture into `image` with its top-left at (x, y), by VIC, else by the CPU once `out`'s jobs are done; clipped to
// the image.
void avd_place_picture(Decoder *d, Output *out, const H264Picture *picture, CVPixelBufferRef image, uint32_t x,
                       uint32_t y);

// AVDTiles.c: the decoder class's version 3 slots.
OSStatus avd_start_tile_session(VTVideoDecoderRef ref, VTTileDecoderSession session,
                                CMVideoFormatDescriptionRef format);
OSStatus avd_decode_tile(VTVideoDecoderRef ref, int64_t frameID, CMSampleBufferRef sample, VTInt32Point tile,
                         VTInt32Size size, CVPixelBufferRef canvas, VTInt32Point offset, VTDecodeFrameFlags flags,
                         VTDecodeInfoFlags *info);
// A decoded tile picture into its canvas, and its answer once its job is done (both on the decoding thread, inside
// avd_decode_tile).
void *avd_tile_decoded(Decoder *d, const H264Picture *picture);
void avd_tile_emit(Decoder *d, Output *out);

// AVDAlpha.c: SelectPixelFormatWithAlpha and WriteDirectlyToPlanesOfTargetCVPixelBuffer (true when `key` is one).
bool avd_alpha_set_property(Decoder *d, CFStringRef key, CFTypeRef value);
bool avd_alpha_copy_property(const Decoder *d, CFStringRef key, CFTypeRef *value);
void avd_alpha_list_properties(CFMutableDictionaryRef supported);
// At the start of a stream NVDEC decodes: whether its pictures go out tri-planar, and from which plane.
void avd_alpha_start(Decoder *d, bool eightBit420);
const char *avd_alpha_describe(const Decoder *d);  // for the session's start line
// The CPU's copy of an alpha picture: its luma alone into plane 2.
void avd_alpha_copy_luma(const H264Picture *picture, CVPixelBufferRef image, uint32_t width, uint32_t height);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void avd_alpha_count(const Decoder *d, bool byVIC);
// The session's first plane (-1: not into a target), its pictures into targets, and the process's alpha-plane pictures
// by VIC and by the CPU.
void avd_alpha_routes(const Decoder *d, int32_t counts[4]);
// AVDRoutes.c: the SwitchPictureRoutes and SwitchDisplayBufferProbe properties.
CFDictionaryRef avd_copy_picture_routes(const Decoder *d);
CFDictionaryRef avd_copy_display_buffer_probe(const Decoder *d);
#endif

#endif
