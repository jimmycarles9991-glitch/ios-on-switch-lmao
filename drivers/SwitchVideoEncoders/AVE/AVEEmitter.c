// SPDX-License-Identifier: BSD-2-Clause-Patent
// See AVEEmitter.h.
#include "AVEEmitter.h"
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <mach/mach_time.h>
#endif
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "H264NVENC.h"

extern os_log_t gLog;

enum {
    kCompletedRing = 16,
    kTilePool = 16,
};

typedef struct {
    uint32_t fence;
    bool valid;
    AVEJobStatus status;
} CompletedJob;

typedef struct PendingTile {
    struct PendingTile *next;
    VTEncoderTile tile;
    CVPixelBufferRef source;
    uint64_t sequence;
    uint64_t began, copied;
    uint32_t fence;
    int slotIndex;  // -1: no NVENC job (immediate error)
    bool done;
    OSStatus immediateError;
    AVEJobStatus jobStatus;
} PendingTile;

struct AVEEmitter {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    dispatch_queue_t queue;
    uint32_t references;  // under lock: the encoder's plus any active drain
    bool closed;
    CompletedJob completed[kCompletedRing];
    uint32_t completedCursor;
    VTTileEncoderSession tileSession;
    CMVideoFormatDescriptionRef format;
    AVETileSlot slots[AVE_TILE_SLOTS];
    bool slotBusy[AVE_TILE_SLOTS];
    uint32_t slotCount, bitstreamBytes;
    uint8_t *avcc;
    size_t avccCap;
    AVETileStats stats;
    void *statsContext;
    PendingTile *head, *tail;
    PendingTile tilePool[kTilePool];
    PendingTile *freeTiles;
    uint64_t pushed, emittedThrough;
    pthread_t drainer;
    uint32_t draining;
};

static bool isPooledTile(const AVEEmitter *e, const PendingTile *t)
{
    return t >= &e->tilePool[0] && t < &e->tilePool[kTilePool];
}

static PendingTile *allocTileLocked(AVEEmitter *e)
{
    PendingTile *t = e->freeTiles;
    if (t) {
        e->freeTiles = t->next;
        memset(t, 0, sizeof(*t));
        return t;
    }
    return calloc(1, sizeof(*t));
}

static void freeTileLocked(AVEEmitter *e, PendingTile *t)
{
    if (isPooledTile(e, t)) {
        t->next = e->freeTiles;
        e->freeTiles = t;
        return;
    }
    free(t);
}

static CompletedJob *findCompletedLocked(AVEEmitter *e, uint32_t fence)
{
    for (uint32_t i = 0; i < kCompletedRing; i++) {
        if (e->completed[i].valid && e->completed[i].fence == fence)
            return &e->completed[i];
    }
    return NULL;
}

static void recordCompletedLocked(AVEEmitter *e, uint32_t fence, AVEJobStatus status)
{
    CompletedJob *slot = &e->completed[e->completedCursor % kCompletedRing];
    e->completedCursor++;
    *slot = (CompletedJob){.fence = fence, .valid = true, .status = status};
}

static void dropUnlocked(AVEEmitter *e)
{
    pthread_mutex_lock(&e->lock);
    const bool last = --e->references == 0;
    pthread_mutex_unlock(&e->lock);
    if (!last)
        return;
    while (e->head) {
        PendingTile *t = e->head;
        e->head = t->next;
        if (t->source)
            CVPixelBufferRelease(t->source);
        if (!isPooledTile(e, t))
            free(t);
    }
    free(e->avcc);
    if (e->format)
        CFRelease(e->format);
    dispatch_release(e->queue);
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

static OSStatus buildTileSample(const AVEEmitter *e, const PendingTile *t, size_t *outSize, CMSampleBufferRef *out)
{
    *outSize = 0;
    *out = NULL;
    if (t->immediateError)
        return t->immediateError;
    const AVEJobStatus *s = &t->jobStatus;
    if (s->result)
        return kVTVideoEncoderMalfunctionErr;
    const uint32_t produced = (s->total_bit_count + 7) / 8;
    if (s->ucode_error_status || !produced || produced > e->bitstreamBytes) {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gLog, "AVE: NVENC tile picture failed: error_status %u ucode %#x, %u bits", s->error_status,
                         s->ucode_error_status, s->total_bit_count);
        return kVTVideoEncoderMalfunctionErr;
    }
    const uint8_t *bs = e->slots[t->slotIndex].bitstreamVA;
    const size_t size = h264enc_annexb_to_avcc(bs, produced, e->avcc, e->avccCap);
    if (!size)
        return kVTVideoEncoderMalfunctionErr;
    CMBlockBufferRef block = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, size, kCFAllocatorDefault, NULL, 0,
                                                         size, kCMBlockBufferAssureMemoryNowFlag, &block);
    if (!status)
        status = CMBlockBufferReplaceDataBytes(e->avcc, block, 0, size);
    if (!status)
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, e->format, 1, 0, NULL, 1, &size, out);
    if (block)
        CFRelease(block);
    if (!status)
        *outSize = size;
    return status;
}

// Called with e->lock held; returns with e->lock released.
static void drainTilesLocked(AVEEmitter *e)
{
    const pthread_t self = pthread_self();
    if ((e->draining && !pthread_equal(e->drainer, self)) || !e->head || !e->head->done) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    e->draining++;
    e->drainer = self;
    e->references++;
    while (e->head && e->head->done) {
        PendingTile *t = e->head;
        e->head = t->next;
        if (!e->head)
            e->tail = NULL;
        const VTTileEncoderSession session = e->closed ? NULL : e->tileSession;
        const AVETileStats stats = e->stats;
        void *statsContext = e->statsContext;
        pthread_mutex_unlock(&e->lock);

        CMSampleBufferRef sample = NULL;
        size_t size = 0;
        const OSStatus status = session ? buildTileSample(e, t, &size, &sample) : t->immediateError;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        const uint64_t finished = mach_absolute_time();
        if (stats && t->slotIndex >= 0)
            stats(statsContext, status, size, t->copied - t->began, finished - t->copied);
#else
        if (stats && t->slotIndex >= 0)
            stats(statsContext, status, size, 0, 0);
#endif
        if (session)
            VTEncoderSessionEmitEncodedTile(session, t->tile, status, status ? kVTEncodeInfo_FrameDropped : 0, sample);
        if (sample)
            CFRelease(sample);
        if (t->source)
            CVPixelBufferRelease(t->source);

        pthread_mutex_lock(&e->lock);
        if (t->slotIndex >= 0 && (uint32_t)t->slotIndex < e->slotCount)
            e->slotBusy[t->slotIndex] = false;
        if (t->sequence > e->emittedThrough)
            e->emittedThrough = t->sequence;
        pthread_cond_broadcast(&e->cond);
        freeTileLocked(e, t);
    }
    e->draining--;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    dropUnlocked(e);
}

AVEEmitter *ave_emitter_create(void)
{
    AVEEmitter *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->queue = dispatch_queue_create("com.brunostjohn.switch.ave.completions", DISPATCH_QUEUE_SERIAL);
    if (!e->queue) {
        free(e);
        return NULL;
    }
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->cond, NULL);
    e->references = 1;
    for (uint32_t i = 0; i < kTilePool; i++) {
        e->tilePool[i].next = e->freeTiles;
        e->freeTiles = &e->tilePool[i];
    }
    return e;
}

void ave_emitter_close(AVEEmitter *e)
{
    if (!e)
        return;
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&e->lock);
    e->closed = true;
    e->tileSession = NULL;
    pthread_cond_broadcast(&e->cond);
    while (e->draining && !pthread_equal(e->drainer, self))
        pthread_cond_wait(&e->cond, &e->lock);
    pthread_mutex_unlock(&e->lock);
}

void ave_emitter_destroy(AVEEmitter *e)
{
    if (!e)
        return;
    ave_emitter_close(e);
    dropUnlocked(e);
}

dispatch_queue_t ave_emitter_queue(const AVEEmitter *e)
{
    return e->queue;
}

void ave_emitter_completed(void *emitter, uint32_t fence, int result, uint32_t error_status,
                           uint32_t ucode_error_status, uint32_t total_bit_count)
{
    AVEEmitter *e = emitter;
    const AVEJobStatus status = {result, error_status, ucode_error_status, total_bit_count};
    bool matchedTile = false;
    pthread_mutex_lock(&e->lock);
    for (PendingTile *t = e->head; t; t = t->next) {
        if (t->slotIndex >= 0 && t->fence == fence && !t->done) {
            t->done = true;
            t->jobStatus = status;
            matchedTile = true;
            break;
        }
    }
    if (!matchedTile)
        recordCompletedLocked(e, fence, status);
    pthread_cond_broadcast(&e->cond);
    if (matchedTile) {
        drainTilesLocked(e);
        return;
    }
    pthread_mutex_unlock(&e->lock);
}

bool ave_emitter_await(AVEEmitter *e, uint32_t fence, AVEJobStatus *out)
{
    pthread_mutex_lock(&e->lock);
    CompletedJob *found = NULL;
    while (!(found = findCompletedLocked(e, fence)) && !e->closed)
        pthread_cond_wait(&e->cond, &e->lock);
    if (!found) {
        pthread_mutex_unlock(&e->lock);
        return false;
    }
    *out = found->status;
    found->valid = false;
    pthread_mutex_unlock(&e->lock);
    return true;
}

bool ave_emitter_configure_tiles(AVEEmitter *e, VTTileEncoderSession session, CMVideoFormatDescriptionRef format,
                                 const AVETileSlot *slots, uint32_t slotCount, uint32_t bitstreamBytes,
                                 AVETileStats stats, void *statsContext)
{
    if (!e || !slotCount || slotCount > AVE_TILE_SLOTS)
        return false;
    const size_t cap = (size_t)bitstreamBytes + 4096;
    uint8_t *avcc = malloc(cap);
    if (!avcc)
        return false;
    pthread_mutex_lock(&e->lock);
    free(e->avcc);
    e->avcc = avcc;
    e->avccCap = cap;
    e->tileSession = session;
    if (format)
        CFRetain(format);
    if (e->format)
        CFRelease(e->format);
    e->format = format;
    memcpy(e->slots, slots, slotCount * sizeof(slots[0]));
    memset(e->slotBusy, 0, sizeof(e->slotBusy));
    e->slotCount = slotCount;
    e->bitstreamBytes = bitstreamBytes;
    e->stats = stats;
    e->statsContext = statsContext;
    pthread_mutex_unlock(&e->lock);
    return true;
}

int ave_emitter_acquire_tile_slot(AVEEmitter *e)
{
    pthread_mutex_lock(&e->lock);
    for (;;) {
        if (e->closed) {
            pthread_mutex_unlock(&e->lock);
            return -1;
        }
        for (uint32_t i = 0; i < e->slotCount; i++) {
            if (!e->slotBusy[i]) {
                e->slotBusy[i] = true;
                pthread_mutex_unlock(&e->lock);
                return (int)i;
            }
        }
        pthread_cond_wait(&e->cond, &e->lock);
    }
}

void ave_emitter_release_tile_slot(AVEEmitter *e, int slotIndex)
{
    if (!e || slotIndex < 0)
        return;
    pthread_mutex_lock(&e->lock);
    if ((uint32_t)slotIndex < e->slotCount)
        e->slotBusy[slotIndex] = false;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
}

void ave_emitter_push_tile(AVEEmitter *e, VTEncoderTile tile, CVPixelBufferRef source, int slotIndex, uint32_t fence,
                           uint64_t began, uint64_t copied, OSStatus immediateError)
{
    pthread_mutex_lock(&e->lock);
    PendingTile *t = allocTileLocked(e);
    if (!t) {
        if (slotIndex >= 0 && (uint32_t)slotIndex < e->slotCount) {
            e->slotBusy[slotIndex] = false;
            pthread_cond_broadcast(&e->cond);
        }
        pthread_mutex_unlock(&e->lock);
        if (source)
            CVPixelBufferRelease(source);
        return;
    }
    t->tile = tile;
    t->source = source;
    t->began = began;
    t->copied = copied;
    t->fence = fence;
    t->slotIndex = slotIndex;
    t->immediateError = immediateError;
    t->done = slotIndex < 0;
    t->sequence = ++e->pushed;
    if (!t->done) {
        CompletedJob *early = findCompletedLocked(e, fence);
        if (early) {
            t->done = true;
            t->jobStatus = early->status;
            early->valid = false;
        }
    }
    if (e->tail)
        e->tail->next = t;
    else
        e->head = t;
    e->tail = t;
    if (e->head->done) {
        drainTilesLocked(e);
        return;
    }
    pthread_mutex_unlock(&e->lock);
}

void ave_emitter_flush_tiles(AVEEmitter *e)
{
    if (!e)
        return;
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&e->lock);
    const uint64_t target = e->pushed;
    while (!e->closed && e->emittedThrough < target) {
        if (e->head && e->head->done && (!e->draining || pthread_equal(e->drainer, self))) {
            drainTilesLocked(e);
            pthread_mutex_lock(&e->lock);
            continue;
        }
        pthread_cond_wait(&e->cond, &e->lock);
    }
    pthread_mutex_unlock(&e->lock);
}
