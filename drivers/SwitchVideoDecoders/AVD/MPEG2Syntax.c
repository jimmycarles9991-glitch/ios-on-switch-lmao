// SPDX-License-Identifier: BSD-2-Clause-Patent
// See MPEG2Syntax.h. Clause numbers are ITU-T H.262 (02/2012).
#include "MPEG2Syntax.h"
#include <string.h>
#include "AVDBits.h"

enum {
    EXT_SEQUENCE = 1,
    EXT_SEQUENCE_DISPLAY = 2,
    EXT_QUANT_MATRIX = 3,
    EXT_PICTURE_CODING = 8
};

static const uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// 6.3.11 default intra_quantiser_matrix, raster order (the non-intra default is 16 throughout).
static const uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

// A quantiser matrix comes in the default zigzag scan whatever alternate_scan says (6.3.11).
static void loadMatrix(AVDBits *b, uint8_t *matrix)
{
    for (int i = 0; i < 64; i++)
        matrix[kZigzag[i]] = (uint8_t)avd_u(b, 8);
}

bool mpeg2_parse_sequence(MPEG2Sequence *seq, const uint8_t *data, size_t size)
{
    AVDBits b = avd_bits(data, size);
    seq->width = (uint16_t)avd_u(&b, 12);
    seq->height = (uint16_t)avd_u(&b, 12);
    avd_skip(&b, 8);   // aspect_ratio_information, frame_rate_code
    avd_skip(&b, 18);  // bit_rate_value
    avd_marker(&b);
    avd_skip(&b, 11);  // vbv_buffer_size_value, constrained_parameters_flag
    memcpy(seq->intra_matrix, kDefaultIntra, 64);
    memset(seq->inter_matrix, 16, 64);
    if (avd_u(&b, 1))
        loadMatrix(&b, seq->intra_matrix);
    if (avd_u(&b, 1))
        loadMatrix(&b, seq->inter_matrix);
    seq->mpeg2 = false;
    seq->progressive_sequence = true;
    seq->chroma_format = 1;
    seq->valid = !b.bad && seq->width && seq->height;
    return seq->valid;
}

static void sequenceExtension(MPEG2Sequence *seq, AVDBits *b)
{
    seq->profile_level = (uint8_t)avd_u(b, 8);
    seq->progressive_sequence = avd_u(b, 1);
    seq->chroma_format = (uint8_t)avd_u(b, 2);
    seq->width = (uint16_t)(seq->width | avd_u(b, 2) << 12);
    seq->height = (uint16_t)(seq->height | avd_u(b, 2) << 12);
    seq->mpeg2 = true;
}

static void displayExtension(MPEG2Sequence *seq, AVDBits *b)
{
    avd_skip(b, 3);  // video_format
    seq->colour_description = avd_u(b, 1);
    if (seq->colour_description) {
        seq->colour_primaries = (uint8_t)avd_u(b, 8);
        seq->transfer = (uint8_t)avd_u(b, 8);
        seq->matrix = (uint8_t)avd_u(b, 8);
    }
}

static void pictureCoding(MPEG2Picture *p, AVDBits *b)
{
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < 2; t++)
            p->f_code[s][t] = (uint8_t)avd_u(b, 4);
    p->intra_dc_precision = (uint8_t)avd_u(b, 2);
    p->structure = (uint8_t)avd_u(b, 2);
    p->top_field_first = avd_u(b, 1);
    p->frame_pred_frame_dct = avd_u(b, 1);
    p->concealment_motion_vectors = avd_u(b, 1);
    p->q_scale_type = avd_u(b, 1);
    p->intra_vlc_format = avd_u(b, 1);
    p->alternate_scan = avd_u(b, 1);
    avd_skip(b, 2);  // repeat_first_field, chroma_420_type
    p->progressive_frame = avd_u(b, 1);
}

bool mpeg2_parse_extension(MPEG2Sequence *seq, MPEG2Picture *picture, const uint8_t *data, size_t size)
{
    AVDBits b = avd_bits(data, size);
    switch (avd_u(&b, 4)) {
    case EXT_SEQUENCE:
        sequenceExtension(seq, &b);
        break;
    case EXT_SEQUENCE_DISPLAY:
        displayExtension(seq, &b);
        break;
    case EXT_QUANT_MATRIX:
        if (avd_u(&b, 1))
            loadMatrix(&b, seq->intra_matrix);
        if (avd_u(&b, 1))
            loadMatrix(&b, seq->inter_matrix);
        break;
    case EXT_PICTURE_CODING:
        if (picture)
            pictureCoding(picture, &b);
        break;
    default:
        break;
    }
    return !b.bad;
}

bool mpeg2_parse_picture(MPEG2Picture *p, const uint8_t *data, size_t size)
{
    AVDBits b = avd_bits(data, size);
    memset(p, 0, sizeof(*p));
    avd_skip(&b, 10);  // temporal_reference
    p->coding_type = (uint8_t)avd_u(&b, 3);
    avd_skip(&b, 16);  // vbv_delay
    for (int s = 0; s < 2; s++)
        p->f_code[s][0] = p->f_code[s][1] = 1;
    if (p->coding_type == MPEG2_P || p->coding_type == MPEG2_B) {
        p->full_pel[0] = avd_u(&b, 1);
        p->f_code[0][0] = p->f_code[0][1] = (uint8_t)avd_u(&b, 3);
    }
    if (p->coding_type == MPEG2_B) {
        p->full_pel[1] = avd_u(&b, 1);
        p->f_code[1][0] = p->f_code[1][1] = (uint8_t)avd_u(&b, 3);
    }
    p->structure = MPEG2_FRAME;
    p->frame_pred_frame_dct = true;
    p->progressive_frame = true;
    return !b.bad && p->coding_type >= MPEG2_I && p->coding_type <= MPEG2_B + 1;
}

int mpeg2_parse_config(MPEG2Sequence *seq, const uint8_t *data, size_t size)
{
    int found = 0;
    for (size_t at = avd_next_start_code(data, size, 0); at + 4 <= size;) {
        const uint8_t code = data[at + 3];
        const size_t next = avd_next_start_code(data, size, at + 4);
        if (code == MPEG2_START_PICTURE)
            break;
        if (code == MPEG2_START_SEQUENCE) {
            if (!mpeg2_parse_sequence(seq, data + at + 4, next - at - 4))
                return -1;
            found = 1;
        } else if (code == MPEG2_START_EXTENSION && found &&
                   !mpeg2_parse_extension(seq, NULL, data + at + 4, next - at - 4)) {
            return -1;
        }
        at = next;
    }
    return found;
}

const char *mpeg2_sequence_unsupported(const MPEG2Sequence *seq)
{
    return !seq->valid                               ? "no sequence header"
           : seq->chroma_format != 1                 ? "not 4:2:0"
           : seq->width < 48 || seq->height < 16     ? "below NVDEC's 48x16"
           : seq->width > 4080 || seq->height > 4080 ? "beyond NVDEC's MPEG-2 4080x4080"
                                                     : NULL;
}

uint32_t mpeg2_height_mbs(const MPEG2Sequence *seq)
{
    return seq->progressive_sequence ? (seq->height + 15u) / 16 : (seq->height + 31u) / 32 * 2;
}
