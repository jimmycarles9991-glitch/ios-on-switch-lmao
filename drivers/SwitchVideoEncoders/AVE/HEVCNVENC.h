// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.265 on NVENC 5.0 (the firmware's app 3): the picture setup and method stream for an IDR picture, as NVIDIA's T210
// driver builds them (TegraNVENC/nvidia/SOURCE-H265). The parameter sets around the slices are HEVCEncSyntax.c's;
// hevcnvenc_configure gives its config the coding tools the setup states, so the two agree. P pictures are not
// built here: their reference fields are not established (SOURCE-H265).
#ifndef HEVC_NVENC_H
#define HEVC_NVENC_H
#include <stdint.h>
#include "H264NVENC.h"
#include "HEVCEncSyntax.h"

enum {
    HEVCNVENC_CTB = 32,
};

// The coding tree and tools NVENC 5.0's setup describes (32x32 CTBs, 8x8 minimum CUs, 4x4..32x32 TUs, depth 3, AMP;
// no SAO, temporal MVP or strong intra smoothing), then hevcenc_configure. 0, or -1.
int hevcnvenc_configure(HEVCEncConfig *config);
uint32_t hevcnvenc_ctbs(const HEVCEncConfig *config);

// Buffer sizes for a session (the H264EncLayout fields, for 32-aligned coded sizes).
void hevcnvenc_layout(const HEVCEncConfig *config, H264EncLayout *layout);

// The submit for one IDR picture (TegraNVENCCheckH265.h accepts it). `input_layout` and `chroma_mode` are
// TegraNVENCInfo's; `picture->qp` is the picture's constant QP.
void hevcnvenc_fill_submit(const HEVCEncConfig *config, uint32_t input_layout, uint32_t chroma_mode,
                           const H264EncLayout *layout, const HEVCEncPicture *picture, const H264EncHandles *handles,
                           TegraNVENCSubmit *submit);

#endif
