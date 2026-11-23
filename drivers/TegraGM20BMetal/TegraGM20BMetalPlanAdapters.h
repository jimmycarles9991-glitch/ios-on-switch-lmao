// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BMetalPlanApply.h"

void encodePlannedState(SwitchMetalCommandBuffer *cb, size_t at, gm20b::CommandBuffer &commands,
                        gm20b::RenderEncoder *render, gm20b::ComputeEncoder *compute, gm20b::StateCursor &cursor);
