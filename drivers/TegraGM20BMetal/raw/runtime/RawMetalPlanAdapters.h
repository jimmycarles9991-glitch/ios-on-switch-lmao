// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "RawMetalPlanApply.h"

void encodePlannedState(RMCommandBuffer *cb, size_t at, raw::CommandBuffer &commands, raw::RenderEncoder *render,
                        raw::ComputeEncoder *compute, raw::StateCursor &cursor);
void replayPlannedState(RMCommandBuffer *cb, size_t at, id encoder, raw::StateCursor &cursor,
                        std::set<RMBuffer *> &buffers, RMBuffer *__strong bound[3][31], uint64_t offsets[3][31],
                        RMPipeline * __strong & pipeline);
