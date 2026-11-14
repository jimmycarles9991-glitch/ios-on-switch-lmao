// SPDX-License-Identifier: MIT
// A command buffer encoded ahead: while its caller records, the submit worker, when it has nothing to submit, encodes the
// encoders that have ended into the device's raw command buffer (RawMetalRangeEncode.h, ahead), so its commit only seals
// what is left and hands the job to the kernel. The encode ahead is the device's open command buffer: anything else the
// worker submits first takes it back (abandoned, encoded at that command buffer's commit as before), so the GPU's order
// is the order of the encodes, as it is when everything is encoded at commit.
#pragma once
#import "RawMetalInternal.h"

// The caller's, as an encoder of `cb` ended (its OpEnd recorded, its range prepared): what is final of the tape published,
// and the worker handed the command buffer once enough was recorded since it last was
void rmPublishEncoder(RMCommandBuffer *cb);

// The worker's: `cb` was handed over (RMScheduleChange::Encoded)
void rmPipelineHanded(RMCommandBuffer *cb);
// The worker's, with nothing to submit: the encoders published since it last looked, encoded. False: nothing to do.
bool rmPipelineStep();
// The worker's, as it is about to sleep (RawState::submitAsleep stored): whether the command buffer it encodes ahead
// published more since its last step
bool rmPipelinePublished();
// The worker's, before it submits a range of `cb` (null: before other work): the encode ahead of another command buffer,
// or of this one where the range is not the one it began, given back
void rmPipelineYield(RMCommandBuffer *cb, size_t from, bool nativeRange);
// The worker's, in encodeRaw: the encode ahead of `cb`'s range from `from` taken over by the commit's encode (resumed);
// false: none, or it is no longer good (abandoned)
bool rmPipelineTake(RMCommandBuffer *cb, size_t from);
// The worker's, once a raw range of `cb` ending at `to` was submitted: its counts of encoders encoded ahead
void rmPipelineSealed(RMCommandBuffer *cb, size_t to);
// The worker's, when the device goes idle: an encode ahead whose command buffer no one else holds (it can never be
// committed) given back
void rmPipelineOrphans();
