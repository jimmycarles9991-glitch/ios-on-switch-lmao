// SPDX-License-Identifier: MIT
// Debug builds: a render state's creation measured on its thread (creation_cost.h)
#pragma once
#include "creation_cost.h"

#if SWITCH_METAL_DIAGNOSTICS
#import <Metal/Metal.h>
#include <cstdint>
#include "encode_profile.h"

@class SwitchMetalPipeline;

// From its making to finish(): the costs are the thread's last creation's (switchMetalLastCreation), a creation of 1 ms or
// more gets one line, and the process counts the creations the pack held and those it did not
class SwitchMetalCreationMeter {
public:
    SwitchMetalCreationMeter();
    ~SwitchMetalCreationMeter();
    SwitchMetalCreationMeter(const SwitchMetalCreationMeter &) = delete;
    SwitchMetalCreationMeter &operator=(const SwitchMetalCreationMeter &) = delete;

    // `p`: the state made, nil where its creation failed before one was
    void finish(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d);

private:
    gm20b::CreationWork work_;
    gm20b::EncodeProfile profile_;
    gm20b::EncodeProfile *saved_;
    uint64_t began_ = 0, probe_ = 0, locks_ = 0, waits_ = 0, waitTicks_ = 0, faults_ = 0, pageins_ = 0;
    bool finished_ = false;
};

// This process's render state creations since its device opened: those the pack held, and every other
void gm20bPackCreations(uint64_t &hits, uint64_t &misses);
#endif
