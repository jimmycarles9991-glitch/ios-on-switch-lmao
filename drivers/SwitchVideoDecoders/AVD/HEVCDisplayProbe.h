// SPDX-License-Identifier: BSD-2-Clause-Patent
// What NVDEC 2.0 writes into its display buffer when a Main 10 picture is decoded in disp_output_mode 2 (Rec.709 10 ->
// 8 bit) or 3 (Rec.2020 10 -> 8 bit), against the same picture decoded in mode 1 (P010: the 10 bits high). The AVD
// plug-in's debug builds run it once per session that asks (HW Test decode.display_buffer): the session's first IDR
// picture is decoded again in each mode with its own slot pointed at scratch, so the real picture and every later
// one are untouched. Which layout the display buffer has (block linear like the pictures, or pitch; at the 8-bit or
// the 16-bit row) is unknown, so each is tried and the closest kept. Mode 3's luma moved several codes off the
// truncation on every boot (a colour conversion, not a rounding), so the closest layout is also scored against what
// three Rec.2020 -> Rec.709 conversions of the mode-1 picture would write.
#ifndef HEVC_DISPLAY_PROBE_H
#define HEVC_DISPLAY_PROBE_H
#include <stdbool.h>
#include <stdint.h>
#include "H264NVDEC.h"

enum {
    HEVC_DISPLAY_NOT_WRITTEN = 0,
    HEVC_DISPLAY_BLOCK_8 = 1,  // block linear, rows of the 8-bit picture's pitch
    HEVC_DISPLAY_PITCH_8 = 2,
    HEVC_DISPLAY_BLOCK_16 = 3,  // block linear at the 16-bit picture's pitch
    HEVC_DISPLAY_PITCH_16 = 4,
    HEVC_DISPLAY_LAYOUTS = 5,
};

enum {
    HEVC_DISPLAY_MODEL_GAMMA = 0,   // the Rec.2020 primaries to Rec.709's by a 3x3 on R'G'B' (no linearisation)
    HEVC_DISPLAY_MODEL_LINEAR = 1,  // the same 3x3 on linear light (BT.1886's 2.4 both ways)
    HEVC_DISPLAY_MODEL_MATRIX = 2,  // the Rec.2020 matrix in, the Rec.709 matrix out, primaries kept
    HEVC_DISPLAY_MODELS = 3,
};

typedef struct {
    int status;                         // 0, or the submit/wait error
    uint32_t layout;                    // HEVC_DISPLAY_*: the closest reading of the luma plane
    uint32_t max_diff;                  // over the visible luma: |display - (ref10 >> 2)|
    uint32_t mean_milli;                // its mean, x1000
    uint32_t trunc_milli, round_milli;  // per mille equal to ref10 >> 2 and to (ref10 + 2) >> 2 (clamped)
    uint32_t model_max[HEVC_DISPLAY_MODELS], model_mean_milli[HEVC_DISPLAY_MODELS];  // |display - model luma|
} HEVCDisplayProbeResult;

// The 8-bit limited-range luma `model` writes for 10-bit limited-range Y'CbCr read as Rec.2020 (non-constant
// luminance).
uint8_t hevc_display_model_luma(uint32_t model, uint32_t y10, uint32_t cb10, uint32_t cr10);

// The closest reading of `display` (poisoned with HEVC_DISPLAY_POISON before the job) against the mode-1 picture
// `ref16` (block linear, `pitch16` bytes a row: `rows` rows of luma, then the interleaved chroma's half as many):
// fills everything but `status`.
#define HEVC_DISPLAY_POISON 0xa5
void hevc_display_probe_compare(const uint8_t *display, uint64_t displayBytes, const uint8_t *ref16,
                                const uint8_t *chroma16, uint32_t pitch16, uint32_t rows, uint32_t width,
                                uint32_t height, HEVCDisplayProbeResult *out);

// Decodes `job` (the mode-1 submit just run, curr_pic_idx's relocations included) again in `mode`, the current slot
// on scratch and a poisoned display buffer bound, and compares. `ref16` is that slot's picture as mode 1 wrote it, its
// chroma `lumaBytes` on.
void hevc_display_probe_run(const H264Backend *b, const TegraNVDECSubmit *job, const TegraNVDECWait *wait,
                            uint32_t surfaceBytes, uint32_t lumaBytes, const uint8_t *ref16, uint32_t pitch16,
                            uint32_t rows, uint32_t width, uint32_t height, uint32_t mode, HEVCDisplayProbeResult *out);

#endif
