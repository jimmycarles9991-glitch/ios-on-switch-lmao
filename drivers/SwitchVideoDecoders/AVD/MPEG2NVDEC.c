// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-1 and MPEG-2 on NVDEC 2.0; see MPEG2NVDEC.h.
#include "MPEG2NVDEC.h"
#include <stdlib.h>
#include <string.h>
#include "AVDBits.h"
#include "MPEGFrames.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

// The most slices a picture may have: one a macroblock, as averne's driver bounds them (8160: 1920x1088).
#define MAX_SLICES(wmbs, hmbs) ((wmbs) * (hmbs) < 8160 ? (wmbs) * (hmbs) : 8160)

// A field picture's first field, waiting for its second: the surface it went into (or that it was skipped), its parity
// and its coding type.
typedef struct {
    bool waiting, skipped;
    int8_t surface;
    uint8_t structure, type;
} FirstField;

struct MPEG2Decoder {
    MPEGFrames frames;
    MPEG2Sequence seq;
    uint32_t width, height, max_slices, slices;
    bool configured;
    FirstField first;
};

// sequence_end_code twice, as averne's driver ends every picture's bitstream
static const uint8_t kEndOfStream[16] = {0, 0, 1, 0xb7, 0, 0, 0, 0, 0, 0, 1, 0xb7, 0, 0, 0, 0};

MPEG2Decoder *mpeg2_decoder_create(const H264Backend *backend)
{
    MPEG2Decoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->frames.b = *backend;
    d->frames.past = d->frames.future = -1;
    return d;
}

void mpeg2_decoder_destroy(MPEG2Decoder *d)
{
    if (!d)
        return;
    mpeg_frames_release(&d->frames);
    free(d);
}

const MPEG2Sequence *mpeg2_decoder_sequence(const MPEG2Decoder *d)
{
    return &d->seq;
}

int mpeg2_decoder_configure(MPEG2Decoder *d, uint32_t width, uint32_t height)
{
    if (d->configured && width <= d->width && height <= d->height)
        return 0;
    d->configured = false;
    const uint32_t wmbs = (width + 15) / 16, hmbs = (height + 31) / 32 * 2;
    d->max_slices = MAX_SLICES(wmbs, hmbs);
    if (mpeg_frames_configure(&d->frames, wmbs, hmbs, (d->max_slices + 1) * 4))
        return -3;
    d->width = width;
    d->height = height;
    d->first.waiting = false;
    d->configured = true;
    return 0;
}

int mpeg2_decoder_set_sequence(MPEG2Decoder *d, const MPEG2Sequence *seq)
{
    if (mpeg2_sequence_unsupported(seq))
        return -1;
    d->seq = *seq;
    return mpeg2_decoder_configure(d, seq->width, seq->height);
}

static void fillSetup(const MPEG2Decoder *d, nvdec_mpeg2_pic_s *p, const MPEG2Picture *pic, bool second)
{
    const MPEGFrames *f = &d->frames;
    const uint32_t wmbs = (d->seq.width + 15u) / 16, hmbs = mpeg2_height_mbs(&d->seq);
    memset(p, 0, sizeof(*p));
    p->stream_len = f->bits_len;
    p->slice_count = d->slices;
    p->FrameWidth = (short)(wmbs * 16);
    p->FrameHeight = (short)(hmbs * 16);
    p->picture_structure = pic->structure;
    p->picture_coding_type = pic->coding_type;
    p->intra_dc_precision = pic->intra_dc_precision;
    p->frame_pred_frame_dct = (char)pic->frame_pred_frame_dct;
    p->concealment_motion_vectors = (char)pic->concealment_motion_vectors;
    p->intra_vlc_format = (char)pic->intra_vlc_format;
    p->f_code[0] = (char)pic->f_code[0][0];
    p->f_code[1] = (char)pic->f_code[0][1];
    p->f_code[2] = (char)pic->f_code[1][0];
    p->f_code[3] = (char)pic->f_code[1][1];
    p->PicWidthInMbs = (unsigned short)wmbs;
    p->FrameHeightInMbs = (unsigned short)hmbs;
    p->pitch_luma = p->pitch_chroma = f->pitch;
    p->alternate_scan = pic->alternate_scan;
    p->secondfield = second;
    p->q_scale_type = pic->q_scale_type;
    p->top_field_first = pic->top_field_first;
    p->full_pel_fwd_vector = !d->seq.mpeg2 && pic->full_pel[0];
    p->full_pel_bwd_vector = !d->seq.mpeg2 && pic->full_pel[1];
    memcpy(p->quant_mat_8x8intra, d->seq.intra_matrix, 64);
    memcpy(p->quant_mat_8x8nonintra, d->seq.inter_matrix, 64);
}

static int64_t run(MPEG2Decoder *d, const MPEG2Picture *pic, int current, int past, int future, bool second)
{
    MPEGFrames *f = &d->frames;
    ((uint32_t *)(mpeg_frames_input(f)->va + MPEG_FRAMES_TABLES))[d->slices] = f->bits_len;
    if (mpeg_frames_append(f, kEndOfStream, sizeof(kEndOfStream), false))
        return -3;
    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    submit.codec = TEGRA_NVDEC_CODEC_MPEG12;
    submit.setup_bytes = sizeof(nvdec_mpeg2_pic_s);
    fillSetup(d, (nvdec_mpeg2_pic_s *)submit.setup, pic, second);
    const uint32_t type =
        d->seq.mpeg2 ? NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_MPEG2 : NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_MPEG1;
    mpeg_frames_entry(&submit, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_MPEG12,
                      0);
    mpeg_frames_entry(&submit, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
                      type | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    mpeg_frames_bind(f, &submit, current, past, future);
    const uint32_t input = mpeg_frames_input(f)->handle;
    mpeg_frames_entry(&submit, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, MPEG_FRAMES_TABLES);
    return mpeg_frames_run(f, &submit, (TegraNVDECRange){input, MPEG_FRAMES_TABLES, (d->slices + 1) * 4, 0});
}

// The picture whose slices were collected: decoded into a new surface, or as the second field into its first's.
// Returns the surface of a frame it completed (*fields says how its rows interleave), -4 when it completed none, or
// -1, -2, -3.
static int finishPicture(MPEG2Decoder *d, const MPEG2Picture *pic, uint32_t *job, uint8_t *fields)
{
    MPEGFrames *f = &d->frames;
    if (pic->coding_type < MPEG2_I || pic->coding_type > MPEG2_B || !pic->structure)
        return -1;
    FirstField *first = &d->first;
    const bool field = pic->structure != MPEG2_FRAME;
    const bool second = field && first->waiting && pic->structure != first->structure;
    first->waiting = false;  // a frame picture or a field of the same parity abandons a waiting field
    if (second && first->skipped)
        return -4;
    const bool references =
        second || pic->coding_type == MPEG2_I || (f->future >= 0 && (pic->coding_type == MPEG2_P || f->past >= 0));
    if (!references) {
        *first = (FirstField){.waiting = field, .skipped = true, .structure = pic->structure};
        return -4;
    }
    const int current = second ? first->surface : mpeg_frames_take(f);
    if (current < 0)
        return -3;
    // NVDEC's slots: an I-picture names itself; a P-picture's past is the newest reference frame (the current surface
    // when an I/P field pair starts the stream: its P field predicts from the I field only); a B-picture's past and
    // future reference frames.
    const int newest = f->future >= 0 ? f->future : current;
    const int past = pic->coding_type == MPEG2_I ? current : pic->coding_type == MPEG2_P ? newest : f->past;
    const int future = pic->coding_type == MPEG2_B ? f->future : current;
    const int64_t fence = run(d, pic, current, past, future, second);
    if (fence < 0)
        return (int)fence;
    *job = (uint32_t)fence;
    if (field && !second) {
        *first = (FirstField){
            .waiting = true, .surface = (int8_t)current, .structure = pic->structure, .type = pic->coding_type};
        return -4;
    }
    if ((second ? first->type : pic->coding_type) != MPEG2_B)
        mpeg_frames_reference(f, current);
    *fields = second ? (first->structure == MPEG2_TOP_FIELD ? AVD_FIELDS_TOP_FIRST : AVD_FIELDS_BOTTOM_FIRST)
              : pic->progressive_frame ? AVD_FIELDS_PROGRESSIVE
              : pic->top_field_first   ? AVD_FIELDS_TOP_FIRST
                                       : AVD_FIELDS_BOTTOM_FIRST;
    return current;
}

static int appendSlice(MPEG2Decoder *d, const uint8_t *data, size_t size)
{
    if (d->slices >= d->max_slices)
        return -1;
    ((uint32_t *)(mpeg_frames_input(&d->frames)->va + MPEG_FRAMES_TABLES))[d->slices++] = d->frames.bits_len;
    return mpeg_frames_append(&d->frames, data, size, false) ? -3 : 0;
}

int mpeg2_decoder_decode(MPEG2Decoder *d, const uint8_t *data, size_t size, void *token)
{
    if (!d->configured)
        return -3;
    MPEG2Picture pic;
    bool picture = false;
    int shown = -1;
    uint32_t shownJob = 0;
    uint8_t shownFields = AVD_FIELDS_PROGRESSIVE;
    size_t at = avd_next_start_code(data, size, 0);
    for (;;) {
        const bool end = at + 4 > size;
        const uint8_t code = end ? MPEG2_START_SEQUENCE_END : data[at + 3];
        const size_t next = end ? size : avd_next_start_code(data, size, at + 4);
        const bool slice = code >= MPEG2_START_SLICE_FIRST && code <= MPEG2_START_SLICE_LAST;
        if (picture && d->slices && !slice) {  // the picture's slices end at the next start code of another kind
            uint32_t job = 0;
            uint8_t fields = AVD_FIELDS_PROGRESSIVE;
            const int index = finishPicture(d, &pic, &job, &fields);
            picture = false;
            if (index >= 0) {
                shown = index;
                shownJob = job;
                shownFields = fields;
            } else if (index != -4) {
                return index;
            }
        }
        if (end)
            break;
        const uint8_t *body = data + at + 4;
        const size_t length = next - at - 4;
        if (code == MPEG2_START_SEQUENCE) {
            if (!mpeg2_parse_sequence(&d->seq, body, length))
                return -1;
        } else if (code == MPEG2_START_EXTENSION) {
            if (!mpeg2_parse_extension(&d->seq, picture ? &pic : NULL, body, length))
                return -1;
        } else if (code == MPEG2_START_PICTURE) {
            if (mpeg2_sequence_unsupported(&d->seq) || !mpeg2_parse_picture(&pic, body, length))
                return -1;
            if (mpeg2_decoder_configure(d, d->seq.width, d->seq.height))
                return -3;
            picture = true;
            d->slices = 0;
            d->frames.bits_len = 0;
        } else if (slice && picture) {
            const int appended = appendSlice(d, data + at, next - at);
            if (appended)
                return appended;
        }
        at = next;
    }
    if (shown < 0)
        return 0;
    mpeg_frames_output(&d->frames, shown, d->seq.width, d->seq.height,
                       d->seq.mpeg2 ? shownFields : AVD_FIELDS_PROGRESSIVE, shownJob, token);
    return 1;
}
