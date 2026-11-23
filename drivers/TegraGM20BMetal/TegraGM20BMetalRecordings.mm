// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPassCache.h"
#include "take_pool.h"

// A finished command buffer's recording storage, kept for the next one (its capacity a frame's): Core Animation's steady frames
// allocate none of it. As many are kept as were live at once (an app that commits a batch before it waits records each into
// kept storage), up to kKeptTotal bytes and kKeptMost recordings; gRawRelease gives back all but kKeptIdle. TakePool
// transfers exclusive ownership: a command buffer takes one where it is made and returns it after completion, or at
// dealloc if it was never submitted. Public completion results stay with the command buffer.
struct SwitchMetalRecording {
    SwitchMetalCommandPlan plan;
    gm20b::GrowList<SwitchMetalPass> passes;
    SwitchMetalBytes bytes;
    std::deque<gm20b::RetirementWrites> retirements;
    gm20b::GrowList<uint8_t> shadow;
    gm20b::GrowList<uint64_t> extra;
    std::vector<id> owners;
    gm20b::PointerMap<bool> ownerKeys;
    SwitchMetalPassCachePtr passCache;
    size_t kept = 0;
};

static constexpr size_t kKeptTotal = 16 << 20, kKeptIdle = 4;
static constexpr uint32_t kKeptMost = 64;
static gm20b::TakePool<SwitchMetalRecording, kKeptMost> &gRecordings =
    *new gm20b::TakePool<SwitchMetalRecording, kKeptMost>;
static std::atomic<size_t> gRecordingBytes{0};

static void swapRecording(SwitchMetalCommandBuffer *b, SwitchMetalRecording &r)
{
    std::swap(b->_plan, r.plan);
    b->_passes.swap(r.passes);
    std::swap(b->_bytes, r.bytes);
    b->_retirements.swap(r.retirements);
    b->_shadow.swap(r.shadow);
    b->_extra.swap(r.extra);
    b->_owners.swap(r.owners);
    std::swap(b->_ownerKeys, r.ownerKeys);
    b->_passCache.swap(r.passCache);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static std::atomic<uint64_t> gRecordingMisses{0}, gRecordingDropsLarge{0}, gRecordingDropsTotal{0},
    gRecordingDropsSlots{0};
#define SWITCH_METAL_RECORDING_COUNT(counter) (counter).fetch_add(1, std::memory_order_relaxed)

SwitchMetalRecordingCounts gm20bRecordingCounts()
{
    return {gRecordingMisses.load(std::memory_order_relaxed), gRecordingDropsLarge.load(std::memory_order_relaxed),
            gRecordingDropsTotal.load(std::memory_order_relaxed), gRecordingDropsSlots.load(std::memory_order_relaxed)};
}
#else
#define SWITCH_METAL_RECORDING_COUNT(counter)
#endif

void gm20bTakeRecording(SwitchMetalCommandBuffer *b)
{
    uint32_t node;
    SwitchMetalRecording *r = gRecordings.take(node);
    if (!r) {
        SWITCH_METAL_RECORDING_COUNT(gRecordingMisses);
        b->_owners.reserve(256);
        b->_passCache = gm20bNewPassCache();
        return;
    }
    swapRecording(b, *r);
    gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
    r->kept = 0;
    gRecordings.giveBack(node);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// facadecost's: setBytes chunks made for a command buffer the pool had none for, and made ahead, and the bytes of every
// setBytes chunk alive (kept, in a recording or in flight)
static std::atomic<uint64_t> gChunkMisses{0}, gChunksMadeAhead{0}, gChunkBytesLive{0};
#define SWITCH_METAL_CHUNK_COUNT(counter, value) counter.fetch_add(uint64_t(value), std::memory_order_relaxed)
#else
#define SWITCH_METAL_CHUNK_COUNT(counter, value)
#endif

static void dropBytes(SwitchMetalBytes &s)
{
    for (gm20b::Buffer &k : s.chunks)
        if (k.va) {
            SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, -k.length);
            gRM->raw->destroyBuffer(k);
        } else
            free(k.cpu);
    s = {};
}

// A recording that outgrew its first chunk keeps one chunk of their total: the next command buffer of its shape names one
// chunk to its job, not one for each 64 KB of its setBytes
static void coalesceBytes(SwitchMetalBytes &s)
{
    if (s.chunks.size() < 2)
        return;
    uint64_t length = 0;
    for (const gm20b::Buffer &k : s.chunks) {
        if (!k.va)
            return;
        length += k.length;
    }
    gm20b::Buffer one;
    if (!gRM->raw->createControlBuffer(length, one))
        return;
    SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, one.length);
    dropBytes(s);
    s.chunks.push_back(one);
}

// Finished command buffers' setBytes chunks for the next to fill, given back at completion rather than with the recording
// at dealloc: command buffers an app keeps after they complete hold none of the GPU pool's control memory. As many are kept
// as were in flight at once, up to kChunksKept and kChunkBytesKept bytes, each up to kChunkMost (a command buffer of more
// setBytes frees its own, as its recording); gRawRelease gives back all but kChunksIdle.
static constexpr uint32_t kChunksKept = 64, kChunksIdle = 4;
static constexpr uint64_t kChunkMost = 1 << 20, kChunkBytesKept = 16 << 20;
static gm20b::TakePool<gm20b::Buffer, kChunksKept> &gBytesChunks = *new gm20b::TakePool<gm20b::Buffer, kChunksKept>;
static std::atomic<uint64_t> gBytesChunkBytes{0};

static std::atomic<bool> gBytesGrowing{false};

// The length of the chunk taken last, up to kChunkMost: what a chunk made for the pool, or for a command buffer it had none
// for, is made at. A recording that coalesced past 64 KB (coalesceBytes) given a 64 KB one outgrows it and coalesces
// again at its completion, making and freeing control buffers on the completion thread.
static std::atomic<uint64_t> gBytesChunkLength{kRMBytesChunk};

static void keepBytesChunk(const gm20b::Buffer &chunk)
{
    uint32_t node;
    const bool fits =
        chunk.va && chunk.length <= kChunkMost &&
        gBytesChunkBytes.fetch_add(chunk.length, std::memory_order_relaxed) + chunk.length <= kChunkBytesKept;
    gm20b::Buffer *slot = fits ? gBytesChunks.room(node) : nullptr;
    if (!slot) {
        if (chunk.va && chunk.length <= kChunkMost)
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
        SwitchMetalBytes one;
        one.chunks.push_back(chunk);
        dropBytes(one);
        return;
    }
    *slot = chunk;
    gBytesChunks.keep(node);
}

static void growBytesChunks(void *)
{
    gm20b::Buffer chunk;
    if (gRM->raw->createControlBuffer(gBytesChunkLength.load(std::memory_order_relaxed), chunk)) {
        SWITCH_METAL_CHUNK_COUNT(gChunksMadeAhead, 1);
        SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, chunk.length);
        keepBytesChunk(chunk);
    }
    gBytesGrowing.store(false, std::memory_order_release);
}

// The pool is grown by a chunk, on the utility queue, when a completion that gave chunks back leaves it at its last chunk
// or none: the completion side decides, so a recording thread only ever takes a chunk (or, finding none, makes its own: a
// miss)
static void growBytesChunksAhead()
{
    if (gBytesChunks.kept() <= 1 && !gBytesGrowing.exchange(true, std::memory_order_acq_rel))
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), nullptr, growBytesChunks);
}

void gm20bBytesDone(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalBytes &s = cb->_bytes;
    const bool gaveBack = !s.chunks.empty();
    coalesceBytes(s);
    for (const gm20b::Buffer &k : s.chunks)
        keepBytesChunk(k);
    s.chunks.clear();  // its capacity goes on with the recording to the next command buffer
    s.at = 0;
    s.used = 0;
    if (gaveBack)
        growBytesChunksAhead();
}

bool gm20bTakeBytesChunk(uint64_t length, gm20b::Buffer &chunk)
{
    uint32_t node;
    if (gm20b::Buffer *kept = gBytesChunks.take(node)) {
        if (kept->length >= length) {
            chunk = *kept;
            *kept = {};
            gBytesChunks.giveBack(node);
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
            gBytesChunkLength.store(std::min(chunk.length, kChunkMost), std::memory_order_relaxed);
            return true;
        }
        gBytesChunks.keep(node);
    }
    if (!gRM->raw->createControlBuffer(std::max(length, gBytesChunkLength.load(std::memory_order_relaxed)), chunk))
        return false;
    SWITCH_METAL_CHUNK_COUNT(gChunkMisses, 1);
    SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, chunk.length);
    return true;
}

void gm20bRecordingDone(SwitchMetalCommandBuffer *b)
{
    if (b->_recordingReturned)
        return;
    b->_recordingReturned = true;
    gm20bPassCacheUnbind(b->_passCache.get());
    constexpr size_t kKeptBytes = 1 << 20;  // (a command buffer of a few megabytes of setBytes: its own, freed)
    uint64_t bytes = 0;
    for (const gm20b::Buffer &k : b->_bytes.chunks)
        bytes += k.va ? k.length : kKeptBytes;  // (heap chunks are not kept)
    bytes += b->_plan.storageBytes() + b->_extra.capacity() * 8 + b->_shadow.capacity() +
             gm20bPassCacheBytes(b->_passCache.get());
    if (bytes > kKeptBytes) {
        SWITCH_METAL_RECORDING_COUNT(gRecordingDropsLarge);
        SwitchMetalRecording dropped;
        swapRecording(b, dropped);
        dropBytes(dropped.bytes);
        return;
    }
    uint32_t node;
    const bool fits = gRecordingBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes <= kKeptTotal;
    SwitchMetalRecording *r = fits ? gRecordings.room(node) : nullptr;
    if (!r) {
        SWITCH_METAL_RECORDING_COUNT(*(fits ? &gRecordingDropsSlots : &gRecordingDropsTotal));
        gRecordingBytes.fetch_sub(bytes, std::memory_order_relaxed);
        SwitchMetalRecording dropped;
        swapRecording(b, dropped);
        dropBytes(dropped.bytes);
        return;
    }
    swapRecording(b, *r);
    r->kept = bytes;
    r->plan.clear();
    r->passes.clear();
    r->bytes.at = 0;
    r->bytes.used = 0;
    r->shadow.clear();
    r->extra.clear();
    r->owners.clear();
    r->ownerKeys.clear();
    coalesceBytes(r->bytes);
    gRecordings.keep(node);
}

// `count` of each kept recording's pass bank, owned while it is read and kept again in the order it had: the next command
// buffer takes the recording returned last
template <typename Count> static SwitchMetalPassTrim sumKeptBanks(Count &&count)
{
    SwitchMetalPassTrim total{};
    uint32_t nodes[kKeptMost], taken = 0;
    for (; taken < kKeptMost; taken++) {
        SwitchMetalRecording *r = gRecordings.take(nodes[taken]);
        if (!r)
            break;
        const SwitchMetalPassTrim part = count(r->passCache.get());
        total.passes += part.passes;
        total.textures += part.textures;
    }
    while (taken)
        gRecordings.keep(nodes[--taken]);
    return total;
}

SwitchMetalPassTrim gm20bPassTrim()
{
    return sumKeptBanks(gm20bPassCacheTrim);
}

SwitchMetalPassTrim gm20bPassHeld()
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return sumKeptBanks(gm20bPassCacheHeld);
#else
    return {};
#endif
}

void releaseRecordings()
{
    while (gBytesChunks.kept() > kChunksIdle) {
        uint32_t node;
        gm20b::Buffer *kept = gBytesChunks.take(node);
        if (!kept)
            break;
        gBytesChunkBytes.fetch_sub(kept->length, std::memory_order_relaxed);
        SwitchMetalBytes one;
        one.chunks.push_back(*kept);
        dropBytes(one);
        *kept = {};
        gBytesChunks.giveBack(node);
    }
    while (gRecordings.kept() > kKeptIdle) {
        uint32_t node;
        SwitchMetalRecording *r = gRecordings.take(node);
        if (!r)
            break;
        gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
        dropBytes(r->bytes);
        *r = SwitchMetalRecording();
        gRecordings.giveBack(node);
    }
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
@interface SwitchMetalCommandBuffer (Recordings)
@end
@implementation SwitchMetalCommandBuffer (Recordings)

// facadecost's: the setBytes chunks' misses and those made ahead since the process began, and their live bytes now
+ (NSDictionary<NSString *, NSNumber *> *)switchMetalBytesChunkStats
{
    return @{
        @"misses" : @(gChunkMisses.load(std::memory_order_relaxed)),
        @"made_ahead" : @(gChunksMadeAhead.load(std::memory_order_relaxed)),
        @"live_bytes" : @(gChunkBytesLive.load(std::memory_order_relaxed)),
    };
}

@end
#endif
