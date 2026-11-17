// SPDX-License-Identifier: MIT
// The passes a recording opened, by their descriptors' contents: a render pass descriptor of Metal's own classes is
// read as the bytes of its objects (their ivars found by name once, raw_objc_layout.h), with no message and nothing it
// would make lazily, and a pass of the same contents as one opened before takes that pass's copy of its descriptor and the
// facts read from it (Core Animation opens its passes of a frame in the same few shapes each frame). The copy is shared and
// read-only: an encoder that changes its pass (a store action it decides, a parallel encoder's seam) takes its own.
#pragma once
#import "RawMetalInternal.h"
#include <array>
#include <cstdint>
#include <vector>

// What a hit hands back: the copy, its facts, and what the encoder took from them
struct RMPassEntry {
    __unsafe_unretained MTLRenderPassDescriptor *pass;  // retained by the cache (CFBridgingRetain), not by ARC
    __unsafe_unretained id visibility;                  // its visibility result buffer (the copy holds it)
    RMPass facts;
    std::array<uint32_t, 8> formats;
    uint32_t count, zeta, samples;
    uint8_t turn;
    std::vector<uint8_t> key;
};

// `d`'s contents, as the key a find and a keep take: false where `d` is not of Metal's own descriptor classes, or a sample
// buffer attachment names a buffer (counter marks read and clear them on the pass's copy)
bool rmPassKey(MTLRenderPassDescriptor *d, std::vector<uint8_t> &key, uint64_t &hash);
// A recording owns its bank until completion: trim never touches a borrowed entry.
RMPassEntry *rmPassFind(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash);
// The pass just opened from `copy`, kept for the next one of the same contents
void rmPassKeep(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash, MTLRenderPassDescriptor *copy,
                const RMPass &facts, const RMRenderEncoder *e);

// Idle recordings' passes let go at pressure/idle release; active banks retire when their recordings return.
struct RMPassTrim {
    size_t passes, textures;
};

RMPassTrim rmPassTrim();
// What the idle recording pool's pass banks hold now (debug builds' rates line).
RMPassTrim rmPassHeld();

RMPassCachePtr rmNewPassCache();
void rmPassInvalidate();
uint64_t rmPassCacheReturned(RMPassCache *cache);
bool rmPassCachePublished(uint64_t generation);
RMPassTrim rmPassCacheTrim(RMPassCache *cache);
RMPassTrim rmPassCacheHeld(const RMPassCache *cache);
size_t rmPassCacheBytes(const RMPassCache *cache);
