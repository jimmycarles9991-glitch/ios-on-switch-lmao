// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BMetalPlanApply.h"

void encodePlannedState(SwitchMetalCommandBuffer *cb, size_t at, gm20b::CommandBuffer &commands,
                        gm20b::RenderEncoder *render, gm20b::ComputeEncoder *compute, gm20b::StateCursor &cursor,
                        bool own = false, uint32_t *namedBytesChunk = nullptr);
void encodePlannedDrawState(SwitchMetalCommandBuffer *cb, uint32_t stateIndex, gm20b::CommandBuffer &commands,
                            gm20b::RenderEncoder *render, gm20b::StateCursor &cursor, bool own = false,
                            uint32_t *namedBytesChunk = nullptr);
