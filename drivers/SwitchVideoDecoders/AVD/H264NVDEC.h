// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 on NVDEC 2.0: the decoder state a hardware "VLD" engine leaves to the driver. NVDEC parses the slices itself;
// this keeps the parameter sets, picture order counts (8.2.1), reference marking (8.2.5, frames only), the engine's
// DPB table and surfaces, fills nvdec_h264_pic_s, and puts decoded pictures out in display order (C.4.5.3 bumping
// with max_num_reorder_frames). Plain C over a small backend, so the host tests drive it without a kernel.
//
// Layouts follow averne's FFmpeg nvtegra hwaccel (libavcodec/nvtegra_h264.c, the decoder NXMP uses on the Switch;
// GPL, read, not copied) and NVIDIA's nvdec_drv.h / clc5b0.h (MIT, vendored in drivers/TegraNVDEC/nvidia).
// Limits: progressive frames only (frame_mbs_only_flag = 1), 4:2:0 8-bit, one slice group, at most 4096x4096.
// Anything else is refused at configuration time.
#ifndef H264_NVDEC_H
#define H264_NVDEC_H
#include "H264Syntax.h"
#include "../../TegraNVDEC/TegraNVDECABI.h"

typedef struct {
    const uint8_t *luma, *chroma;  // block-linear NV12 (16Bx2 sectors, GOB height 2), CPU-visible
    uint32_t pitch;                // bytes per row of both planes, a multiple of 64
    uint32_t handle;  // the NVDEC buffer holding it (TegraNVDECPresent): luma at 0, chroma at chroma_offset
    uint32_t chroma_offset, rows;  // rows: the planes' block-aligned luma rows (a multiple of 32)
    uint32_t coded_width, coded_height;
    uint32_t crop_x, crop_y, width, height;  // the displayed window, in samples
    bool wide;                               // 16-bit samples (HEVC Main 10: P010, laid out as 'x420')
    int32_t poc;
    int status;  // 0, or the engine's error_status when nothing decoded
    void *token;
} H264Picture;

typedef struct {
    void *ctx;
    int (*alloc)(void *ctx, uint64_t size, uint32_t *handle, void **va);
    void (*free)(void *ctx, uint32_t handle);
    int (*submit)(void *ctx, const TegraNVDECSubmit *submit, uint32_t *fence);
    int (*wait)(void *ctx, const TegraNVDECWait *wait);
    void *(*decoded)(void *ctx, const H264Picture *picture);  // an output item for `emit`, or NULL to drop it
    void (*emit)(void *ctx, void *item);                      // display order
    void (*discard)(void *ctx, void *item);                   // dropped without output (no_output_of_prior_pics)
    // Whether `decoded` reads the picture on the CPU (else VIC copies it, TegraNVDECPresent): only then does the job's
    // Wait invalidate the surface. NULL: it does.
    bool (*cpu_reads)(void *ctx);
} H264Backend;

typedef struct H264Decoder H264Decoder;

H264Decoder *h264_decoder_create(const H264Backend *backend);
void h264_decoder_destroy(H264Decoder *decoder);

// Parameter sets from avcC (or in-band); then check the stream is decodable here and allocate for it.
int h264_decoder_parameter_set(H264Decoder *decoder, const uint8_t *nal, size_t size);
// 0; -1 for a stream this decoder cannot take; -3 when NVDEC buffers could not be allocated or mapped.
int h264_decoder_configure(H264Decoder *decoder);
const H264SPS *h264_decoder_sps(const H264Decoder *decoder);  // the configured SPS (after configure)

// One access unit of length-prefixed NAL units (avcC lengthSizeMinusOne + 1 bytes each). Returns 1 when a picture was
// decoded (it reaches `decoded` before this returns, and `emit` when its turn comes), 0 when the unit held none,
// -1 for bad or unsupported data, -2 when the NVDEC job failed, -3 when buffers for a new geometry failed.
// `reorder` false puts every picture out as soon as it is decoded (no temporal processing).
int h264_decoder_decode(H264Decoder *decoder, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder);

// Puts out every picture still waiting for its display turn.
void h264_decoder_flush(H264Decoder *decoder);

#endif
