// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 on NVENC 5.0: what a hardware encoder leaves to the driver. NVENC writes the slices (header and data); this
// writes the parameter sets (SPS, PPS, avcC), fills NVENC's picture setup (nvenc_drv.h) and method stream for each
// picture, lays the input picture out the way the firmware reads it, and turns NVENC's Annex B output into the
// length-prefixed samples CoreMedia carries. Plain C with no Apple dependency: the host tests and the generator of
// the kernel's E3 vectors build it too.
//
// The stream it describes: progressive 4:2:0 8-bit, one slice per picture, IDR at every key frame,
// pic_order_cnt_type 0 with frame_num and POC LSB of 16 bits. Baseline (constrained), Main or High profile; CAVLC or
// CABAC (not with Baseline); the 8x8 transform with High. Without reordering every picture is a reference and each P
// picture predicts from the one before (output order = decode order). With it (H264EncConfig.reorder, not Baseline),
// as NVIDIA's T210 driver codes B pictures (TegraNVENC/nvidia/SOURCE): anchors (IDR, P) are references, each P
// predicting from the anchor before; the B pictures between two anchors in display order follow the later one in
// decode order, are not references, and predict from both (spatial direct, the later anchor's colocated motion), so
// two reference frames and one picture of reordering.
#ifndef H264_NVENC_H
#define H264_NVENC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../TegraNVENC/TegraNVENCABI.h"

enum {
    H264_PROFILE_BASELINE = 66,
    H264_PROFILE_MAIN = 77,
    H264_PROFILE_HIGH = 100
};

enum {
    H264ENC_IDR = 3,
    H264ENC_I = 2,
    H264ENC_B = 1,
    H264ENC_P = 0
};  // nvenc_h264_pic_control_s pic_type

typedef struct {
    uint32_t width, height;          // displayed size; the coded size is whole macroblocks, cropped in the SPS
    uint8_t profile_idc, level_idc;  // level_idc 0: chosen from the size and frame rate
    bool cabac, transform8x8;
    bool full_range, colour_description;
    uint8_t colour_primaries, transfer, matrix;  // H.273 code points
    uint32_t fps_num, fps_den;                   // 0/0: no timing information in the VUI
    uint32_t input_layout, chroma_mode;          // TegraNVENCInfo: what E3 proved
    bool reorder;                                // B pictures (not with Baseline)
} H264EncConfig;

// One picture buffer: luma at offset 0, chroma `chroma_offset` bytes in (TegraNVENCCheck.h's bounds).
typedef struct {
    uint32_t pitch, rows, chroma_offset, bytes;
} H264EncSurface;

// Buffers a session allocates once (sizes from h264enc_layout).
typedef struct {
    H264EncSurface input, recon;
    uint32_t bitstream_bytes;  // one picture's output; 64 KiB multiple
    uint32_t history_bytes, status_bytes;
    uint32_t aux_bytes, history_offset;  // status at 0 of the aux buffer, then history
    uint32_t mepred_bytes;               // each of the two ME predictor buffers
    uint32_t coloc_bytes;                // a picture's colocated motion (reordering sessions)
} H264EncLayout;

typedef struct {
    uint8_t type;  // H264ENC_IDR, H264ENC_I, H264ENC_P or H264ENC_B
    uint8_t qp;    // 0..51 (constant QP: the plug-in's rate control picks it per picture)
    uint32_t frame_num, idr_pic_id, poc_lsb, picture_index;
    // The reference slots of list 0 and list 1 (a B picture's both), and how many pictures lie between this one and
    // each in display order (0: adjacent).
    uint8_t ref_slot[2], gap[2];
    bool unreferenced;  // an I picture no other predicts from, as a B picture never is a reference
} H264EncPicture;

// Handles of the buffers a picture uses. Without reordering a P picture reads `ref` (slot 0) and writes `recon`, and
// the next swaps them. With it `ref` and `ref1` are slots 0 and 1 (0: not named), every picture writes its colocated
// motion to `coloc_out`, and a B picture reads its list-1 reference's from `coloc_in`. The ME predictors alternate by
// picture index: a picture reads what the one before it wrote.
typedef struct {
    uint32_t input, recon, ref, bitstream, aux, mepred[2];
    uint32_t ref1, coloc_out, coloc_in;
} H264EncHandles;

// An RBSP bit writer (no emulation prevention: h264enc_nal adds it).
typedef struct {
    uint8_t *data;
    size_t cap, bytes;
    uint32_t acc;
    unsigned count;
    bool overflow;
} H264Bits;

void h264bits_init(H264Bits *b, uint8_t *data, size_t cap);
void h264bits_u(H264Bits *b, uint32_t value, unsigned n);
void h264bits_ue(H264Bits *b, uint32_t value);
void h264bits_se(H264Bits *b, int32_t value);
bool h264bits_aligned(const H264Bits *b);
void h264bits_align_zero(H264Bits *b);    // pcm_alignment_zero_bit / cabac_alignment_one_bit style padding with 0s
void h264bits_trailing(H264Bits *b);      // rbsp_trailing_bits
size_t h264bits_size(const H264Bits *b);  // whole bytes written (after trailing bits)

// One NAL unit from its header byte and RBSP: emulation prevention added. Returns the size, 0 if `cap` is too small.
size_t h264enc_nal(uint8_t header, const uint8_t *rbsp, size_t size, uint8_t *out, size_t cap);

// Picks level_idc when it is 0, and checks what NVENC and this code can take: 0, or -1.
int h264enc_configure(H264EncConfig *config);
void h264enc_layout(const H264EncConfig *config, H264EncLayout *layout);
// A picture buffer of `width` x `height` (the coded size) in `layout` (TEGRA_NVENC_LAYOUT_*): whole blocks per plane.
void h264enc_surface(uint32_t layout, uint32_t width, uint32_t height, H264EncSurface *surface);
uint32_t h264enc_width_mbs(const H264EncConfig *config);
uint32_t h264enc_height_mbs(const H264EncConfig *config);

// NAL units without start codes. Return the size, 0 if `cap` is too small.
size_t h264enc_write_sps(const H264EncConfig *config, uint8_t *out, size_t cap);
size_t h264enc_write_pps(const H264EncConfig *config, uint8_t *out, size_t cap);
// ISO/IEC 14496-15 AVCDecoderConfigurationRecord with that SPS and PPS (4-byte NAL lengths).
size_t h264enc_write_avcc(const H264EncConfig *config, uint8_t *out, size_t cap);
// The slice header the setup asks NVENC for (7.3.3 with these parameter sets), as RBSP bits; nal_unit_type is 5 for
// an IDR, else 1, and nal_ref_idc 3 for every picture but a B or an unreferenced I (0: not a reference).
void h264enc_slice_header(const H264EncConfig *config, const H264EncPicture *picture, H264Bits *b);

// The submit for one picture (TegraNVENCCheck.h accepts it): methods, relocations, the setup and its controls, the
// input picture's clean range.
void h264enc_fill_submit(const H264EncConfig *config, const H264EncLayout *layout, const H264EncPicture *picture,
                         const H264EncHandles *handles, TegraNVENCSubmit *submit);

// Writes an NV12 picture (`width` x `height`, each plane with its own stride) into an input buffer in the proven
// layout, padding the coded area by edge replication.
void h264enc_put_input(const H264EncConfig *config, const H264EncLayout *layout, uint8_t *buffer, const uint8_t *luma,
                       size_t lumaStride, const uint8_t *chroma, size_t chromaStride);
// The same for `width` x `height` samples from the planes given (a region of a larger picture), edge-replicated to
// `coded_width` x `coded_height` (whole 16-sample runs) in an input surface laid out as `input_layout`.
void h264enc_put_region(uint32_t input_layout, const H264EncSurface *input, uint8_t *buffer, const uint8_t *luma,
                        size_t lumaStride, const uint8_t *chroma, size_t chromaStride, uint32_t width, uint32_t height,
                        uint32_t coded_width, uint32_t coded_height);
// Byte offset of sample (x, y) of a plane laid out as `layout_id` (TEGRA_NVENC_LAYOUT_*) with `pitch` bytes per row.
size_t h264enc_surface_offset(uint32_t layout_id, uint32_t pitch, uint32_t x, uint32_t y);

// Annex B (3- or 4-byte start codes) to 4-byte length-prefixed NAL units, dropping zero padding. Returns the output
// size, or 0 when `cap` is too small or no NAL unit was found.
size_t h264enc_annexb_to_avcc(const uint8_t *in, size_t size, uint8_t *out, size_t cap);

#endif
