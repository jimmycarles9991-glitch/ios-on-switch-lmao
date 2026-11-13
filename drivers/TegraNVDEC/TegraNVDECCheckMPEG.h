// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDECCheck.h's bounds for MPEG-4 Part 2 and MPEG-1/2 setups, included by it. Picture slots 0-2 are the current
// picture and the past and future references (averne's nvtegra_mpeg4.c, nvtegra_mpeg12.c); the current one is written
// in whole macroblock pairs (pitch x 32-aligned height, NV12 block linear). MPEG-4's colocated motion and history
// buffers are read and written and its scratch buffer holds what scratch_pic_buffer_size says; MPEG-1/2 reads slice
// offsets and binds no history buffer, so its setup may name none. Display output (the engine's TF DMA) and the
// histogram write to addresses no relocation names, so both stay off, as do NV24 surfaces and DivX's modes.
#ifndef TEGRA_NVDEC_CHECK_MPEG_H
#define TEGRA_NVDEC_CHECK_MPEG_H

#ifdef __cplusplus
static_assert(sizeof(nvdec_mpeg4_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "MPEG-4 setup travels inline");
static_assert(sizeof(nvdec_mpeg2_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "MPEG-2 setup travels inline");
#else
_Static_assert(sizeof(nvdec_mpeg4_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "MPEG-4 setup travels inline");
_Static_assert(sizeof(nvdec_mpeg2_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "MPEG-2 setup travels inline");
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline bool tegra_nvdec_zero(const void *bytes, size_t size)
{
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < size; i++) {
        if (p[i])
            return false;
    }
    return true;
}

static inline const char *tegra_nvdec_check_mpeg4(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum {
        LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0,
        CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0,
        SLOTS = 3
    };

    enum {
        BITS,
        STATUS,
        COLOC,
        HISTORY,
        SCRATCH,
        BUFFERS
    };

    if (s->setup_bytes != sizeof(nvdec_mpeg4_pic_s))
        return "setup size";
    uint64_t buf[BUFFERS] = {0};
    uint64_t luma[SLOTS] = {0};
    uint64_t chroma[SLOTS] = {0};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            const char *why = tegra_nvdec_check_value(s, i, &seen);
            if (why)
                return why;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET:
            slot = &buf[BITS];
            break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET:
            slot = &buf[STATUS];
            break;
        case NVC5B0_SET_COLOC_DATA_OFFSET:
            slot = &buf[COLOC];
            break;
        case NVC5B0_SET_HISTORY_OFFSET:
            slot = &buf[HISTORY];
            break;
        case NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET:
            slot = &buf[SCRATCH];
            break;
        default:
            if (m >= LUMA && m < LUMA + 4 * SLOTS && !(m & 3)) {
                slot = &luma[(m - LUMA) / 4];
            } else if (m >= CHROMA && m < CHROMA + 4 * SLOTS && !(m & 3)) {
                slot = &chroma[(m - CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = avail[i] ? avail[i] : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    for (uint32_t i = 0; i < BUFFERS; i++) {  // NOLINT(modernize-loop-convert): C too
        if (!buf[i])
            return "missing relocation";
    }
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (!luma[i] || !chroma[i])
            return "missing relocation";
    }

    const nvdec_mpeg4_pic_s *p = (const nvdec_mpeg4_pic_s *)s->setup;
    if (!tegra_nvdec_zero(&p->encryption_params, sizeof(p->encryption_params)))
        return "encryption parameters";
    if (!tegra_nvdec_zero(&p->displayPara, sizeof(p->displayPara)))
        return "display output or histogram";
    if (p->gptimer_timeout_value > (1u << 27))
        return "GP timer limit";
    if (p->tileFormat || p->gob_height || p->reserverd_surface_format || p->curr_output_memory_layout ||
        p->ref_memory_layout[0] || p->ref_memory_layout[1])
        return "surface format";
    if (p->divx_flags)
        return "DivX modes";
    const uint32_t w = (uint32_t)p->width;
    const uint32_t h = (uint32_t)p->height;
    if (p->width != p->FrameWidth || p->height != p->FrameHeight || w < 16 || w > 2048 || h < 16 || h > 2048 ||
        (w & 15) || (h & 15))
        return "dimensions";
    if (p->vop_time_increment_bitcount < 1 || p->vop_time_increment_bitcount > 16)
        return "time increment bits";
    if (p->luma_top_offset || p->luma_bot_offset || p->luma_frame_offset || p->chroma_top_offset ||
        p->chroma_bot_offset || p->chroma_frame_offset)
        return "surface offsets";
    const uint32_t pitch = p->FrameStride[0];
    if (pitch != p->FrameStride[1] || (pitch & 63) || pitch < w || pitch > 16384)
        return "pitch";
    const uint64_t lumaBytes = (uint64_t)pitch * ((h + 31) & ~31u);
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2)
            return "surface too small";
    }
    const uint64_t wmbs = w / 16;
    const uint64_t hmbs = h / 16;
    if (buf[COLOC] < ((((hmbs + 1) & ~1ULL) * wmbs * 64 - 63 + 255) & ~255ULL))
        return "colocated buffer";
    if ((uint64_t)p->HistBufferSize * 256 < ((wmbs * 256 + 0x1100 + 255) & ~255ULL) ||
        buf[HISTORY] < (uint64_t)p->HistBufferSize * 256)
        return "history";
    if (p->scratch_pic_buffer_size != 0x400 || buf[SCRATCH] < p->scratch_pic_buffer_size)
        return "scratch";
    if (buf[STATUS] < sizeof(nvdec_status_s))
        return "status";
    if (!p->stream_len || buf[BITS] < p->stream_len)
        return "bitstream";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

static inline const char *tegra_nvdec_check_mpeg2(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum {
        LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0,
        CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0,
        SLOTS = 3
    };

    enum {
        BITS,
        SLICES,
        STATUS,
        BUFFERS
    };

    if (s->setup_bytes != sizeof(nvdec_mpeg2_pic_s))
        return "setup size";
    uint64_t buf[BUFFERS] = {0};
    uint64_t luma[SLOTS] = {0};
    uint64_t chroma[SLOTS] = {0};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            const char *why = tegra_nvdec_check_value(s, i, &seen);
            if (why)
                return why;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET:
            slot = &buf[BITS];
            break;
        case NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET:
            slot = &buf[SLICES];
            break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET:
            slot = &buf[STATUS];
            break;
        default:
            if (m >= LUMA && m < LUMA + 4 * SLOTS && !(m & 3)) {
                slot = &luma[(m - LUMA) / 4];
            } else if (m >= CHROMA && m < CHROMA + 4 * SLOTS && !(m & 3)) {
                slot = &chroma[(m - CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = avail[i] ? avail[i] : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    for (uint32_t i = 0; i < BUFFERS; i++) {  // NOLINT(modernize-loop-convert): C too
        if (!buf[i])
            return "missing relocation";
    }
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (!luma[i] || !chroma[i])
            return "missing relocation";
    }

    const nvdec_mpeg2_pic_s *p = (const nvdec_mpeg2_pic_s *)s->setup;
    if (!tegra_nvdec_zero(&p->encryption_params, sizeof(p->encryption_params)) ||
        !tegra_nvdec_zero(&p->ssm, sizeof(p->ssm)))
        return "encryption parameters";
    if (!tegra_nvdec_zero(&p->displayPara, sizeof(p->displayPara)))
        return "display output or histogram";
    if (p->gptimer_timeout_value > (1u << 27))
        return "GP timer limit";
    if (p->tileFormat || p->gob_height || p->reserverd_surface_format || p->output_memory_layout ||
        p->ref_memory_layout[0] || p->ref_memory_layout[1])
        return "surface format";
    if (p->HistBufferSize || p->MbInfoSizeInBytes)
        return "history";
    const uint32_t wmbs = p->PicWidthInMbs;
    const uint32_t hmbs = p->FrameHeightInMbs;
    if (wmbs < 1 || wmbs > 256 || hmbs < 1 || hmbs > 256 || p->FrameWidth != (int)(wmbs * 16) ||
        p->FrameHeight != (int)(hmbs * 16))
        return "dimensions";
    if (p->picture_structure < 1 || p->picture_structure > 3 || (p->secondfield && p->picture_structure == 3))
        return "picture structure";
    if (p->luma_top_offset || p->luma_bot_offset || p->luma_frame_offset || p->chroma_top_offset ||
        p->chroma_bot_offset || p->chroma_frame_offset)
        return "surface offsets";
    const uint32_t pitch = p->pitch_luma;
    if (pitch != p->pitch_chroma || (pitch & 63) || pitch < wmbs * 16 || pitch > 16384)
        return "pitch";
    const uint64_t lumaBytes = (uint64_t)pitch * ((hmbs * 16 + 31) & ~31u);
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2)
            return "surface too small";
    }
    if (buf[STATUS] < sizeof(nvdec_status_s))
        return "status";
    if (!p->slice_count || buf[SLICES] < ((uint64_t)p->slice_count + 1) * 4 || !p->stream_len ||
        buf[BITS] < p->stream_len)
        return "bitstream";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

#ifdef __cplusplus
}
#endif

#endif
