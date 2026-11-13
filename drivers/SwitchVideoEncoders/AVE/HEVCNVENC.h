// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.265 on NVENC 5.0 (the firmware's app 3): the picture setup and method stream for IDR and P pictures, as NVIDIA's
// T210 driver builds them (TegraNVENC/nvidia/SOURCE-H265). NVENC writes each slice's NAL header and slice segment
// header itself from the setup; the parameter sets are HEVCEncSyntax.c's, and hevcnvenc_configure gives its config
// the coding tools the setup states, so the two agree.
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

// Buffer sizes for a session (the H264EncLayout fields, for 32-aligned coded sizes), the input in `input_layout`.
void hevcnvenc_layout(const HEVCEncConfig *config, uint32_t input_layout, H264EncLayout *layout);

// The submit for one picture (TegraNVENCCheckH265.h accepts it). `input_layout` and `chroma_mode` are
// TegraNVENCInfo's; `picture->qp` is the picture's constant QP.
//
// An IDR has `poc` 0. A P picture predicts from the picture before it, nothing else: `poc` counts on by one from the
// IDR (the SPS's one reference picture set is delta POC -1), `handles->ref` holds that picture's reconstruction and
// this picture's goes to `handles->recon`, so the caller swaps the two after every picture. The two ME predictor
// buffers alternate by POC (P pictures write `mepred[poc & 1]` and read the other). Any picture may be an IDR; a
// picture after a failed one must be. Buffer sizes are hevcnvenc_layout's: ref and recon `recon.bytes` each, the ME
// predictors `mepred_bytes` each.
void hevcnvenc_fill_submit(const HEVCEncConfig *config, uint32_t input_layout, uint32_t chroma_mode,
                           const H264EncLayout *layout, const HEVCEncPicture *picture, const H264EncHandles *handles,
                           TegraNVENCSubmit *submit);

#endif
