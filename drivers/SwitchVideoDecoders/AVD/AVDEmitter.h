// SPDX-License-Identifier: BSD-2-Clause-Patent
// A frame session's pictures on their way to VideoToolbox. The codec hands them over in its output order (decode
// order, or display order under kReorder); each goes out once the session's jobs up to the newest one at its making
// have completed, in that order, with the status its own job reported. Completions arrive on the emitter's queue
// (NVDECListener.h) and emit from there: the decode thread waits for no picture, except to honour a synchronous
// DecodeFrame, an invalidate or a tile's answer (avd_emitter_settle), and before the CPU copies a picture VIC refused
// (avd_emitter_wait_picture).
#ifndef AVD_EMITTER_H
#define AVD_EMITTER_H
#include <stdbool.h>
#include <stdint.h>
#include "NVDECClient.h"
#include "VTDecoderSPI.h"

typedef struct Output {
    CVPixelBufferRef image;
    VTVideoDecoderFrame frame;
    OSStatus status;
    // Its own NVDEC job (0: none), a field pair's first field's (0: none), and the newest job of the session when it
    // was made.
    uint32_t job, firstFieldJob, due;
    bool jobDone, firstFieldDone, dueDone;
    uint64_t began, sequence;  // its DecodeFrame's start (mach time); its place in the emission order
    struct Output *next, *liveNext, *livePrevious;
} Output;

typedef struct AVDEmitter AVDEmitter;

// `client` serves the waits a settle makes; it must outlive the emitter's last settle.
AVDEmitter *avd_emitter_create(NVDECClient *client);
// The client has stopped listening (nvdec_stop_listening): nothing is emitted any more, and pictures never emitted are
// released once no drain is running.
void avd_emitter_destroy(AVDEmitter *e);
dispatch_queue_t avd_emitter_queue(const AVDEmitter *e);
void avd_emitter_completed(void *emitter, uint32_t fence, int result, uint32_t error_status, uint32_t mbs_correct);

// Decoding, under the decoder's lock.
void avd_emitter_set_session(AVDEmitter *e, VTVideoDecoderSession session);
void avd_emitter_frame_began(AVDEmitter *e, uint64_t now);
void avd_emitter_submitted(AVDEmitter *e, uint32_t fence, const TegraNVDECSubmit *submit, bool waitedForRoom);
Output *avd_emitter_output(AVDEmitter *e, VTVideoDecoderFrame frame, uint32_t job, uint32_t firstFieldJob);
// The picture failed on its way out (its pixel buffer, or the CPU's copy): its job's completion may be arriving.
void avd_emitter_fail(AVDEmitter *e, Output *out, OSStatus status);
void avd_emitter_push(AVDEmitter *e, Output *out);
// The decode thread waits for what the picture's jobs wrote: `range` is invalidated for the CPU's copy.
int avd_emitter_wait_picture(AVDEmitter *e, Output *out, TegraNVDECRange range);
// A dependency of the codec's own (VP9's entropy counts) waited for on the decode thread.
void avd_emitter_dependency_wait(AVDEmitter *e);

// Outside the decoder's lock.
void avd_emitter_drain(AVDEmitter *e);
// Every job of the session waited for and every picture pushed so far emitted before it returns.
void avd_emitter_settle(AVDEmitter *e, bool synchronous);
// A tile's answer: its jobs waited for, its status settled; the caller answers it.
void avd_emitter_settle_output(AVDEmitter *e, Output *out);
// No picture goes out after this returns (one emitting on another thread finishes first).
void avd_emitter_close(AVDEmitter *e);
uint32_t avd_emitter_errors(const AVDEmitter *e);

enum {
    AVD_ROUTE_PICTURE_WAITS,     // the decode thread waited for a picture: the CPU's copy of one VIC refused
    AVD_ROUTE_SYNC_WAITS,        // a synchronous DecodeFrame, an invalidate or a tile waited for its pictures
    AVD_ROUTE_DEPENDENCY_WAITS,  // the codec waited for a job's output (VP9's backward adaptation)
    AVD_ROUTE_ROOM_WAITS,        // a Submit slept for room in NVDEC's queue (the client fed faster than NVDEC decodes)
    AVD_ROUTE_IN_FLIGHT_MAX,     // the most pictures made and not yet emitted at once
    AVD_ROUTE_EMIT_LATENCY_US,   // mean from DecodeFrame's start to the picture's emission
    AVD_ROUTE_COUNT
};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void avd_emitter_routes(const AVDEmitter *e, int32_t counts[AVD_ROUTE_COUNT]);
#endif

#endif
