// SPDX-License-Identifier: MIT
// A completable render state's key: its fast key (TegraGM20BMetalPipelineKey.h fastKey) of what its creation keeps of
// the descriptor, and from that, at their first ask, its key's text and the descriptor its variants are made of. A
// creation the pack answers makes neither and copies no descriptor.
#pragma once
#import <Metal/Metal.h>
#include <cstdint>
#include <string>
#include "stats.h"

@class SwitchMetalPipeline;
@class SwitchMetalFunction;

// What `d`, whose functions `vertex` and `fragment` (nil: none) are this device's, keeps in `p`; its fast key
uint64_t gm20bKeepKey(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, SwitchMetalFunction *vertex,
                      SwitchMetalFunction *fragment);
// The linked functions' segments of a state's key (gm20b::linkedSegment), " | <tag> - " for a function not this device's
std::string gm20bLinkedText(NSArray *vertex, NSArray *vertexPrivate, NSArray *fragment, NSArray *fragmentPrivate);
// A completable state's pack key text, "" for another
const std::string &gm20bStateKey(SwitchMetalPipeline *p);
// A completable state's descriptor of its functions and of the fields its key names, nil for another
MTLRenderPipelineDescriptor *gm20bMadeFrom(SwitchMetalPipeline *p);
// Debug builds: a state the pack lacks (`kind`: "render", "render variant", "compute"), named once a process by its key's
// hash and label, its key in harvest parts under that hash (logHarvestParts): the set the OS draws with is closed at the
// pack's build, so each one is a hole in its coverage, made on the device at a compile's cost
#if SWITCH_METAL_STATS
void gm20bNoteMiss(const char *kind, uint64_t keyHash, const std::string &key, NSString *label);
#endif
// Whether line `line` (each caller its own) is the first of the process for the key of hash `keyHash`
bool gm20bFirstNaming(uint64_t keyHash, uint32_t line);
