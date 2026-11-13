// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCNVDEC.h. Clause numbers are ITU-T H.265 (V8).
#include "HEVCState.h"
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"

HEVCDecoder *hevc_decoder_create(const H264Backend *backend)
{
    HEVCDecoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->b = *backend;
    d->first_sps = -1;
    d->stream_start = true;
    return d;
}

static void freeSurfaces(HEVCDecoder *d)
{
    for (int i = 0; i < SLOTS; i++) {
        if (d->dpb[i].handle)
            d->b.free(d->b.ctx, d->dpb[i].handle);
        if (d->dpb[i].display)
            d->b.free(d->b.ctx, d->dpb[i].display);
        d->dpb[i].handle = d->dpb[i].display = 0;
        d->dpb[i].va = d->dpb[i].display_va = NULL;
    }
}

static void releaseBuffers(HEVCDecoder *d)
{
    if (d->aux)
        d->b.free(d->b.ctx, d->aux);
    nvdec_ring_release(&d->ring, &d->b);
    d->aux = 0;
    d->aux_va = NULL;
}

static void dropAll(HEVCDecoder *d)  // the DPB emptied without output
{
    for (int i = 0; i < SLOTS; i++) {
        Entry *e = &d->dpb[i];
        if (e->item)
            d->b.discard(d->b.ctx, e->item);
        e->item = NULL;
        e->used = e->ref = e->long_term = e->output = false;
    }
}

void hevc_decoder_destroy(HEVCDecoder *d)
{
    if (!d)
        return;
    dropAll(d);
    freeSurfaces(d);
    releaseBuffers(d);
    free(d);
}

int hevc_decoder_parameter_set(HEVCDecoder *d, const uint8_t *nal, size_t size)
{
    if (size < 3)
        return -1;
    const int type = (nal[0] >> 1) & 0x3f;
    if (type == HEVC_NAL_SPS) {
        if (hevc_parse_sps(&d->params, nal, size))
            return -1;
        if (d->first_sps < 0) {  // its id: the SPS just parsed is the only valid one with this content
            for (int i = 0; i < 16; i++)
                if (d->params.sps[i].valid)
                    d->first_sps = i;
        }
        return 0;
    }
    if (type == HEVC_NAL_PPS)
        return hevc_parse_pps(&d->params, nal, size);
    return 0;  // VPS, SEI: nothing NVDEC needs
}

const HEVCSPS *hevc_decoder_sps(const HEVCDecoder *d)
{
    return d->first_sps >= 0 ? &d->params.sps[d->first_sps] : NULL;
}

void hevc_decoder_allow_10bit(HEVCDecoder *d, bool allow)
{
    d->allow10 = allow;
}

void hevc_decoder_display_8bit(HEVCDecoder *d, bool on)
{
    d->display8 = on;
}

// The 8-bit picture at framestride bytes a row: luma, then chroma at half the 16-bit luma's size. The kernel bounds a
// display buffer as 16-bit planes (its unit is undocumented), so the buffer is the 16-bit luma's size.
static int displaySurface(HEVCDecoder *d, Entry *e)
{
    if (!e->display) {
        void *va;
        if (d->b.alloc(d->b.ctx, d->luma_bytes, &e->display, &va)) {
            e->display = 0;
            return -3;
        }
        e->display_va = va;
    } else if (d->b.recycle) {
        void *address = e->display_va;
        if (d->b.recycle(d->b.ctx, e->display, d->luma_bytes, &address))
            return -3;
        e->display_va = address;
    }
    return 0;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void hevc_decoder_want_display_probe(HEVCDecoder *d)
{
    d->probe_wanted = true;
}

bool hevc_decoder_display_probe(const HEVCDecoder *d, HEVCDisplayProbeResult out[2])
{
    if (d->probe_ran)
        memcpy(out, d->probe, sizeof(d->probe));
    return d->probe_ran;
}
#endif

int32_t hevc_decoder_last_poc(const HEVCDecoder *d)
{
    return d->last_poc;
}

unsigned hevc_decoder_missing_refs(const HEVCDecoder *d)
{
    return d->missing;
}

// Buffers for the SPS's coded size. Only called with an empty DPB when the geometry changes (an IRAP that starts a
// coded video sequence; a new SPS mid-sequence with another size is refused).
static int configure(HEVCDecoder *d, const HEVCSPS *sps)
{
    const uint32_t sampleBytes = sps->bit_depth > 8 ? 2 : 1, columns = ALIGN(sps->width, 64);
    const uint32_t pitch = columns * sampleBytes, rows = ALIGN(sps->height, 64);
    if (d->aux && pitch == d->pitch && rows == d->rows)
        return 0;
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used)
            return -1;
    freeSurfaces(d);
    releaseBuffers(d);
    d->width = sps->width, d->height = sps->height, d->pitch = pitch, d->rows = rows;
    d->sample_bytes = sampleBytes;
    d->luma_bytes = pitch * rows;
    d->surface_bytes = d->luma_bytes + d->luma_bytes / 2;
    // Colocated motion: 17 slots of (64-aligned width x height) / 16 bytes; the filter buffer: deblocking, SAO and
    // BSD control rows over the 64-aligned height (averne's nvtegra_hevc.c sizes).
    d->colmv_bytes = columns * rows / 16;
    d->filter_off = ALIGN(17 * d->colmv_bytes, 256);
    d->aux_bytes = d->filter_off + ((kDeblockBytesPerRow + kSaoBytesPerRow) * sampleBytes + kBsdBytesPerRow) * rows;
    void *va;
    if (d->b.alloc(d->b.ctx, d->aux_bytes, &d->aux, &va))
        return -3;
    d->aux_va = va;
    if (nvdec_ring_alloc(&d->ring, &d->b,
                         kBitsOff + ALIGN((uint64_t)sps->width * sps->height / 2 + 0x10000, 0x10000))) {
        releaseBuffers(d);
        return -3;
    }
    return 0;
}

int hevc_decoder_configure(HEVCDecoder *d)
{
    const HEVCSPS *sps = hevc_decoder_sps(d);
    if (!sps)
        return -1;
    const int ret = configure(d, sps);
    if (ret)
        return ret;
    // Surfaces for the DPB the SPS declares (the current picture included), so NVDEC's memory is known to hold them.
    for (unsigned i = 0; i < sps->max_dec_pic_buffering && i < SLOTS; i++) {
        Entry *e = &d->dpb[i];
        void *va = NULL;
        if (e->handle)
            continue;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &e->handle, &va)) {
            e->handle = 0;
            freeSurfaces(d);
            releaseBuffers(d);
            return -3;
        }
        e->va = va;
    }
    return 0;
}

static unsigned countWhere(const HEVCDecoder *d, bool output)
{
    unsigned n = 0;
    for (int i = 0; i < SLOTS; i++)
        n += d->dpb[i].used && (!output || d->dpb[i].output);
    return n;
}

static void removeUnneeded(HEVCDecoder *d)
{
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && !d->dpb[i].output && !d->dpb[i].ref)
            d->dpb[i].used = d->dpb[i].long_term = false;
}

// C.5.2.4: the picture needed for output with the smallest POC goes out.
static bool bump(HEVCDecoder *d)
{
    int best = -1;
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && d->dpb[i].output && (best < 0 || d->dpb[i].poc < d->dpb[best].poc))
            best = i;
    if (best < 0)
        return false;
    Entry *e = &d->dpb[best];
    e->output = false;
    if (e->item)
        d->b.emit(d->b.ctx, e->item);
    e->item = NULL;
    removeUnneeded(d);
    return true;
}

static bool latencyDue(const HEVCDecoder *d, const HEVCSPS *sps)
{
    if (!sps->max_latency_increase_plus1)
        return false;
    const uint64_t limit =
        (uint64_t)sps->max_num_reorder + sps->max_latency_increase_plus1 - 1;  // SpsMaxLatencyPictures
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && d->dpb[i].output && d->dpb[i].latency >= limit)
            return true;
    return false;
}

void hevc_decoder_flush(HEVCDecoder *d)
{
    while (bump(d)) {
    }
}

static void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

typedef struct {
    const uint8_t *data;
    size_t size;
} Span;

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The display-buffer probe's precondition, read on the decode thread: the probe compares against this picture.
static bool decodedCleanly(const HEVCDecoder *d, const NVDECInput *input, uint32_t fence)
{
    TegraNVDECWait wait = {.fence = fence, .invalidate_count = 1};
    wait.invalidate[0] = (TegraNVDECRange){input->handle, kStatusOff, sizeof(nvdec_status_s), 0};
    if (d->b.wait(d->b.ctx, &wait))
        return false;
    const nvdec_status_s *status = (const nvdec_status_s *)(input->va + kStatusOff);
    return !status->error_status || status->mbs_correctly_decoded;
}
#endif

static int findRef(const HEVCDecoder *d, int32_t poc, bool lsbOnly, int32_t mask, bool shortOnly, const bool *taken)
{
    for (int i = 0; i < SLOTS; i++) {
        const Entry *e = &d->dpb[i];
        if (!e->used || !e->ref || taken[i] || (shortOnly && e->long_term))
            continue;
        if (lsbOnly ? (e->poc & mask) == poc : e->poc == poc)
            return i;
    }
    return -1;
}

static int decodePicture(HEVCDecoder *d, const Span *seg, int count, const HEVCSliceHeader *sh, void *token,
                         bool reorder)
{
    const HEVCPPS *pps = &d->params.pps[sh->pps_id];
    const HEVCSPS *sps = &d->params.sps[pps->sps_id];
    if (pps->range_extension || sps->width < 144 || sps->height < 144 || sps->width > 4096 || sps->height > 4096 ||
        (sps->bit_depth == 10 && !d->allow10))
        return -1;
    const uint8_t nal = sh->nal_type;
    const bool irap = nal >= HEVC_NAL_BLA_W_LP && nal <= 23,
               idr = nal == HEVC_NAL_IDR_W_RADL || nal == HEVC_NAL_IDR_N_LP;
    const bool noRaslOutput = irap && (nal != HEVC_NAL_CRA || d->stream_start);
    if (irap)
        d->skip_rasl = noRaslOutput;
    if ((nal == HEVC_NAL_RASL_N || nal == HEVC_NAL_RASL_R) && d->skip_rasl)
        return 0;  // 8.1.3: not decodable
    if (!irap && !d->decoded_any)
        return 0;  // a stream that starts without an IRAP picture: wait for one

    // 8.3.1: the picture order count.
    const int32_t maxLsb = 1 << sps->log2_max_poc_lsb;
    int32_t msb = 0;
    if (!(irap && noRaslOutput)) {
        const int32_t prevLsb = d->prev_tid0_poc & (maxLsb - 1), prevMsb = d->prev_tid0_poc - prevLsb;
        const int32_t lsb = sh->poc_lsb;
        msb = lsb < prevLsb && prevLsb - lsb >= maxLsb / 2  ? prevMsb + maxLsb
              : lsb > prevLsb && lsb - prevLsb > maxLsb / 2 ? prevMsb - maxLsb
                                                            : prevMsb;
    }
    const int32_t poc = msb + sh->poc_lsb;

    // 8.3.2: the reference picture set; references it does not name are no longer references.
    RefSets r;
    memset(&r, 0, sizeof(r));
    bool keep[SLOTS] = {false}, lt[SLOTS] = {false};
    if (irap && noRaslOutput)
        for (int i = 0; i < SLOTS; i++)
            d->dpb[i].ref = false;
    if (!idr) {
        for (unsigned i = 0; i < sh->num_lt; i++) {
            int32_t value = sh->lt[i].poc_lsb;
            if (sh->lt[i].msb_present)
                value += poc - (int32_t)sh->lt[i].msb_cycle * maxLsb - (poc & (maxLsb - 1));
            const int j = findRef(d, value, !sh->lt[i].msb_present, maxLsb - 1, false, keep);
            if (j >= 0)
                keep[j] = lt[j] = true;
            if (sh->lt[i].used && r.nl < 16)
                r.lt[r.nl++] = (int8_t)j;
        }
        for (int s = 0; s < 2; s++) {
            const unsigned n = s ? sh->st_rps.num_positive : sh->st_rps.num_negative;
            for (unsigned i = 0; i < n; i++) {
                const int j = findRef(d, poc + sh->st_rps.delta[s][i], false, 0, true, keep);
                if (j >= 0)
                    keep[j] = true;
                if (!sh->st_rps.used[s][i])
                    continue;
                if (s && r.na < 16)
                    r.after[r.na++] = (int8_t)j;
                if (!s && r.nb < 16)
                    r.before[r.nb++] = (int8_t)j;
            }
        }
        for (int i = 0; i < SLOTS; i++) {
            if (!keep[i])
                d->dpb[i].ref = false;
            if (lt[i])
                d->dpb[i].long_term = true;
        }
    }

    // C.5.2.2: output and removal before the current picture.
    if (irap && noRaslOutput && d->decoded_any) {
        // NoOutputOfPriorPicsFlag: the flag of an IDR or BLA picture; a CRA that starts a sequence after an end of
        // sequence outputs what came before it (as FFmpeg does) rather than dropping it.
        if (nal != HEVC_NAL_CRA && sh->no_output_of_prior_pics)
            dropAll(d);
        hevc_decoder_flush(d);
        removeUnneeded(d);
    } else {
        removeUnneeded(d);
        while (countWhere(d, true) > sps->max_num_reorder || latencyDue(d, sps) ||
               countWhere(d, false) >= sps->max_dec_pic_buffering)
            if (!bump(d))
                break;
    }
    const int configured = configure(d, sps);
    if (configured)
        return configured;

    int cur = -1;
    for (int i = 0; i < SLOTS && cur < 0; i++)
        if (!d->dpb[i].used)
            cur = i;
    if (cur < 0)
        return -1;  // a DPB larger than its SPS allows
    Entry *e = &d->dpb[cur];
    if (!e->handle) {
        void *va;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &e->handle, &va)) {
            e->handle = 0;
            return -3;
        }
        e->va = va;
    } else if (d->b.recycle) {
        void *address = e->va;
        if (d->b.recycle(d->b.ctx, e->handle, d->surface_bytes, &address))
            return -3;
        e->va = address;
    }
    if (hevc_display_buffer(d) && displaySurface(d, e))
        return -3;
    int scratch = -1;
    const int8_t *sets[3] = {r.before, r.after, r.lt};
    const uint8_t counts[3] = {r.nb, r.na, r.nl};
    for (int s = 0; s < 3; s++)
        for (unsigned k = 0; k < counts[s]; k++) {
            if (sets[s][k] < 0)
                d->missing++;
            else if (scratch < 0)
                scratch = sets[s][k];
        }
    if (scratch < 0)
        scratch = cur;

    // The bitstream: every slice segment of the picture after a 4-byte start code (sw_start_code_e).
    uint64_t length = 0;
    for (int i = 0; i < count; i++)
        length += 4 + seg[i].size;
    if (kBitsOff + length + 64 > nvdec_ring_current(&d->ring)->bytes &&
        nvdec_ring_grow(&d->ring, &d->b, kBitsOff + length + 64, 0))
        return -3;
    const NVDECInput *input = nvdec_ring_current(&d->ring);
    uint8_t *bits = input->va + kBitsOff;
    for (int i = 0; i < count; i++) {
        bits[0] = bits[1] = bits[2] = 0, bits[3] = 1;
        memcpy(bits + 4, seg[i].data, seg[i].size);
        bits += 4 + seg[i].size;
    }
    memset(bits, 0, 64);
    uint16_t col[20], row[22];
    if (!hevc_tile_grid(sps, pps, col, row))
        return -1;
    hevc_tile_table((uint16_t *)(input->va + kTilesOff), sps, pps, col, row);
    hevc_scaling_table((nvdec_hevc_scaling_list_s *)(input->va + kScalingOff), hevc_scaling_list(sps, pps));

    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    submit.codec = TEGRA_NVDEC_CODEC_HEVC;
    submit.setup_bytes = sizeof(nvdec_hevc_pic_s);
    hevc_fill_setup(d, (nvdec_hevc_pic_s *)submit.setup, sps, pps, sh, cur, poc, &r, scratch, (uint32_t)length);
    entry(&submit, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_HEVC, 0);
    entry(&submit, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_HEVC | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    entry(&submit, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, d->picture_index++, 0);
    entry(&submit, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kBitsOff);
    entry(&submit, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kStatusOff);
    entry(&submit, NVC5B0_HEVC_SET_SCALING_LIST_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kScalingOff);
    entry(&submit, NVC5B0_HEVC_SET_TILE_SIZES_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kTilesOff);
    entry(&submit, NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->filter_off);
    entry(&submit, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, 0);
    for (uint16_t i = 0; i < SLOTS; i++) {
        const Entry *pic = i == cur || (d->dpb[i].used && d->dpb[i].ref) ? &d->dpb[i] : &d->dpb[scratch];
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pic->handle, 0);
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pic->handle,
              d->luma_bytes);
    }
    if (hevc_display_buffer(d)) {
        entry(&submit, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, e->display, 0);
        entry(&submit, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, e->display, d->luma_bytes / 2);
    }
    entry(&submit, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    submit.clean[0] = (TegraNVDECRange){input->handle, kTilesOff, (uint32_t)(kBitsOff - kTilesOff + length + 64), 0};
    submit.clean_count = 1;
    uint32_t fence = 0;
    if (d->b.submit(d->b.ctx, &submit, &fence))
        return -2;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (d->probe_wanted && !d->probe_ran && idr && d->sample_bytes == 2 && decodedCleanly(d, input, fence)) {
        TegraNVDECWait probeWait = {.invalidate_count = 1};
        probeWait.invalidate[0] = (TegraNVDECRange){e->handle, 0, d->luma_bytes, 0};
        for (uint32_t mode = 2; mode <= 3; mode++)
            hevc_display_probe_run(&d->b, &submit, &probeWait, d->surface_bytes, d->luma_bytes, e->va, d->pitch,
                                   d->rows, sps->width, sps->height, mode, &d->probe[mode - 2]);
        d->probe_ran = true;
    }
#endif
    nvdec_ring_advance(&d->ring);

    // 8.3.1's prevTid0Pic; the current picture becomes a short-term reference (8.3.2).
    if (sh->temporal_id == 0 && !(nal >= HEVC_NAL_RADL_N && nal <= HEVC_NAL_RASL_R) && !(nal <= 14 && !(nal & 1)))
        d->prev_tid0_poc = poc;
    d->decoded_any = true;
    d->stream_start = false;
    d->last_poc = poc;
    e->used = e->ref = true;
    e->long_term = false;
    e->poc = poc;
    e->latency = 0;
    e->output = sh->pic_output;
    e->item = NULL;
    if (e->output) {
        const bool display = hevc_display_buffer(d);
        const H264Picture picture = {
            .luma = display ? e->display_va : e->va,
            .chroma = display ? e->display_va + d->luma_bytes / 2 : e->va + d->luma_bytes,
            .pitch = display ? d->pitch / 2 : d->pitch,
            .handle = display ? e->display : e->handle,
            .chroma_offset = display ? d->luma_bytes / 2 : d->luma_bytes,
            .rows = d->rows,
            .coded_width = sps->width,
            .coded_height = sps->height,
            .crop_x = sps->crop_left,
            .crop_y = sps->crop_top,
            .wide = d->sample_bytes == 2 && !display,
            .width = (uint32_t)(sps->width - sps->crop_left - sps->crop_right),
            .height = (uint32_t)(sps->height - sps->crop_top - sps->crop_bottom),
            .poc = poc,
            .job = fence,
            .token = token,
        };
        e->item = d->b.decoded(d->b.ctx, &picture);
        if (!e->item)
            e->output = false;
    }
    if (!reorder) {
        hevc_decoder_flush(d);
        return sh->pic_output ? 1 : 0;
    }
    // C.5.2.3: latency counts, then the additional bumping.
    for (int i = 0; i < SLOTS; i++)
        if (i != cur && d->dpb[i].used && d->dpb[i].output)
            d->dpb[i].latency++;
    while (countWhere(d, true) > sps->max_num_reorder || latencyDue(d, sps))
        if (!bump(d))
            break;
    return sh->pic_output ? 1 : 0;  // a picture not output is answered at once, as a dropped frame
}

int hevc_decoder_decode(HEVCDecoder *d, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder)
{
    if (lengthSize < 1 || lengthSize > 4)
        return -1;
    Span seg[MAX_SEGMENTS];  // 9.6 KB of stack: each session's decode thread its own
    int count = 0;
    bool eos = false;
    HEVCSliceHeader first;
    for (size_t at = 0; at < size;) {
        if (size - at < lengthSize)
            return -1;
        size_t n = 0;
        for (unsigned i = 0; i < lengthSize; i++)
            n = n << 8 | data[at + i];
        at += lengthSize;
        if (n < 2 || n > size - at)
            return -1;
        const uint8_t *nal = data + at;
        at += n;
        const int type = (nal[0] >> 1) & 0x3f;
        if (type == HEVC_NAL_SPS || type == HEVC_NAL_PPS) {
            if (hevc_decoder_parameter_set(d, nal, n))
                return -1;
        } else if (type == HEVC_NAL_EOS || type == HEVC_NAL_EOB) {
            eos = true;
        } else if (type <= HEVC_NAL_CRA && (type <= HEVC_NAL_RASL_R || type >= HEVC_NAL_BLA_W_LP)) {
            if (!count && (hevc_parse_slice_header(&d->params, nal, n, &first) || !first.first_slice))
                return -1;
            if (count == MAX_SEGMENTS)
                return -1;
            seg[count].data = nal, seg[count++].size = n;
        }
    }
    const int ret = count ? decodePicture(d, seg, count, &first, token, reorder) : 0;
    if (eos)
        d->stream_start = true;  // 8.1.3: the next IRAP picture starts a coded video sequence anew
    return ret;
}
