// SPDX-License-Identifier: MIT
// See TegraGM20BMetalDecoded.h.
#import "TegraGM20BMetalDecoded.h"
#import "TegraGM20BMetalBlitRules.h"
#include <algorithm>
#include <atomic>
#include <vector>
#include "decode_pass.h"

const gm20b::DecodedFormat *gm20bDecoded(SwitchMetalTexture *t)
{
    return t ? gm20b::decodedFormat(uint32_t(t->_descriptor.pixelFormat)) : nullptr;
}

bool gm20bDecodedCopyRecorded(SwitchMetalCommandBuffer *cb, const char *call, SwitchMetalBuffer *b,
                              SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize &size,
                              NSUInteger offset, NSUInteger row, NSUInteger image)
{
    const gm20b::DecodedFormat &f = *gm20bDecoded(t);
    if (level >= t->_descriptor.mipmapLevelCount || slice >= sliceCount((id<MTLTexture>)t)) {
        invalidBlit(cb, call, "of a level or slice past the texture's");
        return false;
    }
    const MTLSize at = levelSize((id<MTLTexture>)t, level);
    if (o.x % f.blockW || o.y % f.blockH || o.x >= at.width || o.y >= at.height || !size.width || !size.height ||
        !size.depth || o.z >= at.depth || size.depth > at.depth - o.z) {
        invalidBlit(cb, call, "into an origin off the format's blocks, or a region past its level");
        return false;
    }
    size.width = std::min(size.width, at.width - o.x);
    size.height = std::min(size.height, at.height - o.y);
    if (f.state == gm20b::DecodeState::Pvrtc && (o.x || o.y || size.width != at.width || size.height != at.height)) {
        invalidBlit(cb, call, "into part of a PVRTC level (Metal's validation: the whole level, its rows in order)");
        return false;
    }
    const uint64_t across = gm20b::decodedBlocks(uint32_t(size.width), f.blockW),
                   down = gm20b::decodedBlocks(uint32_t(size.height), f.blockH);
    const uint64_t last =
        offset + (size.depth - 1) * uint64_t(image) + (down - 1) * uint64_t(row) + across * f.blockBytes;
    if (offset % f.blockBytes || row % f.blockBytes || row < across * f.blockBytes || row > UINT32_MAX ||
        (size.depth > 1 && image < row * down) || last > b->_raw.length) {
        invalidBlit(cb, call, "of buffer rows, images or an offset of part blocks or past the buffer");
        return false;
    }
    return true;
}

bool gm20bEncodeDecodedCopy(gm20b::CommandBuffer *rc, SwitchMetalBuffer *b, SwitchMetalTexture *t, uint64_t offset,
                            uint64_t row, uint64_t image, const uint32_t size[3], uint32_t slice, uint32_t level,
                            const uint32_t origin[3], const char **why)
{
    const gm20b::DecodedFormat &f = *gm20bDecoded(t);
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    const bool volume = owner->_raw.type == gm20b::fx::kTexThreeD;
    for (uint32_t k = 0; k < size[2]; k++) {
        const gm20b::DecodeSource source{&b->_raw, offset + k * image, uint32_t(row)};
        if (!gm20b::decodePass(*gRM->raw, *rc, owner->_raw, f, t->_levelsBase + level,
                               volume ? origin[2] + k : t->_slicesBase + slice, origin[0], origin[1], size[0], size[1],
                               source, why))
            return false;
    }
    return true;
}

// Not dispatch_once: a queue that could not be made (no handler thread) is asked for again at the next upload, never
// kept as nil for the process's life. Two first uploads at once may both make one; the loser's goes.
id<MTLCommandQueue> gm20bDecodeQueue(id<MTLDevice> device)
{
    static std::atomic<void *> kept{nullptr};
    if (void *q = kept.load(std::memory_order_acquire))
        return (__bridge id<MTLCommandQueue>)q;
    id<MTLCommandQueue> made = [device newCommandQueue];
    if (!made)
        return nil;
    made.label = @"gm20b.decode";
    void *expected = nullptr;
    void *retained = (__bridge_retained void *)made;
    if (kept.compare_exchange_strong(expected, retained, std::memory_order_acq_rel))
        return made;
    CFRelease(retained);
    return (__bridge id<MTLCommandQueue>)expected;
}

bool gm20bDecodedReplace(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes,
                         NSUInteger row, NSUInteger image)
{
    const gm20b::DecodedFormat *f = gm20bDecoded(t);
    if (!f)
        return false;
    if (!bytes || level >= t->_descriptor.mipmapLevelCount)
        return true;
    const MTLSize at = levelSize((id<MTLTexture>)t, level);
    const bool pvrtc = f->state == gm20b::DecodeState::Pvrtc;
    // PVRTC: the whole level from its Morton-ordered bytes (bytesPerRow 0, as Metal's validation asks), into rows
    if (pvrtc)
        r = MTLRegionMake3D(0, 0, 0, at.width, at.height, 1);
    const uint32_t across = gm20b::decodedBlocks(
                       uint32_t(std::min(r.size.width, at.width - std::min(r.origin.x, at.width))), f->blockW),
                   down = gm20b::decodedBlocks(
                       uint32_t(std::min(r.size.height, at.height - std::min(r.origin.y, at.height))), f->blockH);
    const uint64_t line = uint64_t(across) * f->blockBytes, plane = line * down;
    const uint64_t from = pvrtc || !row ? line : row, fromPlane = image ? image : from * down;
    id<MTLCommandQueue> queue = gm20bDecodeQueue((id<MTLDevice>)t->_device);
    if (!queue) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.decoded_upload_queue", f->format,
                             "replaceRegion of %lux%lu of a format %u texture: no command queue for the driver's own "
                             "uploads (its handler thread could not be made): nothing written",
                             (unsigned long)r.size.width, (unsigned long)r.size.height, f->format);
        return true;
    }
    id<MTLBuffer> staging = [(id<MTLDevice>)t->_device newBufferWithLength:NSUInteger(plane * r.size.depth)
                                                                   options:MTLResourceStorageModeShared];
    if (!staging) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.decoded_upload", f->format,
                             "replaceRegion of %lux%lu of a format %u texture: no memory for its %llu bytes of blocks: "
                             "nothing written",
                             (unsigned long)r.size.width, (unsigned long)r.size.height, f->format,
                             (unsigned long long)(plane * r.size.depth));
        return true;
    }
    auto *to = static_cast<uint8_t *>(staging.contents);
    if (pvrtc) {
        gm20b::pvrtcRows(static_cast<const uint8_t *>(bytes), across, down, to);
    } else {
        for (NSUInteger z = 0; z < r.size.depth; z++)
            for (uint32_t y = 0; y < down; y++)
                memcpy(to + z * plane + y * line, static_cast<const uint8_t *>(bytes) + z * fromPlane + y * from, line);
    }
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:NSUInteger(line)
        sourceBytesPerImage:NSUInteger(plane)
                 sourceSize:r.size
                  toTexture:(id<MTLTexture>)t
           destinationSlice:slice
           destinationLevel:level
          destinationOrigin:r.origin
                    options:pvrtc ? MTLBlitOptionRowLinearPVRTC : MTLBlitOptionNone];
    [blit endEncoding];
    [cb commit];
    return true;
}

bool gm20bDecodedReadback(SwitchMetalTexture *t, const char *call)
{
    const gm20b::DecodedFormat *f = gm20bDecoded(t);
    if (!f)
        return false;
    SWITCH_REFUSE_DETAIL("gm20b", "tex.decoded_readback", f->format,
                         "%s of a format %u texture: nothing moved (its blocks were decoded as they were uploaded, and "
                         "a format whose views read the same codes keeps none; Apple's GPUs give the blocks back)",
                         call, f->format);
    return true;
}
