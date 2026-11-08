// SPDX-License-Identifier: BSD-2-Clause-Patent
// The transform probes the unit's boot self-test ran: which OutputFlipX/FlipY/Transpose combination implements which
// IOSurfaceAccelerator transform, how a transposed job's rectangles and output surface are given (before or after the
// transpose), and where a destination rectangle inside a larger surface lands under each transform. Run on
// vic_model.h, the model of VIC 4.0's output layout fitted to the unit's diag lines and to hekate's rotation, they give
// kTegraVICTraits (tegra_vic_transpose_test). The includer declares snprintf.
#ifndef TEGRA_VIC_PROBE_H
#define TEGRA_VIC_PROBE_H
#include <stdint.h>
#include <string.h>
#include "../TegraVICConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// The card: 32x16 BGRA whose every pixel names its own coordinates; outputs in 256-byte rows.
enum {
    TEGRA_VIC_CARD_W = 32,
    TEGRA_VIC_CARD_H = 16,
    TEGRA_VIC_PROBE_PITCH = 256,
    TEGRA_VIC_POISON_BYTE = 0xab
};

#define TEGRA_VIC_POISON_PIXEL 0xababababu

static inline uint32_t tegra_vic_card_pixel(uint32_t x, uint32_t y)
{
    const uint32_t b = (x * 7 + y * 13 + 0x21) & 0xff;
    return 0xff000000u | (8 * x) << 16 | (16 * y) << 8 | b;
}

static inline uint32_t tegra_vic_probe_pixel(const uint8_t *va, uint32_t x, uint32_t y)
{
    return ((const uint32_t *)(va + (size_t)y * TEGRA_VIC_PROBE_PITCH))[x];
}

// Where source (x, y) goes under IOSurfaceAccelerator transform t (flips first, then a clockwise quarter turn), for a
// w x h source: output (u, v).
static inline void tegra_vic_transform_point(uint32_t t, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t *u,
                                             uint32_t *v)
{
    if (t & TEGRA_VIC_FLIP_H)
        x = w - 1 - x;
    if (t & TEGRA_VIC_FLIP_V)
        y = h - 1 - y;
    if (t & TEGRA_VIC_ROT90) {
        *u = h - 1 - y;
        *v = x;
    } else {
        *u = x;
        *v = y;
    }
}

// Where an output pixel came from: "x,y" of the card pixel it holds, "-" unwritten (the poison), "?" anything else.
static inline void tegra_vic_card_source(uint32_t pixel, char *out, size_t size)
{
    const uint32_t x = ((pixel >> 16) & 0xff) / 8;
    const uint32_t y = ((pixel >> 8) & 0xff) / 16;
    if (pixel == TEGRA_VIC_POISON_PIXEL) {
        snprintf(out, size, "-");
    } else if (x < TEGRA_VIC_CARD_W && y < TEGRA_VIC_CARD_H && pixel == tegra_vic_card_pixel(x, y)) {
        snprintf(out, size, "%u,%u", x, y);
    } else {
        snprintf(out, size, "?%08x", pixel);
    }
}

// The card pixels at an ow x oh output's corners and next to its origin, appended to `text` (" (0,0)<-1,0 ...").
static inline void tegra_vic_probe_points(const uint8_t *va, uint32_t ow, uint32_t oh, char *text, size_t size)
{
    const uint32_t at[6][2] = {{0, 0}, {1, 0}, {0, 1}, {ow - 1, 0}, {0, oh - 1}, {ow - 1, oh - 1}};
    size_t n = 0;
    if (size)
        text[0] = 0;
    for (uint32_t i = 0; i < 6 && n < size; i++) {
        char from[16];
        tegra_vic_card_source(tegra_vic_probe_pixel(va, at[i][0], at[i][1]), from, sizeof(from));
        const int k = snprintf(text + n, size - n, " (%u,%u)<-%s", at[i][0], at[i][1], from);
        if (k < 0)
            break;
        n += (size_t)k;
    }
}

static inline int64_t tegra_vic_probe_find(const uint8_t *va, uint64_t bytes, uint32_t value)
{
    for (uint64_t i = 0; i + 4 <= bytes; i += 4) {
        if (*(const uint32_t *)(va + i) == value)
            return (int64_t)i;
    }
    return -1;
}

// Which transform of the card the ow x oh output at (ox, oy) is, or -1.
static inline int tegra_vic_probe_recognise(const uint8_t *va, uint32_t ox, uint32_t oy, uint32_t ow, uint32_t oh)
{
    for (uint32_t t = 0; t < 8; t++) {
        const int rot = static_cast<int>((t & TEGRA_VIC_ROT90) != 0);
        if ((rot ? TEGRA_VIC_CARD_H : TEGRA_VIC_CARD_W) != (int)ow ||
            (rot ? TEGRA_VIC_CARD_W : TEGRA_VIC_CARD_H) != (int)oh)
            continue;
        int match = 1;
        for (uint32_t y = 0; match && y < TEGRA_VIC_CARD_H; y++) {
            for (uint32_t x = 0; match && x < TEGRA_VIC_CARD_W; x++) {
                uint32_t u = 0;
                uint32_t v = 0;
                tegra_vic_transform_point(t, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, x, y, &u, &v);
                match = static_cast<int>(tegra_vic_probe_pixel(va, ox + u, oy + v) == tegra_vic_card_pixel(x, y));
            }
        }
        if (match)
            return (int)t;
    }
    return -1;
}

// What the probes run on: the engine (the kernel) or the model (the host test).
typedef struct {
    // Runs one job: the destination poisoned (TEGRA_VIC_POISON_BYTE) before, readable at dstVa after. 0, or an error
    // with *why set.
    int (*run)(void *ctx, const TegraVICImage *src, const TegraVICImage *dst, const TegraVICOp *op,
               const TegraVICTraits *t, const char **why);
    void (*note)(void *ctx, const char *line);  // a one-shot diagnostic line (the kernel: os_log, the log only)
    void *ctx;
    uint8_t *srcVa;
    const uint8_t *dstVa;
    uint64_t srcIova, srcBytes, dstIova, dstBytes;
} TegraVICProbeEnv;

static inline TegraVICImage tegra_vic_probe_bgra(uint64_t iova, uint64_t bytes, uint32_t w, uint32_t h)
{
    TegraVICImage im;
    memset(&im, 0, sizeof(im));
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = w;
    im.height = h;
    im.pitch[0] = TEGRA_VIC_PROBE_PITCH;
    im.rows[0] = h;
    im.base[0] = iova;
    im.span[0] = bytes;
    return im;
}

static inline TegraVICOp tegra_vic_probe_op(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint32_t transform)
{
    TegraVICOp op;
    memset(&op, 0, sizeof(op));
    op.srcW = (uint64_t)sw << 16;
    op.srcH = (uint64_t)sh << 16;
    op.dstW = dw;
    op.dstH = dh;
    op.transform = transform;
    op.nearest = 1;
    return op;
}

static inline void tegra_vic_probe_draw_card(const TegraVICProbeEnv *e)
{
    memset(e->srcVa, 0, (size_t)e->srcBytes);
    for (uint32_t y = 0; y < TEGRA_VIC_CARD_H; y++) {
        for (uint32_t x = 0; x < TEGRA_VIC_CARD_W; x++)
            ((uint32_t *)(e->srcVa + (size_t)y * TEGRA_VIC_PROBE_PITCH))[x] = tegra_vic_card_pixel(x, y);
    }
}

static inline const char *tegra_vic_probe_when(uint32_t before)
{
    return before ? "before" : "after";
}

// The plain transpose's line, one per convention whatever came out, so a boot shows which held and why the others
// did not. A transpose puts card 1,0 at byte 256 (output 0,1) and 0,1 at byte 4 (output 1,0); the planes'
// geometry left post-transpose put 1,0 at 128 on the unit: a row stride of LumaHeight + 1 = 32 pixels.
static inline void tegra_vic_probe_note_transpose(const TegraVICProbeEnv *e, uint32_t convention, int got, uint32_t ow,
                                                  uint32_t oh)
{
    char text[340];
    char what[24];
    if (got >= 0) {
        snprintf(what, sizeof(what), "transform %d", got);
    } else {
        snprintf(what, sizeof(what), "no transform");
    }
    const int n = snprintf(text, sizeof(text),
                           "TegraVIC: diag transpose rects %s, surface %s the transpose (%ux%u out of %ux%u): %s; card "
                           "1,0 at byte %lld, 0,1 at %lld; output pixel <- card",
                           tegra_vic_probe_when(convention & 1), tegra_vic_probe_when(convention & 2), ow, oh,
                           (uint32_t)TEGRA_VIC_CARD_W, (uint32_t)TEGRA_VIC_CARD_H, what,
                           (long long)tegra_vic_probe_find(e->dstVa, e->dstBytes, tegra_vic_card_pixel(1, 0)),
                           (long long)tegra_vic_probe_find(e->dstVa, e->dstBytes, tegra_vic_card_pixel(0, 1)));
    if (n > 0 && (size_t)n < sizeof(text))
        tegra_vic_probe_points(e->dstVa, ow, oh, text + n, sizeof(text) - (size_t)n);
    e->note(e->ctx, text);
}

// Each of the 8 VIC combinations on the card (1:1, whole surface), through a trait that maps IOSurfaceAccelerator
// transform 0 onto it; the transposed ones under the four conventions. The plain transpose runs all four (each noted);
// the first that copies the card as some transform, in the order below, is the convention every later transposed
// combination and every job uses. Returns 1, or 0 with the V4 FAIL line in `line` (only a 1:1 copy that is not the
// card fails V4; a combination that is no transform is left out of t->rotations).
static inline int tegra_vic_probe_transforms(const TegraVICProbeEnv *e, TegraVICTraits *t, char *line, size_t size)
{
    // Conventions: bit 0 the rectangles, bit 1 the output surface, given before the transpose. Everything before it
    // first (hekate's programming, which the unit's lines point to), everything after it last.
    static const uint32_t order[4] = {3, 1, 2, 0};
    tegra_vic_probe_draw_card(e);
    const TegraVICImage src = tegra_vic_probe_bgra(e->srcIova, e->srcBytes, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H);
    int pre = -1;
    for (uint32_t combo = 0; combo < 8; combo++) {
        const uint32_t fx = combo & 1;
        const uint32_t fy = (combo >> 1) & 1;
        const uint32_t tr = (combo >> 2) & 1;
        const int plain = static_cast<int>((tr != 0u) && (fx == 0u) && fy == 0u);
        int found = -1;
        for (uint32_t k = 0; k < (tr ? 4u : 1u); k++) {
            const uint32_t convention = tr ? order[k] : 0;
            if (found >= 0 && !plain)
                break;
            if (tr && !plain && pre >= 0 && (int)convention != pre)
                continue;
            TegraVICTraits probe = *t;
            probe.vicFlips[0][0] = (uint8_t)fx;
            probe.vicFlips[0][1] = (uint8_t)fy;
            probe.vicFlips[0][2] = (uint8_t)tr;
            probe.preTransposeRects = convention & 1;
            probe.preTransposeSurface = convention >> 1;
            probe.rotations = 1;
            const uint32_t ow = tr ? TEGRA_VIC_CARD_H : TEGRA_VIC_CARD_W;
            const uint32_t oh = tr ? TEGRA_VIC_CARD_W : TEGRA_VIC_CARD_H;
            const TegraVICImage dst = tegra_vic_probe_bgra(e->dstIova, e->dstBytes, ow, oh);
            const TegraVICOp op = tegra_vic_probe_op(TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, ow, oh, 0);
            const char *why = nullptr;
            if (e->run(e->ctx, &src, &dst, &op, &probe, &why)) {
                snprintf(line, size, "V4 FAIL transform probe %u%u%u: %s", fx, fy, tr, why ? why : "job failed");
                return 0;
            }
            const int got = tegra_vic_probe_recognise(e->dstVa, 0, 0, ow, oh);
            if (plain)
                tegra_vic_probe_note_transpose(e, convention, got, ow, oh);
            if (got >= 0 && found < 0) {
                found = got;
                if (tr && pre < 0)
                    pre = (int)convention;
            }
        }
        if (found < 0 || (t->rotations & (1u << static_cast<uint32_t>(found))))
            continue;  // no transform, or one already had
        t->vicFlips[found][0] = (uint8_t)fx;
        t->vicFlips[found][1] = (uint8_t)fy;
        t->vicFlips[found][2] = (uint8_t)tr;
        t->rotations |= 1u << static_cast<uint32_t>(found);
    }
    t->preTransposeRects = pre >= 0 ? (uint32_t)pre & 1 : 0;
    t->preTransposeSurface = pre >= 0 ? (uint32_t)pre >> 1 : 0;
    if (!(t->rotations & 1)) {
        snprintf(line, size, "V4 FAIL a 1:1 BGRA copy is not the source (first pixels %#x %#x, want %#x %#x)",
                 tegra_vic_probe_pixel(e->dstVa, 0, 0), tegra_vic_probe_pixel(e->dstVa, 1, 0),
                 tegra_vic_card_pixel(0, 0), tegra_vic_card_pixel(1, 0));
        return 0;
    }
    return 1;
}

// Where a destination rectangle inside a larger surface lands: the card under each proven transform 1-7 into a 64x48
// surface at (8, 4), every pixel of the rectangle checked. A flip that mirrors around the surface, not the rectangle,
// lands the card elsewhere: that transform keeps whole-surface jobs only (tegra_vic_build refuses the rest; callers fall
// back) and one line says where it went. Never fails V4. Transform 0 needs no proof (fills and copies place it).
enum {
    TEGRA_VIC_PLACE_W = 64,
    TEGRA_VIC_PLACE_H = 48,
    TEGRA_VIC_PLACE_X = 8,
    TEGRA_VIC_PLACE_Y = 4
};

static inline void tegra_vic_probe_placement(const TegraVICProbeEnv *e, TegraVICTraits *t)
{
    tegra_vic_probe_draw_card(e);
    const TegraVICImage src = tegra_vic_probe_bgra(e->srcIova, e->srcBytes, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H);
    const TegraVICImage dst = tegra_vic_probe_bgra(e->dstIova, e->dstBytes, TEGRA_VIC_PLACE_W, TEGRA_VIC_PLACE_H);
    t->subRects = 1;
    for (uint32_t tr = 1; tr < 8; tr++) {
        if (!(t->rotations & (1u << tr)))
            continue;
        const int rot = static_cast<int>((tr & TEGRA_VIC_ROT90) != 0);
        const uint32_t w = rot ? TEGRA_VIC_CARD_H : TEGRA_VIC_CARD_W;
        const uint32_t h = rot ? TEGRA_VIC_CARD_W : TEGRA_VIC_CARD_H;
        TegraVICOp op = tegra_vic_probe_op(TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, w, h, tr);
        op.dstX = TEGRA_VIC_PLACE_X;
        op.dstY = TEGRA_VIC_PLACE_Y;
        TegraVICTraits probe = *t;
        probe.subRects = 0xff;
        const char *why = nullptr;
        const int failed = e->run(e->ctx, &src, &dst, &op, &probe, &why);
        uint32_t off = 0;
        uint32_t around = 0;
        if (!failed) {
            for (uint32_t y = 0; y < TEGRA_VIC_CARD_H; y++) {
                for (uint32_t x = 0; x < TEGRA_VIC_CARD_W; x++) {
                    uint32_t u = 0;
                    uint32_t v = 0;
                    tegra_vic_transform_point(tr, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, x, y, &u, &v);
                    off += static_cast<uint32_t>(
                        tegra_vic_probe_pixel(e->dstVa, TEGRA_VIC_PLACE_X + u, TEGRA_VIC_PLACE_Y + v) !=
                        tegra_vic_card_pixel(x, y));
                }
            }
            for (uint32_t y = 0; y < TEGRA_VIC_PLACE_H; y++) {
                for (uint32_t x = 0; x < TEGRA_VIC_PLACE_W; x++) {
                    const int inside = static_cast<int>(x >= TEGRA_VIC_PLACE_X && x < TEGRA_VIC_PLACE_X + w &&
                                                        y >= TEGRA_VIC_PLACE_Y && y < TEGRA_VIC_PLACE_Y + h);
                    around += static_cast<uint32_t>((inside == 0) &&
                                                    tegra_vic_probe_pixel(e->dstVa, x, y) != TEGRA_VIC_POISON_PIXEL);
                }
            }
            if (!off) {
                t->subRects |= 1u << tr;
                continue;
            }
        }
        uint32_t u0 = 0;
        uint32_t v0 = 0;
        tegra_vic_transform_point(tr, TEGRA_VIC_CARD_W, TEGRA_VIC_CARD_H, 0, 0, &u0, &v0);
        const int64_t at = failed ? -1 : tegra_vic_probe_find(e->dstVa, e->dstBytes, tegra_vic_card_pixel(0, 0));
        const char *outcome = "landed elsewhere";
        if (failed)
            outcome = why ? why : "job failed";
        char text[240];
        snprintf(text, sizeof(text),
                 "TegraVIC: diag sub-rectangle transform %u (%ux%u at %u,%u in %ux%u): %s; %u of %u pixels off, %u "
                 "around it written, card 0,0 at %lld,%lld (want %u,%u): whole-surface jobs only",
                 tr, w, h, (uint32_t)TEGRA_VIC_PLACE_X, (uint32_t)TEGRA_VIC_PLACE_Y, (uint32_t)TEGRA_VIC_PLACE_W,
                 (uint32_t)TEGRA_VIC_PLACE_H, outcome, off, (uint32_t)(TEGRA_VIC_CARD_W * TEGRA_VIC_CARD_H), around,
                 at < 0 ? -1LL : (long long)((at % TEGRA_VIC_PROBE_PITCH) / 4),
                 at < 0 ? -1LL : (long long)(at / TEGRA_VIC_PROBE_PITCH), TEGRA_VIC_PLACE_X + u0,
                 TEGRA_VIC_PLACE_Y + v0);
        e->note(e->ctx, text);
    }
}

#ifdef __cplusplus
}
#endif
#endif
