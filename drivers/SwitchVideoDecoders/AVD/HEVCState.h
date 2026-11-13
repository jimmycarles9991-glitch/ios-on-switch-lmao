// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVCNVDEC's decoder state, shared by HEVCNVDEC.c (DPB, output, jobs) and HEVCSetup.c (NVDEC's picture setup and
// its tile and scaling tables).
#ifndef HEVC_STATE_H
#define HEVC_STATE_H
#include "HEVCNVDEC.h"
#include "NVDECRing.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

#define SLOTS        16   // NVDEC's picture slots; the DPB (current picture included) never holds more
#define MAX_SEGMENTS 600  // slice segments in one picture
#define ALIGN(v, a)  (((v) + (a) - 1) / (a) * (a))

enum {
    kStatusOff = 0,
    kTilesOff = 256,  // tile sizes: 0x900 bytes (pairs from entry 0, boundaries from entry 0x380)
    kTilesBytes = 0x900,
    kScalingOff = kTilesOff + kTilesBytes,  // nvdec_hevc_scaling_list_s, 0x400 reserved
    kBitsOff = 0x1000,
    // Filter buffer bytes per 64-aligned row: deblocking, SAO, BSD control (averne's FILTER/SAO/BSD_SIZE, 8-bit).
    // Deblocking and SAO hold pixels, so a 16-bit picture gets twice theirs: the engine writes within the offsets it
    // is given, and room it does not use costs nothing.
    kDeblockBytesPerRow = 480,
    kSaoBytesPerRow = 3840,
    kBsdBytesPerRow = 60,
};

typedef struct {
    bool used;                    // in the DPB
    bool ref, long_term, output;  // marking; needed for output
    int32_t poc;
    uint32_t latency;  // PicLatencyCount
    void *item;        // the decoded picture, waiting to be bumped out
    uint32_t handle;   // its surface (kept while the slot is free, for the next picture)
    uint8_t *va;
    uint32_t display;  // with display8: the picture's 8-bit copy, what goes out
    uint8_t *display_va;
} Entry;

struct HEVCDecoder {
    H264Backend b;
    HEVCParams params;
    int first_sps;
    bool allow10, display8;
    Entry dpb[SLOTS];
    uint32_t width, height, pitch, rows, luma_bytes, surface_bytes;  // the surfaces' geometry (pitch in bytes)
    uint32_t sample_bytes;                                           // 2 for Main 10: 16-bit samples
    uint32_t aux, colmv_bytes, filter_off, aux_bytes;
    uint8_t *aux_va;
    NVDECRing ring;  // each picture's status record, tile sizes, scaling lists and bitstream
    bool decoded_any, stream_start, skip_rasl, have_prev;
    int32_t prev_tid0_poc, last_poc;
    uint32_t picture_index;
    unsigned missing;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool probe_wanted, probe_ran;
    HEVCDisplayProbeResult probe[2];
#endif
};

typedef struct {
    int8_t before[16], after[16], lt[16];  // RefPicSetStCurrBefore / StCurrAfter / LtCurr as slots (-1: missing)
    uint8_t nb, na, nl;
} RefSets;

static inline bool hevc_display_buffer(const HEVCDecoder *d)
{
    return d->display8 && d->sample_bytes == 2;
}

bool hevc_tile_grid(const HEVCSPS *sps, const HEVCPPS *pps, uint16_t *col, uint16_t *row);
void hevc_tile_table(uint16_t *t, const HEVCSPS *sps, const HEVCPPS *pps, const uint16_t *col, const uint16_t *row);
void hevc_scaling_table(nvdec_hevc_scaling_list_s *out, const HEVCScalingList *sl);
void hevc_fill_setup(const HEVCDecoder *d, nvdec_hevc_pic_s *p, const HEVCSPS *sps, const HEVCPPS *pps,
                     const HEVCSliceHeader *sh, int cur, int32_t poc, const RefSets *r, int scratch, uint32_t length);

#endif
