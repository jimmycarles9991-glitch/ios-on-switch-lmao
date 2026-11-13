// SPDX-License-Identifier: MIT
// A pass whose colour attachment is a turned surface plane (raw_rotation.h): which passes the raw path draws straight into the
// plane, and the window-space state turned into the plane's. Over raw::PassDesc and raw::Texture (raw_gpu.h), or any types
// of the same fields.
#pragma once
#include <algorithm>
#include <type_traits>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmit3D.h"
#include "raw_rotation.h"
#include "raw_stencil_bits.h"

namespace raw {

enum class RotatedRefusal : uint8_t {
    None,
    // attachments of two rotations, or of two frame sizes, or an attachment that is not turned beside one that is whose
    // contents outside the pass matter (loaded or stored): one swizzle serves every target of a pass, and no turn copy state
    // moves this one's texels
    Mixed,
    // rotation 4 loaded: Metal loads the plane transposed and stores it as rotation 1, so a kept texel moves
    LoadsTransposed,
    // a state whose pixel program sees the plane's axes and has no turned variant (PipelineTurnRefused)
    Variant = 4
};

inline const char *rotatedRefusalText(RotatedRefusal r)
{
    switch (r) {
    case RotatedRefusal::Mixed:
        return "a turned attachment beside one of another rotation or size, or a kept one not turned, of a texel no "
               "turn copy moves";
    case RotatedRefusal::LoadsTransposed:
        return "a rotation-4 attachment loaded: Metal loads it transposed";
    case RotatedRefusal::Variant:
        return "a state whose pixel program reads a turned plane's axes and whose turned variant could not be made";
    case RotatedRefusal::None:
        break;
    }
    return "";
}

template <typename Texture> Rotation frameOf(const Texture &t)
{
    Rotation r;
    r.turn = t.rotation;
    const bool quarter = r.quarter();
    r.width = quarter ? t.height : t.width;
    r.height = quarter ? t.width : t.height;
    return r;
}

// The pass's rotation (`out`): one turn and one frame shared by every turned colour attachment, every other attachment
// memoryless or neither loaded nor stored (transient: a backing laid out as the plane stands in for it, planeShaped), or a
// depth stand-in laid out as the plane already (raw_turned_pass.h: its rotation set)
template <typename Pass> RotatedRefusal passRotation(const Pass &pass, Rotation &out)
{
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    out = {};
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const auto *t = pass.color[j].texture;
        if (!t || !t->rotation)
            continue;
        const Rotation r = frameOf(*t);
        if (out.turn && (out.turn != r.turn || out.width != r.width || out.height != r.height))
            return RotatedRefusal::Mixed;
        out = r;
        if (r.turn == 4 && pass.color[j].load == Attachment::Load)
            return RotatedRefusal::LoadsTransposed;
    }
    if (!out.turned())
        return RotatedRefusal::None;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const auto *t = pass.color[j].texture;
            t && !t->rotation && !t->memoryless && (pass.color[j].load == Attachment::Load || pass.color[j].store))
            return RotatedRefusal::Mixed;
    if (const auto *z = pass.depthStencil.texture;
        z && !z->memoryless && !z->rotation && (pass.depthStencil.loaded || pass.depthStencil.stored))
        return RotatedRefusal::Mixed;
    return RotatedRefusal::None;
}

// Why the raw path does not draw `pass` at all, None where passRotation takes it as it is or once its turn copies have moved
// what it keeps (raw_turned_pass.h): every attachment beside the pass's turned one that must move (turned another way or
// of another frame, rotation 4 loaded, or not turned and loaded or stored) of a texel size or format turn copies move
// (a stencil plane by its bits, raw_stencil_bits.h). Still refused: an attachment no turn copy moves.
template <typename Pass> RotatedRefusal turnablePass(const Pass &pass)
{
    const auto quarter = [](Rotation f) {
        if (f.turn == 4)
            f.turn = 1;
        return f;
    };
    Rotation r;
    for (uint32_t j = 0; j < pass.count && j < 8 && !r.turned(); j++)
        if (const auto *t = pass.color[j].texture; t && t->rotation)
            r = quarter(frameOf(*t));
    if (!r.turned())
        return RotatedRefusal::None;
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const auto *t = pass.color[j].texture;
        if (!t)
            continue;
        const bool loaded = pass.color[j].load == Attachment::Load;
        const Rotation f = quarter(frameOf(*t));
        const bool own = t->rotation && f.turn == r.turn && f.width == r.width && f.height == r.height;
        if (own && t->rotation == 4 && loaded && !turnCopyState(t->bpp))
            return RotatedRefusal::LoadsTransposed;
        const bool moves = t->rotation ? !own : !t->memoryless && (loaded || pass.color[j].store);
        if (moves && !turnCopyState(t->bpp))
            return RotatedRefusal::Mixed;
    }
    if (const auto *z = pass.depthStencil.texture;
        z && !z->memoryless && (pass.depthStencil.loaded || pass.depthStencil.stored) && !turnMovesZeta(z->format))
        return RotatedRefusal::Mixed;
    return RotatedRefusal::None;
}

// An attachment of a turned pass that a backing laid out as the plane stands in for
template <typename Texture> bool standsInTurned(const Texture &t, const Rotation &r)
{
    return r.turned() && !t.rotation;
}

// The render area of a turned pass, in its frame: the turned attachments' frame, each other attachment's extent within it
template <typename Pass> void frameArea(const Pass &pass, const Rotation &r, uint32_t &width, uint32_t &height)
{
    width = r.width;
    height = r.height;
    const auto consider = [&](const auto *t) {
        if (t && !t->rotation) {
            width = std::min(width, t->width);
            height = std::min(height, t->height);
        }
    };
    consider(pass.depthStencil.texture);
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        consider(pass.color[j].texture);
}

// What a memoryless attachment's backing is laid out as in the pass: the plane's extent
template <typename Texture> Texture planeShaped(const Texture &m, const Rotation &r)
{
    Texture t = m;
    if (r.turned()) {
        t.width = r.planeWidth();
        t.height = r.planeHeight();
    }
    return t;
}

// The viewport's x and y transform and its clip rectangle (the frame's) as the plane's
inline void turnViewport(const Rotation &r, switch_gm20b_viewport_state &v)
{
    if (!r.turned())
        return;
    const float scale[2] = {v.scale[0], v.scale[1]}, offset[2] = {v.offset[0], v.offset[1]};
    r.viewport(scale, offset, v.scale, v.offset);
    const Rect c = r.rect({v.clip_x, v.clip_y, uint32_t(v.clip_x) + v.clip_width, uint32_t(v.clip_y) + v.clip_height});
    v.clip_x = uint16_t(c.x0);
    v.clip_y = uint16_t(c.y0);
    v.clip_width = uint16_t(c.x1 - c.x0);
    v.clip_height = uint16_t(c.y1 - c.y0);
}

// The sample layout's positions (SET_ANTI_ALIAS_SAMPLE_POSITIONS words, four positions a word) as the plane's pixels hold them
inline void turnPositions(const Rotation &r, uint32_t positions[4])
{
    if (!r.turned())
        return;
    for (uint32_t w = 0; w < 4; w++) {
        uint32_t out = 0;
        for (uint32_t k = 0; k < 4; k++)
            out |= uint32_t(r.samplePosition(uint8_t(positions[w] >> (8 * k)))) << (8 * k);
        positions[w] = out;
    }
}

}  // namespace raw
