// SPDX-License-Identifier: MIT
// A command buffer encoded ahead: while its caller records, the submit worker, when it has nothing to submit, encodes the
// encoders that have ended into the device's raw command buffer (TegraGM20BMetalRangeEncode.h, ahead), so its commit only seals
// what is left and hands the job to the kernel. The encode ahead is the device's open command buffer: anything else the
// worker submits first takes it back (abandoned, encoded at that command buffer's commit as before), so the GPU's order
// is the order of the encodes, as it is when everything is encoded at commit.
#pragma once
#import "TegraGM20BMetalInternal.h"

// The caller's, as an encoder of `cb` ended (its OpEnd recorded, its range prepared where the worker reads it ahead): what
// is final of the tape published, and the worker handed the command buffer once enough was recorded since it last was,
// every encoder recorded before that first hand-off prepared then
void gm20bPublishEncoder(SwitchMetalCommandBuffer *cb);
// The caller's: whether the worker may encode the encoder that just ended ahead of the commit, so the caller prepares
// its range (the worker reads it with the tape still being written). Otherwise its range is the worker's to prepare at
// the commit's turn, with the rest of the range it is in.
bool gm20bAheadReads(SwitchMetalCommandBuffer *cb);
// The caller's: the encoder [from, to)'s range prepared on the recording thread
void gm20bPrepareEncoder(SwitchMetalCommandBuffer *cb, size_t from, size_t to);

// The worker's: `cb` was handed over (SwitchMetalScheduleChange::Encoded)
void gm20bPipelineHanded(SwitchMetalCommandBuffer *cb);
// The worker's, with nothing to submit: the encoders published since it last looked, encoded. False: nothing to do.
bool gm20bPipelineStep();
// The worker's, as it is about to sleep (GM20BState::submitAsleep stored): whether the command buffer it encodes ahead
// published more since its last step
bool gm20bPipelinePublished();
// The worker's, before it submits a range of `cb` (null: before other work): the encode ahead of another command buffer,
// or of this one where the range is not the one it began, given back
void gm20bPipelineYield(SwitchMetalCommandBuffer *cb, size_t from, bool nativeRange);
// The worker's, before it finishes `cb` on an error found before any range of it was submitted: the encode ahead of `cb`
// given back first, as gm20bPipelineYield gives it back before a range. That finish may complete `cb`, and its completion
// returns the recording the encode reads to the pool for another command buffer to take.
void gm20bPipelineFailed(SwitchMetalCommandBuffer *cb);
// The worker's, in encodeRaw: the encode ahead of `cb`'s range from `from` taken over by the commit's encode (resumed);
// false: none, or it is no longer good (abandoned)
bool gm20bPipelineTake(SwitchMetalCommandBuffer *cb, size_t from);
// The worker's, in encodeRaw: a taken encode ahead that did not seal (clears its pipelined count; non-zero `veto` counts
// the abandon before whole-range fallback)
void gm20bPipelineAbandoned(SwitchMetalCommandBuffer *cb, uint32_t veto);
// The worker's, at the commit's turn before it prepares `cb`'s next range: a range the encode ahead of `cb` covers
// only in part split where the encode ahead stopped or reached between two encoders, so what was encoded ahead goes to
// the GPU at once and the rest is encoded whole after it
void gm20bPipelineSplit(SwitchMetalCommandBuffer *cb);
void gm20bPipelineEarlyWork(SwitchMetalCommandBuffer *cb, uint32_t cap, uint32_t &remaining, bool &multiRange);
// The worker's, once a raw range of `cb` ending at `to` was submitted: its counts of encoders encoded ahead
void gm20bPipelineSealed(SwitchMetalCommandBuffer *cb, size_t to);
// The worker's, when the device goes idle: an encode ahead whose command buffer no one else holds (it can never be
// committed) given back
void gm20bPipelineOrphans(bool yieldOpen = false);
bool gm20bClaimCommitSlot(SwitchMetalCommandBuffer *cb);
void gm20bFinishEmpty(SwitchMetalCommandBuffer *cb);

#if SWITCH_METAL_DIAGNOSTICS
// Debug builds' frame rates line (TegraGM20BMetalEncoderRates.mm, frame_cpu_counts.h). The worker's: it took `cb` as its
// queue's head, or parked that head for `why` (gm20b::HeadWait); the finishing thread's: `cb` is done, its encode ahead
// counted; the waiter's: its waitUntilScheduled blocked from `fromUs` to `toUs`.
void gm20bNoteTaken(SwitchMetalCommandBuffer *cb);
void gm20bNoteParked(SwitchMetalCommandBuffer *cb, unsigned why);
void gm20bNoteFinished(SwitchMetalCommandBuffer *cb);
void gm20bNoteScheduledWait(SwitchMetalCommandBuffer *cb, uint64_t fromUs, uint64_t toUs);
void gm20bNoteEnqueue(SwitchMetalCommandBuffer *cb);
void gm20bSampleLateWrites(SwitchMetalCommandBuffer *cb, size_t from, size_t to);
void gm20bCheckLateWrites(SwitchMetalCommandBuffer *cb);
void gm20bDropLateWrites(SwitchMetalCommandBuffer *cb);
#endif
