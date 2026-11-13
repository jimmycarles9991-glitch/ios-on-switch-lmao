// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-4 Part 2 (Simple and Advanced Simple, progressive) on NVDEC 2.0. NVDEC parses each VOP and decodes it; this
// keeps the VOL, the two reference surfaces, the VOP timing B-VOP direct mode needs (TRD and TRB, 7.7.2.2, kept as
// FFmpeg's decoder keeps them) and fills nvdec_mpeg4_pic_s. The buffer set, the method stream and the bitstream's
// end sequence follow averne's FFmpeg nvtegra hwaccel (libavcodec/nvtegra_mpeg4.c, GPL; read, not copied); the quant
// matrices go in raster order, as FFmpeg's CUVID hwaccel (nvdec_mpeg4.c) hands them to NVIDIA's driver.
//
// One sample, one picture out: every VOP in it is decoded (a packed bitstream's P- and B-VOP) and the last goes out; a
// VOP that is not coded (vop_coded 0) puts the newest reference out again without a job.
#ifndef MPEG4_NVDEC_H
#define MPEG4_NVDEC_H
#include "H264NVDEC.h"
#include "MPEG4Syntax.h"

typedef struct MPEG4Decoder MPEG4Decoder;

MPEG4Decoder *mpeg4_decoder_create(const H264Backend *backend);
void mpeg4_decoder_destroy(MPEG4Decoder *decoder);

// The VOL a session starts with, and buffers for its size: 0, -1 when NVDEC does not take it
// (mpeg4_vol_unsupported), -3 when NVDEC buffers could not be allocated.
int mpeg4_decoder_configure(MPEG4Decoder *decoder, const MPEG4VOL *vol);
const MPEG4VOL *mpeg4_decoder_vol(const MPEG4Decoder *decoder);

// One sample. 1 when a picture went out, 0 when the sample put none out, -1 for unsupported or corrupt data, -2 when an
// NVDEC job failed, -3 when buffers for a new geometry failed.
int mpeg4_decoder_decode(MPEG4Decoder *decoder, const uint8_t *data, size_t size, void *token);

#endif
