// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the H.264 and H.265 checkers (TegraNVENCCheck.h, TegraNVENCCheckH265.h) share: limits, buffer sizes NVENC 5.0
// writes, the surface check, and the walk over a submit's methods that gathers how much each relocation may hold.
#ifndef TEGRA_NVENC_CHECK_COMMON_H
#define TEGRA_NVENC_CHECK_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "TegraNVENCABI.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "nvidia/clc5b7.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_NVENC_MAX_DIMENSION = 4096,
    TEGRA_NVENC_MAX_MBS = 36864,  // 4096x2304: bounds a job's time (TegraNVENC.cpp kJobBoundUs)
    TEGRA_NVENC_MIN_DIMENSION = 32,
    TEGRA_NVENC_GPTIMER_MAX = 1u << 27,
    TEGRA_NVENC_STATUS_BYTES = 128 + 16 * 4,  // nvenc_pic_stat_s, then slice statistics (one slice; margin)
    TEGRA_NVENC_SETUP_HEADER = (int)sizeof(nvenc_h264_drv_pic_setup_s),
    // ME predictor records a picture writes for the next one: NVIDIA's T210 driver gives each of its two buffers
    // 128 KiB at 1280x720 (3600 macroblocks); 64 bytes a macroblock covers that with margin.
    TEGRA_NVENC_MEPRED_BYTES_PER_MB = 64,
};

static inline uint32_t tegra_nvenc_align(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// The history buffer: the larger of nvenc_drv.h 2.3's minimum, (((frame_width_minus1 + 16) / 16) + 16) * 128 bytes
// (one macroblock row of 128-byte records), and what NVIDIA's own T210 driver gives NVENC 5.0: HIST_BLOCK_SIZE (192)
// bytes per macroblock of the picture plus one row, (width_mbs * (height_mbs + 1)) * 192 (706560 bytes at 1280x720,
// TegraNVENC/nvidia/SOURCE). The firmware hands the size to the engine (hist_buf_size >> 7 into its history unit,
// H.264 app 0x3333), so a smaller buffer is one the engine can outrun.
static inline uint32_t tegra_nvenc_history_bytes(uint32_t width, uint32_t height)
{
    const uint32_t wmbs = (width + 15) / 16;
    const uint32_t hmbs = (height + 15) / 16;
    const uint32_t row = (((width - 1 + 16) / 16) + 16) * 128;
    const uint32_t picture = wmbs * (hmbs + 1) * HIST_BLOCK_SIZE;
    return row > picture ? row : picture;
}

// Both apps give the engine a block-linear block height as a code (H.264 app 0x8643-0x8664, H.265 0x1417e-0x1419f, to
// I/O 0xe8c): 32 GOBs 4, 16 GOBs 3, 8 GOBs 2, 4 GOBs 1, any other block_height 0. Code k is 2 << k GOBs of 8 rows.
static inline uint32_t tegra_nvenc_block_code(uint32_t blockHeight)
{
    switch (blockHeight & 0x7f) {
    case 32:
        return 4;
    case 16:
        return 3;
    case 8:
        return 2;
    case 4:
        return 1;
    default:
        return 0;
    }
}

// The block_height a layout puts in input_cfg (TegraNVENCABI.h): GOBs a block, 0 for pitch and 16x16 tiles.
static inline uint32_t tegra_nvenc_layout_gobs(uint32_t layout)
{
    switch (layout) {
    case TEGRA_NVENC_LAYOUT_BL2:
        return 2;
    case TEGRA_NVENC_LAYOUT_BL4:
        return 4;
    case TEGRA_NVENC_LAYOUT_BL8:
        return 8;
    case TEGRA_NVENC_LAYOUT_BL16:
        return 16;
    default:
        return 0;
    }
}

// Rows of one block of a plane: a 16x16 tile row, or 2 << code GOBs (code 0 read as two GOBs covers pitch rows too).
static inline uint32_t tegra_nvenc_block_rows(bool tiled, uint32_t blockHeight)
{
    return tiled ? 16 : 16u << tegra_nvenc_block_code(blockHeight);
}

// Luma and chroma rows a surface is laid out with: its plane heights rounded to whole blocks, and luma to at least 64
// and chroma to 32 rows as every surface has been laid out since E3.
static inline uint32_t tegra_nvenc_luma_rows(uint32_t height, uint32_t blockRows)
{
    return tegra_nvenc_align(height, blockRows > 64 ? blockRows : 64);
}

// Rows the engine reads or writes: whole blocks of the picture's planes, nothing past the last block.
static inline uint32_t tegra_nvenc_read_rows(uint32_t planeHeight, uint32_t blockRows)
{
    return tegra_nvenc_align(planeHeight, blockRows);
}

static inline uint32_t tegra_nvenc_chroma_rows(uint32_t height, uint32_t blockRows)
{
    return tegra_nvenc_align((height + 1) / 2, blockRows > 32 ? blockRows : 32);
}

// A surface: luma at its relocation, chroma `chromaOffset` bytes into the luma buffer (surface config) or at its own
// relocation. Every reading is bounded: the config offset alone, the chroma relocation alone, and the two added.
// block_height is a GOB count with an unambiguous code (TegraNVENCABI.h), or 0 or 2; the apps take the pitch in 16-byte
// units in a 9-bit field (I/O 0xe88 bits 18-26, 0xeb8), so 8176 bytes at most.
static inline const char *tegra_nvenc_check_surface(const nvenc_h264_surface_cfg_s *c, uint32_t width, uint32_t height,
                                                    uint64_t lumaAvail, uint64_t chromaAvail, bool reference)
{
    if (c->frame_width_minus1 + 1u != width || c->frame_height_minus1 + 1u != height)
        return "surface dimensions";
    if (c->sfc_trans_mode || c->reserved2 || c->memory_mode || c->nv21_enable || c->input_bl_mode || c->reserved)
        return "surface mode";
    if (c->luma_top_frm_offset || c->luma_bot_offset || c->chroma_bot_offset)
        return "surface field offsets";
    if (reference ? (!c->tiled_16x16 || c->block_height) : (c->tiled_16x16 && c->block_height))
        return "surface tiling";
    if (c->block_height && c->block_height != 2 && !tegra_nvenc_block_code(c->block_height))
        return "block height";
    const uint32_t pitch = c->sfc_pitch;
    if (pitch != c->sfc_pitch_chroma || (pitch & 63) || pitch < tegra_nvenc_align(width, 16) || pitch > 8176)
        return "surface pitch";
    // A block-linear picture is bounded by the blocks the engine reads (a decoder's picture has no more); a 16x16-tiled
    // one, which references and reconstructions are, keeps the 64-row margin of the layout every buffer has.
    const uint32_t blockRows = tegra_nvenc_block_rows(c->tiled_16x16, c->block_height);
    const bool exact = !c->tiled_16x16;
    const uint64_t luma =
        (uint64_t)pitch * (exact ? tegra_nvenc_read_rows(height, blockRows) : tegra_nvenc_luma_rows(height, blockRows));
    const uint64_t chroma = (uint64_t)pitch * (exact ? tegra_nvenc_read_rows((height + 1) / 2, blockRows)
                                                     : tegra_nvenc_chroma_rows(height, blockRows));
    const uint64_t offset = (uint64_t)c->chroma_top_frm_offset * 256;
    if (offset && offset < luma)
        return "chroma overlaps luma";
    if (lumaAvail < luma || lumaAvail < offset + chroma || chromaAvail < offset + chroma)
        return "surface too small";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

enum {
    TEGRA_NVENC_REFS = 16
};

// Address methods a submit may name or leave out: the colocated motion (a reordering H.264 session's) and reference
// slots 1-15 (H.265 P pictures name slot 1 on alternate pictures, H.264 B pictures name it too). The OS keeps a
// method's value across jobs, and the H.264 app programs the engine's colocated registers only from a non-zero value
// (0x333e-0x3398, 0x5866-0x58b0), which then stay as they were; a written 0 in OUT_COLOC_DATA is its error 0xe2640180
// for an IDR or P picture (0x315f-0x319e). So until the falcon next boots, the kernel gives a job that leaves out one
// an earlier job named its scratch buffer there, never the earlier client's address; IN_COLOC_DATA is 0 until then.
enum {
    TEGRA_NVENC_LATCHED = 17,
    TEGRA_NVENC_LATCH_NONE = 0,
    TEGRA_NVENC_LATCH_ZERO = 1,
    TEGRA_NVENC_LATCH_SCRATCH = 2,
};

static inline uint16_t tegra_nvenc_latched_method(uint32_t k)
{
    return k == 0   ? (uint16_t)NVC5B7_SET_IN_COLOC_DATA
           : k == 1 ? (uint16_t)NVC5B7_SET_OUT_COLOC_DATA
                    : (uint16_t)(NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * (k - 1));
}

// What to write in each latched method `s` leaves out (TEGRA_NVENC_LATCH_*), given `latched` (bit k: method k held a
// client's address since the falcon booted), which it updates.
static inline void tegra_nvenc_latch(const TegraNVENCSubmit *s, uint32_t *latched, uint8_t action[TEGRA_NVENC_LATCHED])
{
    uint32_t named = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        for (uint32_t k = 0; k < TEGRA_NVENC_LATCHED; k++)
            named |= (uint32_t)(s->entries[i].method == tegra_nvenc_latched_method(k)) << k;
    }
    for (uint32_t k = 0; k < TEGRA_NVENC_LATCHED; k++) {
        action[k] = named & (1u << k)      ? (uint8_t)TEGRA_NVENC_LATCH_NONE
                    : *latched & (1u << k) ? (uint8_t)TEGRA_NVENC_LATCH_SCRATCH
                    : k == 0               ? (uint8_t)TEGRA_NVENC_LATCH_ZERO
                                           : (uint8_t)TEGRA_NVENC_LATCH_NONE;
    }
    *latched |= named;
}

// Bytes from each relocation to the end of its buffer (1 for one exactly at a buffer's end; 0 for none).
typedef struct {
    uint64_t cur, curChroma, out, outChroma, bits, status, history, colocIn, colocOut, mepredIn, mepredOut;
    uint64_t refLuma[TEGRA_NVENC_REFS], refChroma[TEGRA_NVENC_REFS];
    uint32_t control;  // SET_CONTROL_PARAMS
} TegraNVENCRelocs;

// The methods: SET_APPLICATION_ID (the submit's codec) first, EXECUTE (AWAKEN only) last, CONTROL_PARAMS with codec
// type 3 (H.264 and H.265 alike) and the GP timer on, each method at most once, every relocation one the firmware
// writes or reads a bounded buffer through. Returns NULL or the reason.
static inline const char *tegra_nvenc_relocs(const TegraNVENCSubmit *s, const uint64_t *avail, TegraNVENCRelocs *r)
{
    memset(r, 0, sizeof(*r));
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVENC_ENTRY_VALUE) {
            uint32_t bit = 0;
            switch (m) {
            case NVC5B7_SET_APPLICATION_ID:  // first: the OS selects the app before methods are stored for it
                if (e->value != s->codec)
                    return "application id";
                if (i)
                    return "application id not first";
                bit = 0;
                break;
            // H.264, GPTIMER_ON (required), FORCE_OUTPIC, SLICE_STAT_ON, constant QP, nothing else.
            case NVC5B7_SET_CONTROL_PARAMS:
                if ((e->value & 0xf) != NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264 || (e->value & ~0x190fu) ||
                    !(e->value & (1u << 12)))
                    return "control params";
                r->control = e->value;
                bit = 1;
                break;
            case NVC5B7_SET_PICTURE_INDEX:
                bit = 2;
                break;
            case NVC5B7_SET_WATCHDOG_TIMER:
                bit = 3;
                break;
            case NVC5B7_EXECUTE:  // AWAKEN only: NOTIFY would release a semaphore at a raw address
                if (e->value & ~0x100u)
                    return "execute flags";
                if (i != s->entry_count - 1)
                    return "execute not last";
                bit = 4;
                break;
            default:
                return "value method";
            }
            if (seen & (1u << bit))
                return "repeated method";
            seen |= 1u << bit;
            continue;
        }
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        const uint64_t a = avail[i];
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
        switch (m) {
        case NVC5B7_SET_IN_CUR_PIC:
            slot = &r->cur;
            break;
        case NVC5B7_SET_IN_CUR_PIC_CHROMA_U:
            slot = &r->curChroma;
            break;
        case NVC5B7_SET_OUT_REF_PIC_LUMA:
            slot = &r->out;
            break;
        case NVC5B7_SET_OUT_REF_PIC_CHROMA:
            slot = &r->outChroma;
            break;
        case NVC5B7_SET_OUT_BITSTREAM:
            slot = &r->bits;
            break;
        case NVC5B7_SET_OUT_ENC_STATUS:
            slot = &r->status;
            break;
        case NVC5B7_SET_IOHISTORY:
            slot = &r->history;
            break;
        case NVC5B7_SET_IN_COLOC_DATA:
            slot = &r->colocIn;
            break;
        case NVC5B7_SET_OUT_COLOC_DATA:
            slot = &r->colocOut;
            break;
        case NVC5B7_SET_IN_MEPRED_DATA:
            slot = &r->mepredIn;
            break;
        case NVC5B7_SET_OUT_MEPRED_DATA:
            slot = &r->mepredOut;
            break;
        default:
            if (m >= NVC5B7_SET_IN_REF_PIC0_LUMA && m < NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * TEGRA_NVENC_REFS &&
                !(m & 3)) {
                slot = &r->refLuma[(m - NVC5B7_SET_IN_REF_PIC0_LUMA) / 4];
            } else if (m >= NVC5B7_SET_IN_REF_PIC0_CHROMA && m < NVC5B7_SET_IN_REF_PIC0_CHROMA + 4 * TEGRA_NVENC_REFS &&
                       !(m & 3)) {
                slot = &r->refChroma[(m - NVC5B7_SET_IN_REF_PIC0_CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = a ? a : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

#ifdef __cplusplus
}
#endif

#endif
