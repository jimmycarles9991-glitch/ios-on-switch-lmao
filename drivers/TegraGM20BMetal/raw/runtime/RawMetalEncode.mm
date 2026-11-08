// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// The copy engine's copies of those rows: one copy of every row where they lie a pitch apart, else one per row
static void copyRows(raw::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     bool intoTexture)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, w, h, intoTexture);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * t.bpp, t, x, y + k, w, 1,
                             intoTexture);
}

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then
// The host imports a texture's memory lies in, named to the submission (raw::CommandBuffer::use): a surface's or a planar
// texture's planes, a buffer texture's buffer's, a view's parent's; and a YCbCr texture's planes, which only its conversion
// reads (its image is pool memory).
static void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (!t)
        return;
    rc->use(t->_buffer ? t->_buffer->_raw.import : t->_raw.import, write);
    for (const raw::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        if (plane)
            rc->use(plane->import, false);
}

static void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write)
{
    if (b)
        rc->use(b->_raw.import, write);
}

bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why)
{
    raw::Device &d = *gRM->raw;
    raw::CommandBuffer *rc = d.begin();
    if (!rc) {
        *why = "the kernel gave the raw path no channel";
        if (!predLogged(PredEncNoChannel))
            logPred(PredEncNoChannel, 0, 0, @(d.channelError().c_str()));
        hit(cb, PredEncNoChannel);
        return false;
    }
    raw::RenderEncoder *re = nullptr;
    raw::BlitEncoder *be = nullptr;

    auto fail = [&](RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0) {
        *why = w;
        hit(cb, p, a, b);
        rc->abandon();
        return false;
    };
    // a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
    auto refused = [&]() {
        static const RMPred map[] = {PredEncPassRender, PredEncIndexedRange, PredEncColorFormats,
                                     PredEncUboOffset,  PredEncNoTic,        PredEncDepthStencil};
        const raw::Refusal &f = rc->refusal;
        hit(cb, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
        rc->abandon();
        return false;
    };
    // A planar render target's stand-in passes (raw::Texture::planarStandIn), the frontend's: the planes' codes into the stand-in
    // before a pass that loads it (its code image from the planes first, the copy engine's remaps), the stand-in into the planes
    // after one that stores it. 0: drawn; else fail()'s or refused()'s answer.
    std::vector<RMTexture *> standInLoads, standInStores, planesWritten;
    auto standInPass = [&](RMTexture *t, bool store) -> int {
        raw::Texture &x = t->_raw;
        if (!store) {
            raw::BlitEncoder *b = rc->blit();
            b->convertPlanar(x);
            b->end();
        }
        raw::PassDesc p;
        p.color[0].texture = store ? x.luma : x.planarStandIn;
        p.color[1].texture = store ? x.chroma : nullptr;
        for (raw::ColorAttachment &a : p.color) {
            a.load = raw::ColorAttachment::DontCare;
            a.store = true;
        }
        p.count = store ? 2 : 1;
        raw::RenderEncoder *e = rc->render(p);
        if (!e) {
            fail(PredEncPlanarRT, "a planar texture's stand-in pass the raw path cannot render", store);
            return 1;
        }
        e->setPipeline(d.standInPipeline(store));
        e->setTexture(0, store ? x.planarStandIn : &x);
        if (!e->draw(3 /* MTLPrimitiveTypeTriangle */, 0, 3, 1, 0, why)) {
            refused();
            return 2;
        }
        e->end();
        if (store)
            planesWritten.push_back(t);
        return 0;
    };

    // the planar textures its draws sample: their images from their planes first, once (m2v converts at each encoder that uses
    // one; nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's
    // full barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
    // image is made when a draw samples it. A sampler that clamps to zero or to the border reads a 4:4:4 code texture as clamp to
    // edge (m2v's address map), which the raw path does not derive: such a command buffer is m2v's.
    struct Conversion {
        RMTexture *t;
        uint32_t seed;
        uint64_t writes;
    };

    std::vector<Conversion> converted;  // planar images converted here, at their surfaces' seeds and write counts
    {
        std::vector<RMTexture *> planar, rendered;
        std::vector<RMSampler *> samplers;
        auto isPlanar = [](void *o) {
            return o && ((__bridge RMTexture *)o)->_raw.converted();
        };
        RMTexture *bound[16] = {};  // the open render encoder's planar fragment textures
        RMPipeline *pipe = nil;
        for (size_t at = from; at < to; at++) {
            const Cmd &c = cb->_tape[at];
            // Metal's copies of a planar texture move its planes' bytes, not the image: m2v's
            if ((c.op == OpCopyTexture && (isPlanar(c.obj) || isPlanar(c.obj2))) ||
                (c.op == OpCopyBufferToTexture && isPlanar(c.obj2)) ||
                (c.op == OpCopyTextureToBuffer && isPlanar(c.obj)))
                return fail(PredEncPlanarCopy, "a copy to or from a planar texture", c.op);
            if (c.op == OpRender) {
                std::fill(std::begin(bound), std::end(bound), nil);
                pipe = nil;
                const RMPass &pass = cb->_passes[c.x];
                for (unsigned j = 0; j < 8; j++)
                    if (RMTexture *t = (RMTexture *)pass.color[j].texture; t && t->_raw.planarStandIn)
                        rendered.push_back(t);
            } else if (c.op == OpPipeline) {
                pipe = [(__bridge id)c.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)c.obj : nil;
            } else if (c.op == OpTexture && c.stage == 2 && c.index < 16) {
                bound[c.index] = isPlanar(c.obj) ? (__bridge RMTexture *)c.obj : nil;
            } else if ((c.op == OpDraw || c.op == OpDrawIndexed) && pipe && pipe->_raw) {
                const raw::PackPipeline &r = *pipe->_raw->rec;
                for (uint32_t e = 0; e < r.entries; e++) {
                    RMTexture *t =
                        r.entry[e].kind == raw::EntryTexture && r.entry[e].slot < 16 ? bound[r.entry[e].slot] : nil;
                    if (t && std::find(planar.begin(), planar.end(), t) == planar.end())
                        planar.push_back(t);
                }
            } else if (c.op == OpSampler && c.obj) {
                RMSampler *sm = (__bridge RMSampler *)c.obj;
                if (std::find(samplers.begin(), samplers.end(), sm) == samplers.end())
                    samplers.push_back(sm);
            }
        }
        // its image is converted once, before the passes: one that a pass here also stores into is m2v's
        for (RMTexture *t : rendered)
            if (std::find(planar.begin(), planar.end(), t) != planar.end())
                return fail(PredEncPlanarRT, "a planar texture sampled and rendered in one command buffer",
                            t->_raw.width, t->_raw.height);
        if (!planar.empty()) {
            // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
            for (RMTexture *t : planar)
                for (RMSampler *sm : samplers)
                    if (t->_raw.addressMap && !d.samplerVariant(sm->_raw, t->_raw.addressMap))
                        return fail(PredEncPlanarClamp, "no sampler header for an address map");
            // an image converted already from its surface at the surface's seed now is not converted again: the work that
            // converted it went before on this channel (the decoder and ImageIO write a surface under a write lock, which
            // moves its seed). The new seeds take effect once this command buffer is submitted.
            std::vector<RMTexture *> stale;
            for (RMTexture *t : planar) {
                const uint32_t seed = t->_surface ? IOSurfaceGetSeed(t->_surface) : 0;
                const uint64_t writes = t->_surface ? surfaceWrites(t->_surface) : 0;
                if (!t->_surface || !t->_imageCurrent || t->_imageSeed != seed || t->_imageWrites != writes) {
                    stale.push_back(t);
                    converted.push_back({t, seed, writes});
#if SWITCH_FRAME_TIMELINE
                    traceSurfaceImage(t, "converted", seed);
#endif
                }
            }
            planar.swap(stale);
            // 4:4:4 code textures: two copy-engine remaps each
            bool copies = false;
            for (RMTexture *t : planar)
                copies |= t->_raw.planar;
            if (copies) {
                raw::BlitEncoder *conv = rc->blit();
                for (RMTexture *t : planar)
                    if (t->_raw.planar)
                        conv->convertPlanar(t->_raw);
                conv->end();
            }
            // YCbCr textures' planes whose rows or offset are 16 bytes apart but not 32 (ImageIO's photo thumbnails, a width of
            // 360: rows of 368) are sampled through pool images (raw::Texture::sampled, createSurfaceTexture): filled from the
            // planes by the copy engine first, as refresh() fills a sampled surface's (r7u: without it the conversion read
            // whatever the pool held, magenta and green stripes of older images)
            raw::BlitEncoder *fill = nullptr;
            for (RMTexture *t : planar)
                if (t->_raw.ycbcr)
                    for (const raw::Texture *x : {t->_raw.luma, t->_raw.chroma})
                        if (x && x->sampled) {
                            if (!fill)
                                fill = rc->blit();
                            fill->copyTexels(*x, 0, 0, *x->sampled, 0, 0, x->width, x->height);
                        }
            if (fill)
                fill->end();
            // YCbCr textures: a pass of the pack's conversion state each, into the image the draws sample
            for (RMTexture *t : planar) {
                raw::Texture &x = t->_raw;
                if (!x.ycbcr)
                    continue;
                raw::PassDesc p;
                p.color[0].texture = &x;
                p.color[0].load = raw::ColorAttachment::DontCare;
                p.color[0].store = true;
                p.count = 1;
                raw::RenderEncoder *conv = rc->render(p);
                if (!conv)
                    return fail(PredEncPassRender, "a YCbCr texture's image the raw path cannot render");
                conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
                conv->setBytes(2, 0, x.shape, sizeof x.shape);
                conv->setTexture(0, x.luma);
                conv->setTexture(1, x.chroma ? x.chroma : x.luma);  // (over plane 1: chroma codes 0, none read)
                conv->setViewport(0, 0, x.width, x.height, 0, 1);
                conv->setScissor(0, 0, x.width, x.height);
                if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                    return refused();
                conv->end();
            }
        }
    }
    // textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
    // copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
    // its shadow at each encoder that uses it once written since)
    std::set<RMTexture *> fresh;
    raw::Texture levelTargets[8];  // the open pass's level targets: the encoder reads them until its end()
    uint64_t levelPasses = 0;
    static std::atomic<bool> loggedLevel{false};
    auto refresh = [&](size_t pass) {
        for (size_t at = pass + 1; at < to && cb->_tape[at].op != OpEnd; at++) {
            const Cmd &c = cb->_tape[at];
            if (c.op != OpTexture || !c.obj)
                continue;
            RMTexture *t = (__bridge RMTexture *)c.obj;
            if (!t->_raw.sampled || !fresh.insert(t).second)
                continue;
            const raw::Texture &x = t->_raw;
            raw::BlitEncoder *b = rc->blit();
            b->copyTexels(x, 0, 0, *x.sampled, 0, 0, x.width, x.height);
            b->end();
        }
    };
    // extended-range textures (raw_xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
    // before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
    // orders the conversion between the codes' writes and the samples.
    std::set<RMTexture *> standInFresh;
    std::deque<raw::Texture>
        codes;  // the conversions' sources: the codes' headers (the encoder reads them until its end)
    auto standIns = [&](size_t pass) {
        RMTexture *bound[16] = {};
        RMPipeline *pipe = nil;
        std::vector<RMTexture *> stale;
        for (size_t k = pass + 1; k < to && cb->_tape[k].op != OpEnd; k++) {
            const Cmd &u = cb->_tape[k];
            if (u.op == OpPipeline) {
                pipe = [(__bridge id)u.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)u.obj : nil;
            } else if (u.op == OpTexture && u.stage == 2 && u.index < 16) {
                RMTexture *t = (__bridge RMTexture *)u.obj;
                bound[u.index] = t && t->_raw.standIn ? t : nil;
            } else if ((u.op == OpDraw || u.op == OpDrawIndexed) && pipe && pipe->_raw) {
                const raw::PackPipeline &r = *pipe->_raw->rec;
                for (uint32_t e = 0; e < r.entries; e++) {
                    RMTexture *t =
                        r.entry[e].kind == raw::EntryTexture && r.entry[e].slot < 16 ? bound[r.entry[e].slot] : nil;
                    if (t && !standInFresh.count(t) && std::find(stale.begin(), stale.end(), t) == stale.end())
                        stale.push_back(t);
                }
            }
        }
        for (RMTexture *t : stale) {
            raw::Texture &x = t->_raw;
            raw::PassDesc p;
            p.color[0].texture = x.standIn;
            p.color[0].load = raw::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            raw::RenderEncoder *conv = rc->render(p);
            if (!conv)
                return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
            codes.push_back(x);
            codes.back().standIn = nullptr;
            useTexture(rc, t, false);
            conv->setPipeline(d.xrPipeline(raw::xrForm(x.format)));
            conv->setTexture(0, &codes.back());
            conv->setViewport(0, 0, x.width, x.height, 0, 1);
            conv->setScissor(0, 0, x.width, x.height);
            if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            conv->end();
            standInFresh.insert(t);
        }
        return true;
    };
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender: {
            const RMPass &pass = cb->_passes[c.x];
            if (cb->_sampledShadows)
                refresh(at);
            if (cb->_standIns && !standIns(at))
                return false;
            raw::PassDesc p;
            for (unsigned j = 0; j < 8; j++) {
                const RMPass::Color &a = pass.color[j];
                RMTexture *t = (RMTexture *)a.texture;
                if (!t)
                    continue;
                if (t->_raw.planar && !t->_raw.planarStandIn)
                    return fail(PredEncPlanarRT, "a planar texture as a render target", j);
                useTexture(rc, t, true);
                if (t->_raw.planarStandIn) {
                    // Metal's DontCare load keeps the planes' codes where no draw writes, and the store writes every texel
                    // (raw/test/standinoracle.mm): such a pass loads them too
                    const bool stores = a.store != MTLStoreActionDontCare;
                    if (a.load == MTLLoadActionLoad || (a.load == MTLLoadActionDontCare && stores))
                        standInLoads.push_back(t);
                    if (stores)
                        standInStores.push_back(t);
                }
                if (!fresh.empty())
                    fresh.erase(t);  // this pass writes it
                standInFresh.erase(t);
                if (t->_raw.chain && a.level) {
                    if (!raw::chainLevelTarget(t->_raw, uint32_t(a.level), levelTargets[j]))
                        return fail(PredEncPassRender, "a mip level the raw path cannot render to", a.level);
                    levelTargets[j].tic = gRM->raw->levelTic(t->_raw, uint32_t(a.level));
                    levelPasses++;
                    if (!loggedLevel.exchange(true))
                        os_log(rawLog(), "raw: renders into mip levels (first: level %lu of %ux%u format %u)",
                               (unsigned long)a.level, t->_raw.width, t->_raw.height, t->_raw.format);
                }
                p.color[j].texture = a.level                 ? &levelTargets[j]
                                     : t->_raw.planarStandIn ? t->_raw.planarStandIn
                                                             : &t->_raw;
                p.color[j].load = a.load == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                                  : a.load == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                                : raw::ColorAttachment::DontCare;
                p.color[j].store =
                    a.store == MTLStoreActionStore || a.store == MTLStoreActionStoreAndMultisampleResolve;
                const MTLClearColor k = a.clear;
                p.color[j].clear[0] = float(k.red);
                p.color[j].clear[1] = float(k.green);
                p.color[j].clear[2] = float(k.blue);
                p.color[j].clear[3] = float(k.alpha);
                p.count = j + 1;
            }
            if (RMTexture *z = (RMTexture *)pass.depth) {  // (rawDepthStencil: decided at record)
                p.depthStencil.texture = &z->_raw;
                p.depthStencil.clearDepth = pass.depthLoad == MTLLoadActionClear;
                p.depthStencil.clearStencil = pass.stencilLoad == MTLLoadActionClear;
                p.depthStencil.depth = float(std::clamp(pass.clearDepth, 0.0, 1.0));
                p.depthStencil.stencil = pass.clearStencil;
            }
            p.width = uint32_t(std::min<NSUInteger>(pass.width, UINT32_MAX));
            p.height = uint32_t(std::min<NSUInteger>(pass.height, UINT32_MAX));
            for (RMTexture *t : standInLoads)
                if (standInPass(t, false))
                    return false;
            standInLoads.clear();
            if (!(re = rc->render(p)))
                return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
            break;
        }
        case OpBlit:
            be = rc->blit();
            break;
        case OpEnd:
            if (re)
                re->end();
            if (be)
                be->end();
            for (RMTexture *t : standInStores)
                if (standInPass(t, true))
                    return false;
            standInStores.clear();
            re = nullptr;
            be = nullptr;
            break;
        case OpPipeline:
            re->setPipeline(((__bridge RMPipeline *)c.obj)->_raw);
            break;
        case OpBlendColor: {
            const float rgba[4] = {extraFloat(cb, c.x), extraFloat(cb, c.x + 1), extraFloat(cb, c.x + 2),
                                   extraFloat(cb, c.x + 3)};
            re->setBlendColor(rgba);
            break;
        }
        case OpBytes: {
            const raw::Buffer &k = cb->_bytes.chunks[c.c];
            if (!k.wc)
                rc->clean(k.cpu + c.a, std::max<uint32_t>((c.b + 255) & ~255u, 256));
            re->setBytesAt(c.stage, c.index, k.va + c.a, c.b, k.cpu + c.a);
            break;
        }
        case OpDepthStencil:
            re->setDepthStencil(c.obj ? &((__bridge RMDepthStencil *)c.obj)->_raw : nullptr);
            break;
        case OpStencilRef:
            re->setStencilReference(c.a, c.b);
            break;
        case OpBuffer:
            useBuffer(rc, (__bridge RMBuffer *)c.obj, false);
            re->setBuffer(c.stage, c.index, c.obj ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr, c.c);
            break;
        case OpBufferOffset:
            re->setBufferOffset(c.stage, c.index, c.c);
            break;
        case OpTexture:
            if (c.stage == 2) {
                useTexture(rc, (__bridge RMTexture *)c.obj, false);
                re->setTexture(c.index, c.obj ? &((__bridge RMTexture *)c.obj)->_raw : nullptr);
            }
            break;
        case OpSampler:
            if (c.stage == 2)
                re->setSampler(c.index, c.obj ? &((__bridge RMSampler *)c.obj)->_raw : nullptr);
            break;
        case OpScissor:
            re->setScissor(c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c));
            break;
        case OpCull:
            re->setCullMode(uint8_t(c.a));
            break;
        case OpWinding:
            re->setWinding(uint8_t(c.a));
            break;
        case OpFillMode:
            re->setFillMode(uint8_t(c.a));
            break;
        case OpDepthBias:
            re->setDepthBias(extraFloat(cb, c.x), extraFloat(cb, c.x + 1), extraFloat(cb, c.x + 2));
            break;
        case OpViewport:
            re->setViewport(extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                            extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4), extraDouble(cb, c.x + 5));
            break;
        case OpDraw:
            if (!re->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why))
                return refused();
            break;
        case OpBarrier:
            re->barrier();
            break;
        case OpDrawIndexed:
            useBuffer(rc, (__bridge RMBuffer *)c.obj, false);
            if (!re->drawIndexed(c.stage, c.a, c.b == MTLIndexTypeUInt16 ? 2 : 4, &((__bridge RMBuffer *)c.obj)->_raw,
                                 c.c, uint32_t(extra(cb, c.x)), int32_t(extra(cb, c.x + 1)),
                                 uint32_t(extra(cb, c.x + 2)), why))
                return refused();
            break;
        case OpCopyTexture: {
            RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1), dl = extra(cb, x + 9);
            const uint32_t sx = uint32_t(extra(cb, x + 2)), sy = uint32_t(extra(cb, x + 3)),
                           w = uint32_t(extra(cb, x + 5)), h = uint32_t(extra(cb, x + 6)), dx = uint32_t(o),
                           dy = uint32_t(o >> 32);
            raw::Texture fromLevel, toLevel;
            if (sl)
                raw::chainLevelImage(s->_raw, uint32_t(sl), fromLevel);
            if (dl)
                raw::chainLevelImage(t->_raw, uint32_t(dl), toLevel);
            be->copyTexels(sl ? fromLevel : s->_raw, sx, sy, dl ? toLevel : t->_raw, dx, dy, w, h);
            break;
        }
        case OpMipmaps: {  // a chain's levels from level 0 (decided at record)
            RMTexture *t = (__bridge RMTexture *)c.obj;
            useTexture(rc, t, true);
            be->generate(t->_raw);
            break;
        }
        case OpCopyBuffer: {
            RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
            useBuffer(rc, s, false);
            useBuffer(rc, t, true);
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
            const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1);
            const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4), dl = extra(cb, x + 7);
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            raw::Texture level;
            if (dl)
                raw::chainLevelImage(t->_raw, uint32_t(dl), level);
            BufferRows rows;
            bufferRows(so, row, uint32_t(h), uint32_t(w * t->_raw.bpp), t->_raw.bpp, true,
                       rows);  // (decided at record)
            const uint64_t first = bufferRow(rows, so, row, 0);
            if (!s->_raw.wc)
                rc->clean(s->_raw.cpu + first, bufferRow(rows, so, row, uint32_t(h - 1)) - first + w * t->_raw.bpp);
            copyRows(be, s->_raw.va, rows, so, row, dl ? level : t->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(w),
                     uint32_t(h), true);
            break;
        }
        case OpCopyTextureToBuffer: {
            RMTexture *s = (__bridge RMTexture *)c.obj;
            RMBuffer *t = (__bridge RMBuffer *)c.obj2;
            useTexture(rc, s, false);
            useBuffer(rc, t, true);
            const uint64_t x = c.x, o = extra(cb, x + 2), w = extra(cb, x + 4), h = extra(cb, x + 5),
                           dof = extra(cb, x + 7), row = extra(cb, x + 8), sl = extra(cb, x + 1);
            raw::Texture level;
            if (sl)
                raw::chainLevelImage(s->_raw, uint32_t(sl), level);
            BufferRows rows;
            bufferRows(dof, row, uint32_t(h), uint32_t(w * s->_raw.bpp), s->_raw.bpp, false,
                       rows);  // (decided at record)
            const uint64_t first = bufferRow(rows, dof, row, 0);
            if (!t->_raw.wc)
                rc->clean(t->_raw.cpu + first, bufferRow(rows, dof, row, uint32_t(h - 1)) - first + w * s->_raw.bpp,
                          true);
            copyRows(be, t->_raw.va, rows, dof, row, sl ? level : s->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(w),
                     uint32_t(h), false);
            break;
        }
        default:  // states, fences, events, resources the raw path needs nothing for
            break;
        }
    }
    if (re)
        re->end();
    if (be)
        be->end();
    for (RMTexture *t : standInStores)
        if (standInPass(t, true))
            return false;
    uint32_t seq = 0;
    if (!rc->commit(seq)) {
        cb->_partial = rc->partial;
        return fail(PredEncKernel, "the kernel refused the submission");
    }
    for (const Conversion &c : converted) {  // (submitted: their images hold the surfaces at these seeds)
        c.t->_imageSeed = c.seed;
        c.t->_imageWrites = c.writes;
        c.t->_imageCurrent = true;
    }
    noteSurfaceWrites(cb, from, to);
    for (RMTexture *t : planesWritten)  // a write the surface's seed does not show: its image is converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    d.stats.levelPasses += levelPasses;
    gRM->stats.draws += rc->draws;
    gRM->stats.passes += rc->passes;
    gRM->stats.copies += rc->blits;
    return true;
}
