// SPDX-License-Identifier: BSD-2-Clause-Patent
// Async completion waiter and ordered HEVC tile pipeline for AVE. Driven by NVENCListener's serial completion queue
// (Mach notification port or single-Wait fallback); never holds Encoder::lock across waits or VideoToolbox callbacks.
#ifndef AVE_EMITTER_H
#define AVE_EMITTER_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include "VTEncoderSPI.h"

enum {
    AVE_TILE_SLOTS = 4,
};

typedef struct AVEEmitter AVEEmitter;

typedef struct {
    int result;
    uint32_t error_status;
    uint32_t ucode_error_status;
    uint32_t total_bit_count;
} AVEJobStatus;

typedef struct {
    uint32_t inputHandle, bitstreamHandle, auxHandle;
    uint8_t *inputVA, *bitstreamVA, *auxVA;
} AVETileSlot;

typedef void (*AVETileStats)(void *context, OSStatus status, size_t bytes, uint64_t copyTicks, uint64_t encodeTicks);

AVEEmitter *ave_emitter_create(void);
void ave_emitter_close(AVEEmitter *emitter);
void ave_emitter_destroy(AVEEmitter *emitter);
dispatch_queue_t ave_emitter_queue(const AVEEmitter *emitter);

void ave_emitter_completed(void *emitter, uint32_t fence, int result, uint32_t error_status,
                           uint32_t ucode_error_status, uint32_t total_bit_count);
bool ave_emitter_await(AVEEmitter *emitter, uint32_t fence, AVEJobStatus *out);

bool ave_emitter_configure_tiles(AVEEmitter *emitter, VTTileEncoderSession session, CMVideoFormatDescriptionRef format,
                                 const AVETileSlot *slots, uint32_t slotCount, uint32_t bitstreamBytes,
                                 AVETileStats stats, void *statsContext);
int ave_emitter_acquire_tile_slot(AVEEmitter *emitter);
void ave_emitter_release_tile_slot(AVEEmitter *emitter, int slotIndex);
void ave_emitter_push_tile(AVEEmitter *emitter, VTEncoderTile tile, CVPixelBufferRef source, int slotIndex,
                           uint32_t fence, uint64_t began, uint64_t copied, OSStatus immediateError);
void ave_emitter_flush_tiles(AVEEmitter *emitter);

#endif
