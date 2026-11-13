// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-1 and MPEG-2 (4:2:0, Simple and Main profiles, frame and field pictures) on NVDEC 2.0. NVDEC decodes the
// slices; this keeps the sequence, the two reference frames and the field pairing, and fills nvdec_mpeg2_pic_s. The
// buffer set (status, slice offsets, bitstream: no history or colocated buffer), the method stream and the bitstream's
// end sequence follow averne's FFmpeg nvtegra hwaccel (libavcodec/nvtegra_mpeg12.c, GPL; read, not copied); the quant
// matrices go in raster order, as FFmpeg's CUVID hwaccel (nvdec_mpeg12.c) hands them to NVIDIA's driver.
//
// A field picture's two fields go into one surface (the second with secondfield set) and the frame goes out once,
// marked as two fields; a sample whose pictures complete no frame puts nothing out. A picture whose references are
// missing (a stream joined after its I-picture) is skipped.
#ifndef MPEG2_NVDEC_H
#define MPEG2_NVDEC_H
#include "H264NVDEC.h"
#include "MPEG2Syntax.h"

typedef struct MPEG2Decoder MPEG2Decoder;

MPEG2Decoder *mpeg2_decoder_create(const H264Backend *backend);
void mpeg2_decoder_destroy(MPEG2Decoder *decoder);

// Buffers for width x height (the format's, or its sequence header's): 0, or -3 when NVDEC buffers could not be had.
// A sequence header in a sample may change the size; a larger one reallocates.
int mpeg2_decoder_configure(MPEG2Decoder *decoder, uint32_t width, uint32_t height);
// The sequence the decoder has seen last (valid once a sequence header was parsed).
const MPEG2Sequence *mpeg2_decoder_sequence(const MPEG2Decoder *decoder);
// The sequence header and extensions of the format description, before the first sample.
int mpeg2_decoder_set_sequence(MPEG2Decoder *decoder, const MPEG2Sequence *seq);

// One sample. 1 when a frame went out, 0 when the sample completed none, -1 for unsupported or corrupt data, -2 when an
// NVDEC job failed, -3 when buffers for a new geometry failed.
int mpeg2_decoder_decode(MPEG2Decoder *decoder, const uint8_t *data, size_t size, void *token);

#endif
