// SPDX-License-Identifier: MIT
// Debug builds: a render state's creation measured on its thread (creation_cost.h)
#pragma once
#include "creation_cost.h"

#if SWITCH_METAL_PROF || SWITCH_METAL_STATS
#import <Metal/Metal.h>
#include <cstdint>
#include <string>
#include "encode_profile.h"

namespace gm20b {
class Pack;
struct PackRun;
}  // namespace gm20b

@class SwitchMetalPipeline;
#endif

#if SWITCH_METAL_PROF
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
#endif

#if SWITCH_METAL_STATS
bool gm20bPackIsTier1(const gm20b::Pack &pack, const gm20b::PackRun &run);
// A tier-1 state's first use in the process, logged with its harvest parts off the creating thread
void gm20bPackNoteTier1Use(const gm20b::Pack &pack, const gm20b::PackRun &run, uint64_t fast, uint64_t name,
                           SwitchMetalPipeline *p, NSString *label, const std::string &ownKey);
void gm20bNotePackCreation(bool packed);
// This process's render state creations since its device opened: those the pack held, every other, tier-1 first uses,
// and tier-1 first uses whose pages were cold on the card before import
void gm20bPackCreations(uint64_t &hits, uint64_t &misses, uint64_t &cold, uint64_t &card);
#endif
