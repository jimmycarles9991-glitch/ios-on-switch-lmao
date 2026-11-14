// SPDX-License-Identifier: MIT
// A blit encoder's commands of the tape onto the copy engine (and the 2D engine's mip chains): see encodeRaw.
#import "RawMetalInternal.h"
#include "../fx/fx_mip_route.h"
#include "raw_twod_fill.h"

// What stands for a texture's memory in its blit encoder's order (raw::BlitEncoder::access): its surface, its buffer, or the
// texture a view views
static const void *memoryOf(RMTexture *t)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (t && t->_surface)
        return t->_surface;
    if (t && t->_buffer)
        return (__bridge const void *)t->_buffer;
    return (__bridge const void *)t;
}

static const void *memoryOf(RMBuffer *b)
{
    return (__bridge const void *)b;
}

// The copy engine's copies of those rows into or out of plane z: one copy of every row where they lie a pitch apart, else one
// per row
static void copyRows(raw::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                     uint32_t h, bool intoTexture, uint8_t aspect)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, z, w, h, intoTexture, aspect);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * raw::bufferUnit(t, aspect), t, x,
                             y + k, z, w, 1, intoTexture, aspect);
}

// A copy side's z (raw::BlitEncoder): an array's or cube's layer, else a 3D texture's plane
static uint32_t layerOrPlane(const raw::Texture &t, uint32_t slice, uint32_t z)
{
    return t.layers > 1 ? slice : z;
}

// A buffer<->texture copy of `planes` planes from plane z, `image` bytes apart in the buffer (each plane's rows decided at
// record: bufferPlanes); the buffer's lines cleaned before, or invalidated for the CPU after where the copy writes it
static void copyPlanes(raw::BlitEncoder *be, raw::CommandBuffer *rc, const raw::Buffer &b, uint64_t offset,
                       uint64_t row, uint64_t image, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z,
                       uint32_t w, uint32_t h, uint32_t planes, bool intoTexture, uint8_t aspect)
{
    raw::toElements(t, x, y, w, h);  // (whole blocks: checked at record)
    const uint32_t unit = raw::bufferUnit(t, aspect);
    for (uint32_t k = 0; k < std::max(planes, 1u); k++) {
        const uint64_t at = offset + k * image;
        BufferRows rows;
        bufferRows(at, row, h, w * unit, unit, intoTexture, rows);
        const uint64_t first = bufferRow(rows, at, row, 0);
        if (!b.wc)
            rc->clean(b.cpu + first, bufferRow(rows, at, row, h - 1) - first + w * unit, !intoTexture);
        copyRows(be, b.va, rows, at, row, t, x, y, z + k, w, h, intoTexture, aspect);
    }
}

// A no-copy texture copied into a pool image (raw::Texture::callerPages): what a copy wrote into the image goes on to the
// caller's memory, which stays the texture's contents, after it on the copy engine
static void copyToCallerPages(raw::BlitEncoder *be, raw::CommandBuffer *rc, RMTexture *t)
{
    while (t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    const raw::Texture &x = t->_raw;
    if (!x.callerPages)
        return;
    be->access(memoryOf(t), x.callerPages);
    rc->clean(x.callerPages->bo.cpu, x.storage.bytes, true);
    be->copyBytes(x.imagePitchVa, x.callerPages->bo.va, x.storage.bytes);
}

void encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *be,
                std::set<RMTexture *> &fresh, std::set<RMTexture *> &standInFresh)
{
    switch (c.op) {
    case OpCopyTexture: {
        RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
        useTexture(rc, s, false);
        useTexture(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1), dl = extra(cb, x + 9);
        const uint32_t sx = uint32_t(extra(cb, x + 2)), sy = uint32_t(extra(cb, x + 3)),
                       sz = uint32_t(extra(cb, x + 4)), w = uint32_t(extra(cb, x + 5)), h = uint32_t(extra(cb, x + 6)),
                       d = uint32_t(extra(cb, x + 7)), dx = uint32_t(o), dy = uint32_t(o >> 32),
                       dz = uint32_t(extra(cb, x + 11)), ss = uint32_t(extra(cb, x)), ds = uint32_t(extra(cb, x + 8));
        raw::Texture fromLevel, toLevel;
        if (sl)
            raw::chainLevelImage(s->_raw, uint32_t(sl), fromLevel);
        if (dl)
            raw::chainLevelImage(t->_raw, uint32_t(dl), toLevel);
        uint32_t ex = sx, ey = sy, ew = w, eh = h;
        raw::toElements(s->_raw, ex, ey, ew, eh);  // (whole blocks, the same blocks both sides: checked at record)
        const uint32_t fx = dx / t->_raw.blockW, fy = dy / t->_raw.blockH;
        for (uint32_t k = 0; k < std::max(d, 1u); k++)
            be->copyTexels(sl ? fromLevel : s->_raw, ex, ey, layerOrPlane(s->_raw, ss, sz + k), dl ? toLevel : t->_raw,
                           fx, fy, layerOrPlane(t->_raw, ds, dz + k), ew, eh);
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyBuffer: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
        useBuffer(rc, s, false);
        useBuffer(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
        if (!s->_raw.wc)
            rc->clean(s->_raw.cpu + c.c, n);
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + dof, n, true);
        be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
        break;
    }
    case OpFillBuffer: {
        RMBuffer *t = (__bridge RMBuffer *)c.obj;
        const uint64_t n = extra(cb, c.x);
        useBuffer(rc, t, true);
        be->access(nullptr, memoryOf(t));
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + c.c, n, true);
        be->fillBytes(t->_raw.va + c.c, n, uint8_t(c.a));
        break;
    }
    case OpCopyBufferToTexture: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj;
        RMTexture *t = (__bridge RMTexture *)c.obj2;
        useBuffer(rc, s, false);
        useTexture(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1),
                       image = extra(cb, x + 2);
        const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4), d = extra(cb, x + 5), dl = extra(cb, x + 7),
                       dz = layerOrPlane(t->_raw, uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        raw::Texture level;
        if (dl)
            raw::chainLevelImage(t->_raw, uint32_t(dl), level);
        copyPlanes(be, rc, s->_raw, so, row, image, dl ? level : t->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(dz),
                   uint32_t(w), uint32_t(h), uint32_t(d), true, uint8_t(c.a));
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyTextureToBuffer: {
        RMTexture *s = (__bridge RMTexture *)c.obj;
        RMBuffer *t = (__bridge RMBuffer *)c.obj2;
        useTexture(rc, s, false);
        useBuffer(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 2),
                       sz = layerOrPlane(s->_raw, uint32_t(extra(cb, x)), uint32_t(extra(cb, x + 3))),
                       w = extra(cb, x + 4), h = extra(cb, x + 5), d = extra(cb, x + 6), dof = extra(cb, x + 7),
                       row = extra(cb, x + 8), image = extra(cb, x + 9), sl = extra(cb, x + 1);
        raw::Texture level;
        if (sl)
            raw::chainLevelImage(s->_raw, uint32_t(sl), level);
        copyPlanes(be, rc, t->_raw, dof, row, image, sl ? level : s->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(sz),
                   uint32_t(w), uint32_t(h), uint32_t(d), false, uint8_t(c.a));
        break;
    }
    default:
        break;
    }
}

// fillTexture:level:slice:region:color: (OpFillTexture): the 2D engine's solid rectangle where rule clear.twod holds for the
// format and memory (raw_twod_fill.h), else CLEAR_SURFACE over the region in a pass of its own that loads the rest
bool fillTexturePass(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc)
{
    RMTexture *t = (__bridge RMTexture *)c.obj;
    const uint32_t x0 = uint32_t(extra(cb, c.x)), y0 = uint32_t(extra(cb, c.x + 1));
    const uint32_t x1 = x0 + uint32_t(extra(cb, c.x + 2)), y1 = y0 + uint32_t(extra(cb, c.x + 3));
    float rgba[4];
    for (int k = 0; k < 4; k++) {
        const uint32_t bits = uint32_t(extra(cb, c.x + 4 + k));
        memcpy(&rgba[k], &bits, sizeof bits);
    }
    useTexture(rc, t, true);
    if (x0 >= x1 || y0 >= y1)
        return true;
    if (raw::twodFillHolds(t->_raw.format, t->_raw.comptags)) {
        raw::BlitEncoder *b = rc->blit();
        b->fillTwod(t->_raw, x0, y0, x1, y1, raw::twodFillWord(t->_raw.format, rgba));
        b->end();
        return true;
    }
    raw::PassDesc p;
    p.color[0].texture = &t->_raw;
    p.color[0].load = raw::ColorAttachment::Load;
    p.color[0].store = true;
    p.count = 1;
    p.width = t->_raw.width;
    p.height = t->_raw.height;
    raw::RenderEncoder *e = rc->render(p);
    if (!e)
        return false;
    e->clearRegion(x0, y0, x1, y1, rgba);
    e->end();
    return true;
}

// Levels first + 1 .. last of a chain by the pack's mip state (raw_mip.h): one pass a level, level l + 1 drawn from a header of
// level l alone. Each pass's prologue barrier orders its reads after the pass before wrote.
bool mipPasses(raw::CommandBuffer *rc, RMTexture *t, uint32_t first, uint32_t last, const char **why)
{
    raw::Device &d = *gRM->raw;
    raw::Pipeline *mip = d.mipPipeline(t->_raw.format);
    const raw::fx::MipChain &c = t->_raw.chain->layout;
    if (!mip || last >= c.levels)
        return false;
    useTexture(rc, t, true);
    for (uint32_t l = first; l < last; l++) {
        raw::Texture from = t->_raw, to;
        from.tic = d.levelTic(t->_raw, l);
        if (!from.tic || !raw::chainLevelTarget(t->_raw, l + 1, to))
            return false;
        to.tic = d.levelTic(t->_raw, l + 1);
        raw::PassDesc p;
        p.color[0].texture = &to;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        p.width = c.level[l + 1].width;
        p.height = c.level[l + 1].height;
        raw::RenderEncoder *e = rc->render(p);
        if (!e)
            return false;
        e->setPipeline(mip);
        e->setTexture(0, &from);
        const float inverseSize[2] = {1.0f / float(p.width), 1.0f / float(p.height)};
        e->setBytes(2, 0, inverseSize, sizeof inverseSize);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return false;
        e->end();
    }
    RAW_DIAGNOSTIC(d.stats.mip3dLevels += last - first);
    return true;
}

// generateMipmapsForTexture: each run of the chain's steps on its engine (fx_mip_route.h, decided at record), the blit
// encoder ended around 3D runs and `be` the one encoding goes on with. `probe2d`: a debug build's probe of a format no probe
// covered yet, every step on the 2D engine as that surface format (fx_mip_probe.h)
bool encodeMipmaps(raw::CommandBuffer *rc, RMTexture *t, uint32_t probe2d, raw::BlitEncoder *&be, const char **why)
{
    const raw::Texture::Chain &chain = *t->_raw.chain;
    const uint32_t fmt2d = probe2d ? probe2d : chain.fmt2d;
    bool ok = true;
    const bool routed =
        raw::fx::eachMipRun(chain.layout, fmt2d != 0, !probe2d && gRM->raw->mipPipeline(t->_raw.format) != nullptr,
                            [&](const raw::fx::MipRun &run) {
                                if (!ok)
                                    return;
                                if (run.engine == raw::fx::MipEngine::TwoD) {
                                    useTexture(rc, t, true);
                                    be->access(memoryOf(t), memoryOf(t));
                                    be->generate(t->_raw, fmt2d, run.first, run.last, run.unproven);
                                    return;
                                }
                                be->end();
                                ok = mipPasses(rc, t, run.first, run.last, why);
                                be = rc->blit();
                            });
    return routed && ok;
}
