// SPDX-License-Identifier: MIT
// The passes the encoding thread opened, by their descriptors' contents: a render pass descriptor of Metal's own classes is
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
    MTLRenderPassDescriptor *pass;
    __unsafe_unretained id visibility;  // its visibility result buffer (the copy holds it)
    RMPass facts;
    std::array<uint32_t, 8> formats;
    uint32_t count, zeta, samples;
    uint8_t turn;
    uint64_t hash = 0, used = 0;
    std::vector<uint8_t> key;
};

// `d`'s contents, as the key a find and a keep take: false where `d` is not of Metal's own descriptor classes, or a sample
// buffer attachment names a buffer (counter marks read and clear them on the pass's copy)
bool rmPassKey(MTLRenderPassDescriptor *d, std::vector<uint8_t> &key, uint64_t &hash);
// The thread's pass of those contents, null where it has none
RMPassEntry *rmPassFind(const std::vector<uint8_t> &key, uint64_t hash);
// The pass just opened from `copy`, kept for the next one of the same contents
void rmPassKeep(const std::vector<uint8_t> &key, uint64_t hash, MTLRenderPassDescriptor *copy, const RMPass &facts,
                const RMRenderEncoder *e);
