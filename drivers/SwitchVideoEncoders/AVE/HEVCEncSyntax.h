// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC (ITU-T H.265) syntax an encoder writes around NVENC's slice data: the VPS, SPS and PPS, the hvcC record
// CoreMedia carries them in, and each picture's slice segment header. The writer counterpart of AVD's HEVCSyntax.c,
// which parses all of it back in the host test. Plain C with no Apple dependency.
//
// The stream: Main profile (4:2:0, 8 bits), progressive, one slice segment per picture, no B pictures (output order =
// decode order), each P picture predicting from the picture before it (one short-term reference picture set in the
// SPS: delta POC -1), an IDR (IDR_W_RADL) at every key frame, 8-bit POC LSB, transform skip and loop filtering
// across slices on (the PPS NVIDIA's T210 driver states). The coding tree sizes and the tools
// (AMP, SAO, temporal MVP, strong intra smoothing) are the config's, so the SPS states whatever NVENC's setup is made
// to do.
#ifndef HEVC_ENC_SYNTAX_H
#define HEVC_ENC_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "H264NVENC.h"

enum {
    HEVCENC_LOG2_MAX_POC_LSB = 8,  // what NVENC 5.0's H.265 setup states (TegraNVENC/nvidia/SOURCE-H265)
};

typedef struct {
    uint32_t width, height;  // displayed size; the coded size is whole CTBs (what NVENC codes), cropped in the SPS
    uint8_t level_idc;       // general_level_idc (30 x the level); 0: chosen from the size and frame rate
    uint8_t log2_ctb, log2_min_cb, log2_min_tb, log2_max_tb;
    uint8_t max_tr_depth_inter, max_tr_depth_intra;
    bool amp, sao, temporal_mvp, strong_intra_smoothing;
    bool full_range, colour_description;
    uint8_t colour_primaries, transfer, matrix;  // H.273 code points
    uint32_t fps_num, fps_den;                   // 0/0: no timing information in the VUI
} HEVCEncConfig;

typedef struct {
    bool idr;                // IDR_W_RADL, an I slice; else TRAIL_R, a P slice predicting from the picture before
    uint32_t poc;            // picture order count since the last IDR (0 at the IDR)
    uint8_t qp;              // slice QP, 0..51
    uint32_t picture_index;  // pictures since the session began, IDRs included (NVENC's SET_PICTURE_INDEX)
} HEVCEncPicture;

// Fills unset tree sizes with the defaults, picks level_idc when it is 0, and checks what this code can describe: 0,
// or -1.
int hevcenc_configure(HEVCEncConfig *config);
uint32_t hevcenc_coded_width(const HEVCEncConfig *config);
uint32_t hevcenc_coded_height(const HEVCEncConfig *config);

// NAL units without start codes (two-byte header, emulation prevention added). Return the size, 0 if `cap` is too
// small.
size_t hevcenc_write_vps(const HEVCEncConfig *config, uint8_t *out, size_t cap);
size_t hevcenc_write_sps(const HEVCEncConfig *config, uint8_t *out, size_t cap);
size_t hevcenc_write_pps(const HEVCEncConfig *config, uint8_t *out, size_t cap);
// ISO/IEC 14496-15 HEVCDecoderConfigurationRecord with that VPS, SPS and PPS (4-byte NAL lengths).
size_t hevcenc_write_hvcc(const HEVCEncConfig *config, uint8_t *out, size_t cap);
// The slice segment header (7.3.6.1 with these parameter sets) up to its byte alignment, as RBSP bits.
void hevcenc_slice_header(const HEVCEncConfig *config, const HEVCEncPicture *picture, H264Bits *b);
// The NAL unit type a picture's slice segment takes.
uint8_t hevcenc_nal_type(const HEVCEncPicture *picture);

#endif
