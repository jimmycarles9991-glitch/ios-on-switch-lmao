// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-4 Part 2 on NVDEC 2.0; see MPEG4NVDEC.h.
#include "MPEG4NVDEC.h"
#include <stdlib.h>
#include <string.h>
#include "AVDBits.h"
#include "MPEGFrames.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

struct MPEG4Decoder {
    MPEGFrames frames;
    MPEG4VOL vol;
    bool configured;
    uint32_t aux, aux_bytes, coloc_off, history_off, history_bytes, scratch_off;
    // FFmpeg's VOP timing (mpeg4videodec.c decode_vop_header), in vop_time_increment_resolution ticks
    int64_t time_base, last_time_base, last_non_b_time, pp_time, pb_time, pp_field_time, pb_field_time, t_frame;
};

enum {
    kScratchBytes = 0x400,
};

// visual_object_sequence_end_code twice, as averne's driver ends every VOP's bitstream
static const uint8_t kEndOfStream[16] = {0, 0, 1, 0xb1, 0, 0, 0, 0, 0, 0, 1, 0xb1, 0, 0, 0, 0};

MPEG4Decoder *mpeg4_decoder_create(const H264Backend *backend)
{
    MPEG4Decoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->frames.b = *backend;
    d->frames.past = d->frames.future = -1;
    return d;
}

static void releaseAux(MPEG4Decoder *d)
{
    if (d->aux)
        d->frames.b.free(d->frames.b.ctx, d->aux);
    d->aux = 0;
}

void mpeg4_decoder_destroy(MPEG4Decoder *d)
{
    if (!d)
        return;
    mpeg_frames_release(&d->frames);
    releaseAux(d);
    free(d);
}

const MPEG4VOL *mpeg4_decoder_vol(const MPEG4Decoder *d)
{
    return &d->vol;
}

// averne's nvtegra_mpeg4.c sizes: colocated motion per macroblock pair row, the history buffer, a 1 KiB scratch.
static int allocate(MPEG4Decoder *d, uint32_t wmbs, uint32_t hmbs)
{
    releaseAux(d);
    if (mpeg_frames_configure(&d->frames, wmbs, hmbs, 0))
        return -3;
    const uint32_t coloc = ALIGN(ALIGN(hmbs, 2) * wmbs * 64 - 63, 256);
    d->history_bytes = ALIGN(wmbs * 256 + 0x1100, 256);
    d->coloc_off = 0;
    d->history_off = coloc;
    d->scratch_off = d->history_off + d->history_bytes;
    d->aux_bytes = d->scratch_off + kScratchBytes;
    void *va;
    if (d->frames.b.alloc(d->frames.b.ctx, d->aux_bytes, &d->aux, &va)) {
        d->aux = 0;
        mpeg_frames_release(&d->frames);
        return -3;
    }
    return 0;
}

int mpeg4_decoder_configure(MPEG4Decoder *d, const MPEG4VOL *vol)
{
    if (mpeg4_vol_unsupported(vol))
        return -1;
    const uint32_t wmbs = (vol->width + 15u) / 16, hmbs = (vol->height + 15u) / 16;
    if (!d->configured || wmbs != d->frames.width_mbs || hmbs != d->frames.height_mbs) {
        d->configured = false;
        if (allocate(d, wmbs, hmbs))
            return -3;
    }
    d->vol = *vol;
    d->configured = true;
    return 0;
}

static int64_t roundedDiv(int64_t a, int64_t b)
{
    return (a > 0 ? a + (b >> 1) : a - (b >> 1)) / b;
}

// false for a B-VOP whose references' times leave it nowhere between them (FFmpeg skips such a VOP).
static bool advanceTime(MPEG4Decoder *d, const MPEG4VOP *vop)
{
    const int64_t resolution = d->vol.time_resolution;
    if (vop->coding_type != MPEG4_VOP_B) {
        d->last_time_base = d->time_base;
        d->time_base += vop->modulo_time_base;
        const int64_t time = d->time_base * resolution + vop->time_increment;
        d->pp_time = time - d->last_non_b_time;
        d->last_non_b_time = time;
        return true;
    }
    const int64_t time = (d->last_time_base + vop->modulo_time_base) * resolution + vop->time_increment;
    d->pb_time = d->pp_time - (d->last_non_b_time - time);
    if (d->pp_time <= d->pb_time || d->pp_time <= d->pp_time - d->pb_time || d->pp_time <= 0)
        return false;
    if (!d->t_frame)
        d->t_frame = d->pb_time ? d->pb_time : 1;
    const int64_t past = roundedDiv(d->last_non_b_time - d->pp_time, d->t_frame);
    d->pp_field_time = (roundedDiv(d->last_non_b_time, d->t_frame) - past) * 2;
    d->pb_field_time = (roundedDiv(time, d->t_frame) - past) * 2;
    if (d->pp_field_time <= d->pb_field_time || d->pb_field_time <= 1) {
        d->pb_field_time = 2;
        d->pp_field_time = 4;
    }
    return true;
}

static void fillSetup(const MPEG4Decoder *d, nvdec_mpeg4_pic_s *p, const MPEG4VOP *vop)
{
    const MPEG4VOL *vol = &d->vol;
    const MPEGFrames *f = &d->frames;
    memset(p, 0, sizeof(*p));
    p->stream_len = f->bits_len;
    p->slice_count = 1;
    p->scratch_pic_buffer_size = kScratchBytes;
    p->FrameWidth = p->width = (short)(f->width_mbs * 16);
    p->FrameHeight = p->height = (short)(f->height_mbs * 16);
    p->vop_time_increment_bitcount = (char)vol->time_bits;
    p->resync_marker_disable = vol->resync_marker_disable;
    p->FrameStride[0] = p->FrameStride[1] = f->pitch;
    p->HistBufferSize = d->history_bytes / 256;
    p->trd[0] = (int)d->pp_time;
    p->trd[1] = (int)(d->pp_field_time >> 1);
    p->trb[0] = (int)d->pb_time;
    p->trb[1] = (int)(d->pb_field_time >> 1);
    p->vop_fcode_forward = vop->fcode_forward;
    p->vop_fcode_backward = vop->fcode_backward;
    p->quant_type = vol->quant_type;
    p->quarter_sample = vol->quarter_sample;
    p->ptype = vop->coding_type;
    p->rnd = vop->rounding;
    p->alternate_vertical_scan_flag = vop->alternate_vertical_scan;
    memcpy(p->intra_quant_mat, vol->intra_matrix, 64);
    memcpy(p->nonintra_quant_mat, vol->inter_matrix, 64);
}

// One coded VOP (start code to the next start code) into a free surface; -1, -2, -3 as mpeg4_decoder_decode, else the
// surface (the job that decodes it in *job).
static int decodeVOP(MPEG4Decoder *d, const MPEG4VOP *vop, const uint8_t *data, size_t size, uint32_t *job)
{
    MPEGFrames *f = &d->frames;
    if (vop->coding_type == MPEG4_VOP_S)
        return -1;
    const int current = mpeg_frames_take(f);
    if (current < 0)
        return -3;
    if (mpeg_frames_append(f, data, size, true) || mpeg_frames_append(f, kEndOfStream, sizeof(kEndOfStream), false))
        return -3;
    // NVDEC's slots: the reference a P-VOP predicts from, or a B-VOP's past and future; an I-VOP names itself.
    const int past = vop->coding_type == MPEG4_VOP_I ? current : vop->coding_type == MPEG4_VOP_P ? f->future : f->past;
    const int future = vop->coding_type == MPEG4_VOP_B ? f->future : current;
    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    submit.codec = TEGRA_NVDEC_CODEC_MPEG4;
    submit.setup_bytes = sizeof(nvdec_mpeg4_pic_s);
    fillSetup(d, (nvdec_mpeg4_pic_s *)submit.setup, vop);
    mpeg_frames_entry(&submit, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_MPEG4,
                      0);
    mpeg_frames_entry(
        &submit, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
        NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_MPEG4 | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    mpeg_frames_bind(f, &submit, current, past, future);
    mpeg_frames_entry(&submit, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->coloc_off);
    mpeg_frames_entry(&submit, NVC5B0_SET_HISTORY_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->history_off);
    mpeg_frames_entry(&submit, NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->scratch_off);
    const int64_t fence = mpeg_frames_run(f, &submit, (TegraNVDECRange){0});
    if (fence < 0)
        return (int)fence;
    *job = (uint32_t)fence;
    if (vop->coding_type != MPEG4_VOP_B)
        mpeg_frames_reference(f, current);
    return current;
}

// A VOL in the sample (some encoders repeat it at key frames): the same geometry keeps the buffers.
static int sampleVOL(MPEG4Decoder *d, const uint8_t *data, size_t size)
{
    MPEG4VOL vol = d->vol;
    const int found = mpeg4_parse_config(&vol, data, size);
    if (found <= 0)
        return found;
    return mpeg4_decoder_configure(d, &vol);
}

int mpeg4_decoder_decode(MPEG4Decoder *d, const uint8_t *data, size_t size, void *token)
{
    if (!d->configured)
        return -3;
    const int vol = sampleVOL(d, data, size);
    if (vol < 0)
        return vol == -3 ? -3 : -1;
    int shown = -1;
    uint32_t shownJob = 0;
    for (size_t at = avd_next_start_code(data, size, 0); at + 4 <= size;) {
        const size_t next = avd_next_start_code(data, size, at + 4);
        uint32_t seconds;
        if (data[at + 3] == MPEG4_START_GOV) {
            if (!mpeg4_parse_gov(data + at + 4, next - at - 4, &seconds))
                return -1;
            d->time_base = seconds;
        } else if (data[at + 3] == MPEG4_START_VOP) {
            MPEG4VOP vop;
            if (!mpeg4_parse_vop(&d->vol, data + at + 4, next - at - 4, &vop))
                return -1;
            // A VOP whose references are missing (a stream joined after its I-VOP) is skipped, not predicted from nothing.
            const bool references = vop.coding_type == MPEG4_VOP_I || vop.coding_type == MPEG4_VOP_S ||
                                    (d->frames.future >= 0 && (vop.coding_type != MPEG4_VOP_B || d->frames.past >= 0));
            if (advanceTime(d, &vop) && references) {
                if (vop.coded) {
                    uint32_t job = 0;
                    const int index = decodeVOP(d, &vop, data + at, next - at, &job);
                    if (index < 0)
                        return index;
                    shown = index;
                    shownJob = job;
                } else if (d->frames.future >= 0) {
                    shown = d->frames.future;
                    shownJob = 0;
                }
            }
        }
        at = next;
    }
    if (shown < 0)
        return 0;
    mpeg_frames_output(&d->frames, shown, d->vol.width, d->vol.height, AVD_FIELDS_UNSAID, shownJob, token);
    return 1;
}
