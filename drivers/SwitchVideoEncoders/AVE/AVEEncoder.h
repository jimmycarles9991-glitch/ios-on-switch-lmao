// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVE's instance state, shared by its frame sessions (AVEEncoder.c) and tile sessions (AVETiles.c).
#ifndef AVE_ENCODER_H
#define AVE_ENCODER_H
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include "AVEEmitter.h"
#include "AVEReorder.h"
#include "H264NVENC.h"
#include "HEVCNVENC.h"
#include "NVENCClient.h"
#include "RateControl.h"
#include "VTEncoderSPI.h"

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t busyCond;
    bool encoding;  // single-owner guard across unlocked NVENC submits/waits/emits
    uint32_t busyWaiters;
    VTVideoEncoderSession session;
    VTTileEncoderSession tileSession;  // a tile session (HEIC grids) instead of a frame session
    NVENCClient *client;
    AVEEmitter *emitter;
    TegraNVENCInfo info;
    CMVideoDimensions size;  // a tile session's: the size every tile picture is coded at
    CMVideoCodecType codec;  // 'avc1', or 'hvc1': every picture an IDR (HEVCNVENC.h has no P pictures)
    bool configured, failed;
    HEVCEncConfig hevc;
    H264EncConfig config;
    H264EncLayout layout;
    H264EncHandles handles;
    uint8_t *inputVA, *bitstreamVA, *auxVA;
    AVETileSlot tileSlots[AVE_TILE_SLOTS];
    uint32_t tileSlotCount;
    uint32_t buffers[24];  // every buffer allocated, reordering and pipelined tile buffers among them
    unsigned bufferCount;
    CMVideoFormatDescriptionRef format;
    uint8_t *avcc;  // AVCC scratch for one picture
    size_t avccCap;
    int32_t maxKeyInterval, averageBitRate, constantBitRate;
    double maxKeyDuration, quality, expectedFrameRate, limitBytes, limitSeconds;
    bool allowTemporal, realTime, prioritizeSpeed, allowReordering;
    int32_t maxFrameDelay;  // MaxFrameDelayCount: frames held at most, -1 unlimited
    CFStringRef profileLevel, entropy, primaries, transfer, matrix;
    bool haveRef, keyPending;
    uint32_t frameNum, idrPicId, pictureIndex, pocCount, sinceKey;
    CMTime lastKeyTime;
    RateControl rc;
    AVEReorder reorder;
    uint32_t frames, keyFrames, tiles, errors;
    uint64_t bytes, copyTicks, encodeTicks;
    bool surfaceInput;  // VIC writes the input from the picture's IOSurface (AVEInput.c)
    // Pictures whose input the CPU copied, VIC wrote (among them those the copy engine staged first), VIC could not take,
    // and NVENC read in place from a native decoder picture.
    uint32_t inputCopies, inputVIC, inputBounced, inputRefused, inputDirect;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    CVPixelBufferRef probeSource;  // the last picture, for SwitchEncoderLayoutProbe
#endif
} Encoder;

// A region of a source picture, in pixels.
typedef struct {
    uint32_t x, y, width, height;
} AVERegion;

extern os_log_t gLog;

void ave_begin_encode(Encoder *e);
void ave_end_encode(Encoder *e);

// The TegraNVENC client for a session of e->size and e->codec, or kVTCouldNotFindVideoEncoderErr with the refusal
// logged once (a size NVENC cannot code, the process refused, no H.265 on this kernel).
OSStatus ave_open_engine(Encoder *e);
// The stream the first picture fixes (full range or not and its colour tags): buffers, parameter sets, the format
// description every sample carries, the rate control.
OSStatus ave_configure(Encoder *e, CVPixelBufferRef image);
// Called OFF e->lock. Waits via the completion port when listening (zero Wait ioctls), else one Wait ioctl (plus a
// legacy bitstream Wait only when the kernel lacks TEGRA_NVENC_FEATURE_COMPLETION).
bool ave_wait_job(Encoder *e, uint32_t fence, uint32_t auxHandle, const uint8_t *auxVA, uint32_t bitsHandle,
                  uint32_t bitsLimit, AVEJobStatus *out);

// One picture of a reordering session (AVEReorder.c), its type, references, frame number and POC stated: coded,
// then emitted with `dts`. NVENC refusing it sets `refused` and emits nothing; any other failure returns an error and
// emits nothing either.
typedef struct {
    VTVideoEncoderFrame frame;
    CVPixelBufferRef image;
    CMTime pts, duration, dts;
    H264EncPicture picture;
    H264EncHandles handles;
    bool refused;
} AVEPlan;

OSStatus ave_encode_planned(Encoder *e, AVEPlan *plan);
// Whether the next picture is a key frame: `since` pictures in display order since the last one.
bool ave_wants_key(const Encoder *e, CMTime pts, CFDictionaryRef options, uint32_t since);
int ave_alloc_buffer(Encoder *e, uint64_t size, uint32_t *handle);
void ave_tile_record_stats(void *context, OSStatus status, size_t bytes, uint64_t copyTicks, uint64_t encodeTicks);

// AVEReorder.c: B pictures for H.264 frame sessions that allow reordering.
bool ave_reorder_wanted(const Encoder *e);
OSStatus ave_reorder_start(Encoder *e);
// Holds, codes and emits the frame (and those it completes); returns an error only for a frame it neither held nor
// answered, which the caller then drops.
OSStatus ave_reorder_frame(Encoder *e, VTVideoEncoderFrame frame, CVPixelBufferRef image, CMTime pts, CMTime duration,
                           CFDictionaryRef options);
void ave_reorder_flush(Encoder *e);
void ave_reorder_release(Encoder *e);

// AVEInput.c: how a picture reaches NVENC's input buffer.
uint32_t ave_input_layout(const Encoder *e);
CFDictionaryRef ave_copy_input_attributes(const Encoder *e, int32_t width, int32_t height);
bool ave_input_put_buffer(const Encoder *e, CVPixelBufferRef image, const AVERegion *region, uint32_t inputHandle,
                          uint8_t *inputVA, TegraNVENCSubmit *submit, bool *refused);
int ave_input_submit_raw(NVENCClient *client, const TegraNVENCSubmit *submit, uint32_t *fence, uint32_t *route);
void ave_input_record_route(Encoder *e, const TegraNVENCSubmit *submit, int status, uint32_t route);

// AVETiles.c: the encoder class's version 3 slots.
OSStatus ave_start_tile_session(VTVideoEncoderRef ref, VTTileEncoderSession session, CFDictionaryRef specification,
                                VTInt32Size tileSize, CFAllocatorRef allocator);
OSStatus ave_prepare_to_encode_tiles(VTVideoEncoderRef ref, Boolean unused);
OSStatus ave_encode_tile(VTVideoEncoderRef ref, VTEncoderTile tile, CVImageBufferRef source, VTInt32Point origin,
                         VTInt32Size size, CFDictionaryRef options, VTEncodeInfoFlags *info);
OSStatus ave_complete_tiles(VTVideoEncoderRef ref);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's encode tests: this session's pictures NVENC coded, tiles among them, failures, and how their inputs
// reached NVENC.
CFDictionaryRef ave_copy_routes(const Encoder *e);
// AVELayoutProbe.c, hw_test_app's encode.nvenc_input_layouts: the last picture as one IDR from each input layout.
CFDictionaryRef ave_copy_layout_probe(Encoder *e);
#endif

#endif
