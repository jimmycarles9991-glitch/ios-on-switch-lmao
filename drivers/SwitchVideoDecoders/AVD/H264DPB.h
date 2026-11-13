// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264's picture bookkeeping for NVDEC, frames and fields alike: picture order counts (8.2.1), frame_num gaps
// (8.2.5.2), decoded reference picture marking per field (8.2.5), which field starts and which completes a frame, and
// the dpb[] table of nvdec_h264_pic_s. NVDEC builds the reference picture lists (8.2.4) from that table itself.
// Plain C with no backend, so the host tests drive it directly.
//
// The table follows NVIDIA's own T210 driver (L4T libnvtvmr.so, its H.264 picture setup at 0x19700-0x19b90): each
// entry is a frame store; state bit 0 and 1 say its top and bottom field are used for reference, each field's
// marking is 1 for short-term and 2 for long-term, is_field says it was decoded as field pictures, and when a second
// field is decoded its own frame store (holding the first field) names the first field's order count in both
// FieldOrderCnt slots (0x19b38, 0x1a118-0x1a138, 0x1a328).
#ifndef H264_DPB_H
#define H264_DPB_H
#include "H264Syntax.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

enum {
    H264_REF_NONE,
    H264_REF_SHORT,
    H264_REF_LONG
};

// A picture's structure, as the fields it holds (bit 0 top, bit 1 bottom: nvdec_dpb_entry_s.state's bits).
enum {
    H264_TOP = 1,
    H264_BOTTOM = 2,
    H264_FRAME = 3
};

typedef struct {
    uint8_t marking[2];  // H264_REF_* of the top and the bottom field
    bool non_existing;   // 8.2.5.2 frame_num gap filler
    bool is_field;       // decoded as field pictures: a non-paired field or a complementary field pair
    uint8_t pic_idx;     // surface, picture slot and colocated slot
    uint8_t slot;        // entry in nvdec_h264_pic_s.dpb[], kept while the frame store is used for reference
    uint32_t frame_num, long_term_frame_idx;
    int32_t poc[2];  // top and bottom field order counts; 0 for a field never decoded
} H264RefFrame;

// The picture being decoded.
typedef struct {
    uint8_t structure;  // H264_FRAME, H264_TOP or H264_BOTTOM
    bool second;        // the second field of the frame whose first field was decoded last
    uint8_t pic_idx;
    int32_t poc[2];  // a frame's two order counts; a field's own, and for a second field its first field's too
    int32_t poc_msb;
    uint32_t frame_num_offset;
} H264Current;

typedef struct {
    H264RefFrame refs[16];
    int32_t max_long_term_frame_idx;  // -1: "no long-term frame indices"
    uint8_t log2_max_frame_num, ref_limit;
    int32_t prev_poc_msb, prev_poc_lsb;
    uint32_t prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
    // The frame whose first field was decoded last, while its second field may still come.
    bool pairing, pair_reference;
    uint8_t pair_parity, pair_idx;
    uint32_t pair_frame_num;
    int32_t pair_poc;
} H264DPB;

// A new sequence (configure, an IDR picture, a new SPS): no references, order counts from zero.
void h264_dpb_reset(H264DPB *dpb, const H264SPS *sps);
// The SPS's frame_num length and reference count, kept with the references.
void h264_dpb_use_sps(H264DPB *dpb, const H264SPS *sps);

// 7.3.3's field_pic_flag and bottom_field_flag as a structure.
uint8_t h264_slice_structure(const H264Slice *slice);
// Whether the picture `slice` starts is the second field of the frame being paired: the opposite parity, the same
// frame_num and reference-ness, not an IDR picture (3.29, 3.30: a pair otherwise goes into two frame stores).
bool h264_dpb_second_field(const H264DPB *dpb, const H264Slice *slice);
// 8.2.5.2 for a non-IDR picture that does not continue frame_num: non-existing frames slid in; `pic_idx` is what
// their dpb[] entries name (they are never read: not_existing).
void h264_dpb_fill_gap(H264DPB *dpb, uint32_t frame_num, uint8_t pic_idx);
// 8.2.1: the picture's order counts and the state its marking needs; current->pic_idx is the caller's.
void h264_dpb_order(const H264DPB *dpb, const H264SPS *sps, const H264Slice *slice, H264Current *current);
// Whether a reference frame store holds surface `pic_idx`.
bool h264_dpb_holds(const H264DPB *dpb, uint8_t pic_idx);
// Drops the short-term frame store with the smallest FrameNumWrap (a stream holding more than it declared).
bool h264_dpb_drop_oldest(H264DPB *dpb, uint32_t frame_num);

// nvdec_h264_pic_s's dpb[] and CurrFieldOrderCnt[] for the current picture.
void h264_dpb_fill(const H264DPB *dpb, const H264Current *current, nvdec_dpb_entry_s out[16], int currPoc[2]);

// After the picture is decoded: 8.2.5 marking for a reference picture, then the order count and pairing state the
// next picture starts from. Returns true for memory_management_control_operation 5, after which current->poc counts
// from the picture itself (8.2.1).
bool h264_dpb_finish(H264DPB *dpb, const H264Slice *slice, H264Current *current);

#endif
