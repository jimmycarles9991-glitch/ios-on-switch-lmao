// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 on NVDEC 2.0: the decoder state a hardware "VLD" engine leaves to the driver. NVDEC parses the slices itself;
// this keeps the parameter sets, the surfaces, the picture bookkeeping of H264DPB.h (order counts, reference marking
// per field, the engine's DPB table), fills nvdec_h264_pic_s, and puts decoded frames out in display order (C.4.5.3
// bumping with max_num_reorder_frames). Interlaced streams: an MBAFF or field-coded frame picture is one job; a field
// pair is two jobs into one surface, and its frame goes out once, after the second field (or alone, as the frame of a
// field nothing pairs with). Plain C over a small backend, so the host tests drive it without a kernel.
//
// Layouts follow averne's FFmpeg nvtegra hwaccel (libavcodec/nvtegra_h264.c, the decoder NXMP uses on the Switch;
// GPL, read, not copied) and NVIDIA's nvdec_drv.h / clc5b0.h (MIT, vendored in drivers/TegraNVDEC/nvidia).
// Limits: 4:2:0 8-bit, one slice group, at most 4096x4096. Anything else is refused at configuration time.
#ifndef H264_NVDEC_H
#define H264_NVDEC_H
#include "H264Syntax.h"
#include "../../TegraNVDEC/TegraNVDECABI.h"

// How a picture's rows interleave in time (kCVImageBufferFieldCountKey and FieldDetailKey), where the codec says.
enum {
    AVD_FIELDS_UNSAID = 0,
    AVD_FIELDS_PROGRESSIVE = 1,
    AVD_FIELDS_TOP_FIRST = 2,
    AVD_FIELDS_BOTTOM_FIRST = 3
};

typedef struct {
    const uint8_t *luma, *chroma;  // block-linear NV12 (16Bx2 sectors, GOB height 2), CPU-visible
    uint32_t pitch;                // bytes per row of both planes, a multiple of 64
    uint32_t handle;  // the NVDEC buffer holding it (TegraNVDECPresent): luma at 0, chroma at chroma_offset
    uint32_t chroma_offset, rows;  // rows: the planes' block-aligned luma rows (a multiple of 32)
    uint32_t coded_width, coded_height;
    uint32_t crop_x, crop_y, width, height;  // the displayed window, in samples
    bool wide;                               // 16-bit samples (HEVC Main 10: P010, laid out as 'x420')
    uint8_t fields;                          // AVD_FIELDS_*
    int32_t poc;
    // The fence of the NVDEC job that decoded it in this call (its status goes with it), 0: none; and of a field
    // pair's first field, whose failure marks the frame too.
    uint32_t job, first_field_job;
    bool refused;  // a job of the frame was refused: it goes out marked bad
    void *token;
} H264Picture;

typedef struct {
    void *ctx;
    int (*alloc)(void *ctx, uint64_t size, uint32_t *handle, void **va);
    void (*free)(void *ctx, uint32_t handle);
    int (*submit)(void *ctx, const TegraNVDECSubmit *submit, uint32_t *fence);
    // Only for an output of the job the decoder itself must read (VP9's entropy counts): pictures never wait.
    int (*wait)(void *ctx, const TegraNVDECWait *wait);
    // An output item for `emit` made as soon as the picture's job is submitted, or NULL to drop it. Whoever reads the
    // picture waits for the job (the AVD plug-in's emitter); it is ordered after every job submitted before it.
    void *(*decoded)(void *ctx, const H264Picture *picture);
    void (*emit)(void *ctx, void *item);     // display order
    void (*discard)(void *ctx, void *item);  // dropped without output (no_output_of_prior_pics)
    int (*recycle)(void *ctx, uint32_t handle, uint64_t bytes, void **va);
    uint32_t inputs;  // input buffers to cycle (NVDECRing.h); 0 as 1
} H264Backend;

typedef struct H264Decoder H264Decoder;

H264Decoder *h264_decoder_create(const H264Backend *backend);
void h264_decoder_destroy(H264Decoder *decoder);

// Parameter sets from avcC (or in-band); then check the stream is decodable here and allocate for it.
int h264_decoder_parameter_set(H264Decoder *decoder, const uint8_t *nal, size_t size);
// 0; -1 for a stream this decoder cannot take; -3 when NVDEC buffers could not be allocated or mapped.
int h264_decoder_configure(H264Decoder *decoder);
const H264SPS *h264_decoder_sps(const H264Decoder *decoder);  // the configured SPS (after configure)

// One sample of length-prefixed NAL units (avcC lengthSizeMinusOne + 1 bytes each): a frame, or a field pair, or one
// field of a pair split over two samples. Returns 1 when the sample's token went to a frame whose jobs were submitted:
// it reaches `decoded` before this returns, or (a first field) when its second field or the next picture is
// submitted or at the flush, and `emit` when its turn comes. 0 when the sample started no frame (parameter sets, or
// only the second field of the previous sample's frame), -1 for bad or unsupported data, -2 when an NVDEC job was
// refused, -3 when buffers for a new geometry failed.
// `reorder` false puts every picture out as soon as it is decoded (no temporal processing).
int h264_decoder_decode(H264Decoder *decoder, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder);

// Puts out every picture still waiting for its display turn.
void h264_decoder_flush(H264Decoder *decoder);

#endif
