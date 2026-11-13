// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-1 (ISO/IEC 11172-2) and MPEG-2 (ITU-T H.262) headers as far as NVDEC's picture setup needs them: the sequence
// header and its extensions (6.2.2.1, 6.2.2.3, 6.2.3.1-6.2.3.2, quant matrices 6.2.3.2), the picture header and its
// coding extension (6.2.3, 6.2.3.1). NVDEC decodes the slices itself. Plain C, for the host tests.
#ifndef MPEG2_SYNTAX_H
#define MPEG2_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MPEG2_START_PICTURE = 0x00,
    MPEG2_START_SLICE_FIRST = 0x01,
    MPEG2_START_SLICE_LAST = 0xaf,
    MPEG2_START_SEQUENCE = 0xb3,
    MPEG2_START_EXTENSION = 0xb5,
    MPEG2_START_SEQUENCE_END = 0xb7,
};

enum {
    MPEG2_I = 1,
    MPEG2_P = 2,
    MPEG2_B = 3
};

enum {
    MPEG2_TOP_FIELD = 1,
    MPEG2_BOTTOM_FIELD = 2,
    MPEG2_FRAME = 3
};

typedef struct {
    bool valid, mpeg2;  // mpeg2: a sequence extension followed the sequence header
    uint16_t width, height;
    uint8_t profile_level, chroma_format;
    bool progressive_sequence;
    bool colour_description;
    uint8_t colour_primaries, transfer, matrix;
    uint8_t intra_matrix[64], inter_matrix[64];  // raster order, the defaults unless loaded
} MPEG2Sequence;

typedef struct {
    uint8_t coding_type;   // MPEG2_I, MPEG2_P, MPEG2_B (4: MPEG-1's D-picture)
    uint8_t f_code[2][2];  // [forward, backward][horizontal, vertical]
    bool full_pel[2];      // MPEG-1 only
    uint8_t intra_dc_precision, structure;
    bool top_field_first, frame_pred_frame_dct, concealment_motion_vectors, q_scale_type, intra_vlc_format;
    bool alternate_scan, progressive_frame;
} MPEG2Picture;

// The sequence header after 00 00 01 B3 (its matrices back to the defaults unless loaded); false when malformed.
bool mpeg2_parse_sequence(MPEG2Sequence *seq, const uint8_t *data, size_t size);
// An extension after 00 00 01 B5: sequence, sequence display and quant matrix extensions into `seq`, the picture
// coding extension into `picture`; others are ignored. false when malformed.
bool mpeg2_parse_extension(MPEG2Sequence *seq, MPEG2Picture *picture, const uint8_t *data, size_t size);
// The picture header after 00 00 01 00, with MPEG-1's fixed coding parameters until a coding extension follows.
bool mpeg2_parse_picture(MPEG2Picture *picture, const uint8_t *data, size_t size);

// Every sequence header and extension in `data` into `seq`, up to the first picture: 1 when a sequence header was
// there, 0 when none, -1 when one was malformed.
int mpeg2_parse_config(MPEG2Sequence *seq, const uint8_t *data, size_t size);

// Why NVDEC does not take this sequence, or NULL: 4:2:0 only, 48x16 to 4080x4080 (NVDEC 2.0's MPEG-1/2 range).
const char *mpeg2_sequence_unsupported(const MPEG2Sequence *seq);
// The macroblock rows NVDEC is given: whole field macroblock pairs unless the sequence is progressive (6.3.3).
uint32_t mpeg2_height_mbs(const MPEG2Sequence *seq);

#endif
