// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC Main / Main 10 / Main Still Picture on NVDEC 2.0: the decoder state a VLD engine leaves to its driver. NVDEC
// parses every slice segment itself; this keeps the parameter sets, picture order counts (8.3.1), the reference
// picture sets and marking (8.3.2), the decoded picture buffer with output by bumping (C.5.2), NVDEC's 16 picture
// slots (a picture keeps its slot for its whole DPB life), the colocated motion, filter, tile size and scaling list
// buffers, and fills nvdec_hevc_pic_s. Main 10 is written in 16-bit samples (disp_output_mode 1, P010: the 10 bits
// high), into surfaces and filter rows twice as wide.
//
// NVDEC-specific facts (buffer set and sizes, method stream, tile size table layout, the 4-byte start code before each
// slice, sw_hdr_skip_length's extent) follow averne's FFmpeg nvtegra hwaccel (libavcodec/nvtegra_hevc.c and his
// hevcdec.c patch, GPL; read, not copied) and NVIDIA's nvdec_drv.h / clc5b0.h. Departures: surfaces have 64-aligned
// rows (whole CTUs; the kernel checker bounds them so), missing references point at a picture that exists instead
// of an uninitialised slot index, and RASL pictures of a CRA that starts the stream are not decoded (8.1.3).
#ifndef HEVC_NVDEC_H
#define HEVC_NVDEC_H
#include "H264NVDEC.h"
#include "HEVCDisplayProbe.h"
#include "HEVCSyntax.h"

typedef struct HEVCDecoder HEVCDecoder;

HEVCDecoder *hevc_decoder_create(const H264Backend *backend);
void hevc_decoder_destroy(HEVCDecoder *decoder);

// Parameter sets from hvcC (or in band). 0, or -1 for one this decoder cannot take.
int hevc_decoder_parameter_set(HEVCDecoder *decoder, const uint8_t *nal, size_t size);
// The first SPS (after the hvcC sets): the stream's geometry and profile, NULL before one parsed.
const HEVCSPS *hevc_decoder_sps(const HEVCDecoder *decoder);
// Buffers for the stream's first SPS (the session start, so a session NVDEC has no memory for goes to software now).
// 0, -1 without an SPS, -3 when NVDEC buffers could not be allocated.
int hevc_decoder_configure(HEVCDecoder *decoder);
// Whether Main 10 pictures are decoded (TEGRA_NVDEC_HEVC_MAIN10); 8-bit only when false.
void hevc_decoder_allow_10bit(HEVCDecoder *decoder, bool allow);
// Main 10 pictures go out as the 8-bit NV12 NVDEC writes into a display buffer in the same job (disp_output_mode 2: the
// samples truncated to 8 bits, HW Test decode.display_buffer; Rec.709 only), the references staying 16-bit.
void hevc_decoder_display_8bit(HEVCDecoder *decoder, bool on);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// HEVCDisplayProbe.h on the session's first Main 10 IDR picture; false until it has run.
void hevc_decoder_want_display_probe(HEVCDecoder *decoder);
bool hevc_decoder_display_probe(const HEVCDecoder *decoder, HEVCDisplayProbeResult out[2]);
#endif

// One access unit of length-prefixed NAL units (lengthSize 1..4). Returns 1 when a picture was decoded (it reaches
// `decoded` now and `emit` when it is bumped out, or at once when `reorder` is false), 0 when the sample had none to
// decode (parameter sets, a skipped RASL picture), -1 for unsupported or corrupt data, -2 when an NVDEC job failed, -3
// when buffers failed.
int hevc_decoder_decode(HEVCDecoder *decoder, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder);
// End of stream: every picture still waiting goes out, in output order.
void hevc_decoder_flush(HEVCDecoder *decoder);

// For the host test: the last picture's POC and its NVDEC slot, and how many reference slots named a missing picture.
int32_t hevc_decoder_last_poc(const HEVCDecoder *decoder);
unsigned hevc_decoder_missing_refs(const HEVCDecoder *decoder);

#endif
