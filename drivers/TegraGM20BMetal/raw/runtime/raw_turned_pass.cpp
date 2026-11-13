// SPDX-License-Identifier: MIT
// See raw_turned_pass.h.
#include "raw_turned_pass.h"

#include <algorithm>

#include "raw_area_load.h"

namespace raw {
namespace {

// The scratch backings' tags: a mirror's at + slot, a colour stand-in's at + 8 + slot, the depth stand-in's at + 16
constexpr uint32_t kTurnTag = 48;

Rotation quarterFrame(const Texture &t)
{
    Rotation f = frameOf(t);
    if (f.turn == 4)
        f.turn = 1;
    return f;
}

bool sameFrame(const Rotation &a, const Rotation &b)
{
    return a.turn == b.turn && a.width == b.width && a.height == b.height;
}

// The render area from the origin: every attachment's frame (an unturned one's extent), within the pass's target size
Rect renderArea(const PassDesc &p, const Rotation &rot)
{
    uint32_t w = p.width ? std::min(p.width, rot.width) : rot.width;
    uint32_t h = p.height ? std::min(p.height, rot.height) : rot.height;
    const auto consider = [&](const Texture *t) {
        if (!t)
            return;
        const Rotation f = frameOf(*t);
        w = std::min(w, f.width);
        h = std::min(h, f.height);
    };
    for (uint32_t j = 0; j < p.count && j < 8; j++)
        consider(p.color[j].texture);
    consider(p.depthStencil.texture);
    return {0, 0, w, h};
}

}  // namespace

Texture *TurnedPass::standIn(CommandBuffer &rc, const Texture &t, uint32_t tag, const char **why)
{
    Texture *s = rc.turnScratch(t.zeta ? t.format : turnCopyFormat(t.bpp), rot_.planeWidth(), rot_.planeHeight(), tag);
    if (!s)
        *why = "no plane-shaped stand-in for an attachment beside a turned one";
    return s;
}

Texture *TurnedPass::moved(CommandBuffer &rc, Texture &t, uint32_t tag, const TurnMap *load, const TurnMap *store,
                           const char **why, const TurnMap *reached)
{
    Texture *s = standIn(rc, t, tag, why);
    if (!s || (load && !rc.turnCopy(*s, t, *load, why)))
        return nullptr;
    if (const TurnMask mask = turnMask(reach_, frameToPlane(rot_));
        reached && mask.mask && !rc.turnCopy(*s, t, *reached, why, &mask))
        return nullptr;
    if (store)
        backs_.push_back({&t, s, within(*store, planeArea_)});
    held_.push_back(*s);
    held_.back().format = t.format;
    held_.back().rotation = rot_.turn;
    return &held_.back();
}

bool TurnedPass::mirrored(CommandBuffer &rc, Texture &t, uint32_t tag, const char **why)
{
    if (!reach_.mask)
        return true;
    Texture *temp = rc.turnScratch(turnCopyFormat(t.bpp), t.width, t.height, tag);
    if (!temp) {
        *why = "no scratch for a rotation-4 plane's load";
        return false;
    }
    BlitEncoder *b = rc.blit();
    b->copyTexels(t, 0, 0, 0, *temp, 0, 0, 0, t.width, t.height);
    b->end();
    const TurnMask mask = turnMask(reach_, frameToPlane(rot_));
    return rc.turnCopy(t, *temp, planeMirror(t.width), why, &mask);
}

bool TurnedPass::clearAhead(CommandBuffer &rc, const ColorAttachment &a, const Rect &area, bool draws, const char **why)
{
    PassDesc q;
    q.color[0] = a;
    q.count = 1;
    q.width = area.x1;
    q.height = area.y1;
    q.drawn = draws;
    RenderEncoder *e = rc.render(q);
    if (!e) {
        *why = "no pass to clear an attachment beside a turned one ahead of it";
        return false;
    }
    e->end();
    return true;
}

bool TurnedPass::begin(CommandBuffer &rc, PassDesc &p, DrawReplay &replay, const char **why)
{
    backs_.clear();
    held_.clear();
    rot_ = {};
    reach_ = {};
    for (uint32_t j = 0; j < p.count && j < 8 && !rot_.turned(); j++)
        if (const Texture *t = p.color[j].texture; t && t->rotation)
            rot_ = quarterFrame(*t);
    if (!rot_.turned())
        return true;
    const Rect area = renderArea(p, rot_);
    p.width = area.x1;
    p.height = area.y1;
    planeArea_ = rot_.rect(area);
    const bool runs = replay.any() || runsWithoutDraws(p);
    p.drawn |= runs;
    if ((loadsRotation4(p) || loadsPastArea(p, area.x1, area.y1)) &&
        !rc.reach(p, area.x1, area.y1, replay, reach_, why))
        return false;
    const TurnMap intoPlane = frameToPlane(rot_), outOfPlane = planeToFrame(rot_);
    for (uint32_t j = 0; j < p.count && j < 8; j++) {
        ColorAttachment &a = p.color[j];
        Texture *t = a.texture;
        if (!t)
            continue;
        const bool loaded = a.load == ColorAttachment::Load;
        if (loaded && !loadPastArea(rc, *t, area.x1, area.y1, reach_, kTurnTag + j, why))
            return false;
        if (t->rotation && sameFrame(quarterFrame(*t), rot_)) {
            if (t->rotation != 4)
                continue;
            if (loaded && !mirrored(rc, *t, kTurnTag + j, why))
                return false;
            held_.push_back(*t);
            held_.back().rotation = 1;
            a.texture = &held_.back();
            continue;
        }
        // a stored Clear reaches past the render area (RenderEncoder::clears), where a stand-in of the pass's plane may not:
        // that reach is cleared by a pass of its own first, the stand-in then cleared over the render area
        if (a.load == ColorAttachment::Clear && a.store && !t->memoryless && !clearAhead(rc, a, area, runs, why))
            return false;
        if (t->rotation) {
            const Rotation own = frameOf(*t);
            const TurnMap load = after(planeLoad(own, false), intoPlane), store = after(outOfPlane, frameToPlane(own)),
                          transposed = after(planeLoad(own, true), intoPlane);
            if (!(a.texture = moved(rc, *t, kTurnTag + 8 + j, loaded ? &load : nullptr, a.store ? &store : nullptr, why,
                                    loaded && own.turn == 4 ? &transposed : nullptr)))
                return false;
            continue;
        }
        if (t->memoryless || (!loaded && !a.store))
            continue;
        if (!(a.texture =
                  moved(rc, *t, kTurnTag + 8 + j, loaded ? &intoPlane : nullptr, a.store ? &outOfPlane : nullptr, why)))
            return false;
    }
    DepthStencilAttachment &z = p.depthStencil;
    if (z.texture && !z.texture->memoryless && (z.loaded || z.stored) &&
        !(z.texture = moved(rc, *z.texture, kTurnTag + 16, z.loaded ? &intoPlane : nullptr,
                            z.stored ? &outOfPlane : nullptr, why)))
        return false;
    return true;
}

bool TurnedPass::end(CommandBuffer &rc, const char **why)
{
    for (const Back &b : backs_)
        if (!rc.turnCopy(*b.into, *b.standIn, b.map, why))
            return false;
    backs_.clear();
    held_.clear();
    return true;
}

}  // namespace raw
