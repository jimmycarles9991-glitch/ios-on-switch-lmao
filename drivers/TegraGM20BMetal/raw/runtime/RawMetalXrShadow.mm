// SPDX-License-Identifier: MIT
// Extended-range colours' half shadows (raw_xr_shadow.h): which colours a pass holds them of, decided from its draws when its
// encoder ends; the shadowed variants its draws take, which the queue head waits for; the shadows filled before the pass.
#import "RawMetalInternal.h"
#include "raw_xr_shadow.h"

void rmXrShadowBegin(RMCommandBuffer *cb, RMRenderEncoder *e, const RMPass &facts)
{
    RMXrShadowOpen &o = cb->_xrShadowOpen;
    o.open = false;
    o.pass = {};
    o.draws.clear();
    // a parallel encoder's sub-encoders record apart from the command buffer that runs them
    if (cb->_commandLog || facts.layers > 1 || e->_samples > 1 || e->_turn)
        return;
    for (uint32_t c = 0; c < 8; c++) {
        const RMPass::Color &a = facts.color[c];
        __unsafe_unretained RMTexture *x = (RMTexture *)a.texture;
        if (!x || ![(id)x isKindOfClass:[RMTexture class]] || !x->_rawOk || (facts.compressed >> c & 1) ||
            e->_formats[c] != x->_raw.format || x->_raw.planarStandIn ||
            (x->_raw.memoryless && a.load != MTLLoadActionClear))
            continue;
        const raw::XrForm form = raw::xrShadowForm(x->_raw.format, e->_xrPlanes);
        const char *tile = raw::xrTileState(form);
        if (form == raw::XrForm::None ||
            (a.load != MTLLoadActionClear && !gRM->raw->pipeline(raw::Pack::fnv(tile, strlen(tile)))))
            continue;
        o.pass.eligible |= uint8_t(1u << c);
        const double clear[4] = {a.clear.red, a.clear.green, a.clear.blue, a.clear.alpha};
        if (a.load == MTLLoadActionClear && !raw::xrClearHeldByCodes(form, clear))
            o.pass.clearedOff |= uint8_t(1u << c);
    }
    o.open = o.pass.eligible != 0;
    o.passAt = e->_passAt;
    o.renderAt = e->_renderAt;
}

static uint8_t readsOf(const raw::Pipeline &pl)
{
    return pl.storageBits | pl.fetchBits | pl.readBits;
}

static uint8_t writesOf(const raw::Pipeline &pl)
{
    return pl.writeBits | pl.storageWriteBits;
}

// Each draw, in order (checkDraw's every call, before its cache of the state's checks)
void rmXrShadowDrew(RMCommandBuffer *cb, const raw::Pipeline &base)
{
    if (cb->_xrShadowOpen.open)
        cb->_xrShadowOpen.pass.draw(readsOf(base), writesOf(base));
}

// A state and variant the pass's draws take (drawnRecord, at each change of them)
void rmXrShadowDraw(RMCommandBuffer *cb, RMPipeline *pipe, const raw::Pipeline &base,
                    raw::Variant<raw::Pipeline> *variant, const raw::CubicBinds &binds, uint8_t tests, bool planes,
                    uint16_t xrSlots, const std::vector<raw::WrittenTexture> &written)
{
    RMXrShadowOpen &o = cb->_xrShadowOpen;
    if (!o.open)
        return;
    const uint8_t touched = (readsOf(base) | writesOf(base)) & o.pass.eligible;
    if (!touched)
        return;
    for (const RMXrShadowDraw &d : o.draws)
        if (d.pipe == pipe && d.base == variant)
            return;
    o.draws.push_back({pipe, variant, binds, tests, planes, xrSlots, written, touched});
}

void rmXrShadowBlock(RMCommandBuffer *cb)
{
    cb->_xrShadowOpen.pass.blocked = true;
}

// The shadowed variant of each state that touches a held colour, asked now so the queue head finds it made
void rmXrShadowEnd(RMCommandBuffer *cb)
{
    RMXrShadowOpen &o = cb->_xrShadowOpen;
    if (!o.open)
        return;
    o.open = false;
    const uint8_t held = o.pass.held();
    if (!held) {
        o.draws.clear();
        return;
    }
    RMXrShadowTwins twins;
    twins.renderAt = o.renderAt;
    twins.passAt = o.passAt;
    for (const RMXrShadowDraw &d : o.draws)
        if (const uint8_t mask = d.touched & held)
            twins.twins.push_back({d.base ? static_cast<const void *>(d.base) : (__bridge const void *)d.pipe,
                                   rmRenderVariant(d.pipe, d.binds, d.tests, d.planes, d.xrSlots, d.written, mask)});
    o.draws.clear();
    cb->_passes.own(o.passAt).xrShadows = held;
    cb->_xrShadowPasses.push_back(std::move(twins));
    // the queue head may still drop these shadows (rmXrShadowReadiness): only an encode after the commit can follow
    rmPipelineVeto(cb, VetoShadows);
}

// A pass whose shadowed variants are not all made holds no shadow: its draws read the codes, as before
int rmXrShadowReadiness(RMCommandBuffer *cb, size_t from, size_t to)
{
    using Variant = raw::Variant<raw::Pipeline>;
    bool pending = false;
    for (RMXrShadowTwins &t : cb->_xrShadowPasses) {
        if (t.renderAt < from || t.renderAt >= to || t.twins.empty())
            continue;
        bool given = false;
        for (const RMXrShadowTwin &twin : t.twins) {
            std::function<void()> observer;
            if (cb->_variantObserved.insert(twin.shadowed).second) {
                void *queue = (__bridge void *)cb->_queue;
                observer = [queue] {
                    wakeSubmit(queue);
                };
            }
            const Variant::State state = twin.shadowed->observe(std::move(observer));
            pending |= state == Variant::State::Pending;
            given |= state == Variant::State::Refused || state == Variant::State::Failed;
        }
        if (given) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true, std::memory_order_relaxed))
                os_log_error(rawLog(),
                             "raw: a pass's extended-range colours %#x are drawn without their half shadow, from the "
                             "codes: a draw's state has no shadowed variant (once per process)",
                             unsigned(cb->_passes[t.passAt].xrShadows));
            cb->_passes[t.passAt].xrShadows = 0;
            t.twins.clear();
        }
    }
    return pending ? 0 : 1;
}

const RMXrShadowTwins *rmXrShadowTwinsOf(const RMCommandBuffer *cb, uint32_t passAt)
{
    if (passAt >= cb->_passes.size() || !cb->_passes[passAt].xrShadows)
        return nullptr;
    for (const RMXrShadowTwins &t : cb->_xrShadowPasses)
        if (t.passAt == passAt)
            return &t;
    return nullptr;
}

// Each held colour's shadow, set before the pass: a Clear's halves, else the tile of the attachment's codes as the pass
// begins (the pack's tile state)
bool rmXrShadowFill(raw::CommandBuffer *rc, const RMPass &pass, raw::PassDesc &p, const char **why)
{
    raw::Device &d = *gRM->raw;
    for (uint8_t held = pass.xrShadows; held; held &= uint8_t(held - 1)) {
        const uint32_t c = uint32_t(__builtin_ctz(held));
        const raw::Texture *target = p.color[c].texture;
        __unsafe_unretained RMTexture *t = (RMTexture *)pass.color[c].texture;
        const raw::XrForm form = raw::xrShadowForm(t->_raw.format, false);
        raw::Texture *shadow =
            target ? rc->turnScratch(raw::kXrStandInFormat, target->width, target->height, raw::kXrShadowTag + c)
                   : nullptr;
        if (!shadow) {
            *why = "no half shadow for an extended-range colour";
            return false;
        }
        raw::PassDesc q;
        q.color[0].texture = shadow;
        q.color[0].store = true;
        q.count = 1;
        const bool cleared = pass.color[c].load == MTLLoadActionClear;
        if (cleared) {
            const MTLClearColor k = pass.color[c].clear;
            const double color[4] = {k.red, k.green, k.blue, k.alpha};
            q.color[0].load = raw::ColorAttachment::Clear;
            raw::xrClearTile(form, color, q.color[0].clear);
        } else {
            q.color[0].load = raw::ColorAttachment::DontCare;
        }
        raw::Texture codes = *target;
        codes.standIn = nullptr;
        if (!cleared && codes.sampled && rc->readTexture(&t->_raw) == &t->_raw) {
            raw::BlitEncoder *b = rc->blit();
            b->copyTexels(codes, 0, 0, 0, *codes.sampled, 0, 0, 0, codes.width, codes.height);
            b->end();
        }
        raw::RenderEncoder *e = rc->render(q);
        if (!e) {
            *why = "an extended-range colour's half shadow the raw path cannot render";
            return false;
        }
        if (!cleared) {
            e->setPipeline(d.pipeline(raw::Pack::fnv(raw::xrTileState(form), strlen(raw::xrTileState(form)))));
            e->setTexture(0, &codes);
            e->setViewport(0, 0, shadow->width, shadow->height, 0, 1);
            e->setScissor(0, 0, shadow->width, shadow->height);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
        }
        e->end();
        p.xrShadow[c] = shadow;
    }
    return true;
}
