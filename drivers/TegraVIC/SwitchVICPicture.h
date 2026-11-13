// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct switch_gpu_backing;

typedef struct {
    struct switch_gpu_backing *backing;
    const uint64_t *pages;  // the picture's memory, 4 KiB pages in order (an NVDEC buffer's page list)
    uint32_t pageCount;
    uint64_t bytes;
    uint32_t pitch, rows;      // bytes per row (whole 64-byte GOBs), luma rows (whole 32-row blocks pairs)
    uint32_t chromaOffset;     // the CbCr plane, 256-byte aligned, rows / 2 rows of pitch bytes
    uint32_t blockHeightLog2;  // GOBs a block: NVDEC's GOB_2 is 1
    uint32_t cropX, cropY;     // the displayed window's corner (even), in samples
    uint32_t width, height;    // its size (even); clipped to the destination's
    uint32_t dstX, dstY;       // where in the destination it goes (even; dstX a multiple of 256 bytes but 0)
    uint32_t surfaceID;        // a '420v' or '420f' IOSurface of `task`; with 16-bit samples 'x420' or 'xf20'
    uint32_t sampleBytes;      // 1, or 2 for 16-bit samples (NVDEC's Main 10, P010)
    uint32_t plane;            // TegraNVDECPresent.plane: 0, or 2 for the luma alone into a 'v0a8' alpha plane
} TegraVICPicture;
