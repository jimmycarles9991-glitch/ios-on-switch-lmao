// SPDX-License-Identifier: MIT
// A command buffer encoded ahead: while its caller records, the submit worker, when it has nothing to submit, encodes the
// encoders that have ended into the device's raw command buffer (TegraGM20BMetalRangeEncode.h, ahead), so its commit only seals
// what is left and hands the job to the kernel. The encode ahead is the device's open command buffer: anything else the
// worker submits first takes it back (abandoned, encoded at that command buffer's commit as before), so the GPU's order
// is the order of the encodes, as it is when everything is encoded at commit.
#pragma once
#import "TegraGM20BMetalInternal.h"

// The caller's, as an encoder of `cb` ended (its OpEnd recorded, its range prepared): what is final of the tape published,
// and the worker handed the command buffer once enough was recorded since it last was
void gm20bPublishEncoder(SwitchMetalCommandBuffer *cb);

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
// The worker's, once a raw range of `cb` ending at `to` was submitted: its counts of encoders encoded ahead
void gm20bPipelineSealed(SwitchMetalCommandBuffer *cb, size_t to);
// The worker's, when the device goes idle: an encode ahead whose command buffer no one else holds (it can never be
// committed) given back
void gm20bPipelineOrphans();
