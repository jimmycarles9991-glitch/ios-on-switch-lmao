// SPDX-License-Identifier: BSD-2-Clause-Patent
// Field-coded H.264 streams for h264_fields_host, written from a list of pictures: Main profile, CAVLC, 16 macroblocks
// wide and one macroblock pair high, frame_mbs_only_flag 0, deblocking off. Every picture is decodable: an I picture is
// all I_PCM at a level that names it (16 + 3 x its index in decoding order); a P picture's first macroblock is that
// I_PCM block and each of its next `probes` macroblocks copies the first macroblock of RefPicList0[i] (P_L0_16x16,
// ref_idx_l0 i, a whole-macroblock motion vector back to x = 0, no residual), so a decoder's output shows the reference
// list it built. A frame picture's second macroblock row is I_PCM, and its bottom field's rows are one level up.
#ifndef H264_FIELDS_STREAM_H
#define H264_FIELDS_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    FS_TOP = 1,
    FS_BOTTOM = 2,
    FS_FRAME = 3
};

typedef struct {
    char type;          // 'I' or 'P'
    uint8_t structure;  // FS_*
    bool idr, ref;
    uint8_t frame_num, poc_lsb;
    uint8_t probes;    // P: RefPicList0 entries copied (num_ref_idx_l0_active)
    bool long_term;    // IDR: long_term_reference_flag
    const char *mmco;  // "op[:a[:b]] ...": memory_management_control_operations, in order
    bool new_sample;   // starts a sample of its own (else it joins the previous picture's sample)
} FieldsPicture;

typedef struct {
    const char *name;
    uint8_t max_num_ref_frames;
    bool gaps;  // gaps_in_frame_num_value_allowed_flag
    const FieldsPicture *pictures;
    unsigned count;
} FieldsScenario;

typedef struct {
    uint8_t sps[32], pps[16];
    size_t sps_size, pps_size;
    uint8_t *annexb;  // the whole stream with start codes (--dump)
    size_t annexb_size;
    uint8_t *samples;  // the samples, 4-byte length-prefixed NAL units, back to back
    size_t sample_offset[64], sample_size[64];
    unsigned sample_count;
} FieldsStream;

int fields_stream_write(const FieldsScenario *scenario, FieldsStream *out);
void fields_stream_free(FieldsStream *stream);

#endif
