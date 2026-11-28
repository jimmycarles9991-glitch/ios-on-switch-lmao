// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPipelineVariants.h"
#include "fragment_tests.h"
#include "render_variant.h"
#include "shader_test_push.h"

static void cubicBinds(__unsafe_unretained SwitchMetalRenderEncoder *encoder, const gm20b::Pipeline &pl,
                       gm20b::CubicBinds &b)
{
    SwitchMetalRenderSlots &slots = encoder->_cb->_renderSlots;
    b.samplers.clear();
    b.textures.clear();
    encoder->_cubicTextures[0] = {};
    encoder->_cubicTextures[1] = {};
    encoder->_formSamplers[0] = encoder->_formSamplers[1] = 0;
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const gm20b::PackEntry &e = pl.entries[i];
        if ((e.kind != gm20b::EntryTexture && e.kind != gm20b::EntrySamplerOnly) ||
            e.samplerKind != gm20b::SamplerSlot || (e.stage != 1 && e.stage != 2) || e.sampler >= 16)
            continue;
        encoder->_formSamplers[e.stage - 1] |= uint16_t(1u << e.sampler);
        __unsafe_unretained SwitchMetalSampler *s = slots.samp[e.stage - 1][e.sampler];
        if (!s || !s->_raw.desc.cubic)
            continue;
        const gm20b::SamplerDesc &d = s->_raw.desc;
        if (std::none_of(b.samplers.begin(), b.samplers.end(), [&](const auto &c) {
                return c.slot == e.sampler && c.stage == e.stage;
            }))
            b.samplers.push_back({e.sampler, d.sAddress, d.tAddress, d.rAddress,
                                  uint8_t(s->_descriptor.borderColor & 3), !d.pixel, e.stage});
        if (e.kind != gm20b::EntryTexture || e.slot >= gm20b::kStageTextures)
            continue;
        gm20b::addSlot(encoder->_cubicTextures[e.stage - 1], e.slot);
        __unsafe_unretained SwitchMetalTexture *t = slots.tex[e.stage - 1][e.slot];
        if (t && std::none_of(b.textures.begin(), b.textures.end(), [&](const auto &c) {
                return c.slot == e.slot && c.stage == e.stage;
            }))
            b.textures.push_back({e.slot, uint32_t(t->_descriptor.pixelFormat), e.stage});
    }
    std::sort(b.samplers.begin(), b.samplers.end(), [](const auto &x, const auto &y) {
        return x.stage != y.stage ? x.stage > y.stage : x.slot < y.slot;
    });
    std::sort(b.textures.begin(), b.textures.end(), [](const auto &x, const auto &y) {
        return x.stage != y.stage ? x.stage > y.stage : x.slot < y.slot;
    });
}

void packedVariants(SwitchMetalPipeline *p, const gm20b::PackRun &run)
{
    // p is unshared until gm20bPipelineShell returns it; a variant is placed when a draw first selects its name.
    p->_packRun = run;
    p->_hasPackVariants = false;
    for (uint32_t i = 0; i < run.records; ++i) {
        if (run.first[i].flags & gm20b::PipelineVariantRecord) {
            p->_hasPackVariants = true;
            break;
        }
    }
}

// A draw's first select of a name places a tier-0 record there and queues any other to the placer
// (TegraGM20BMetalPipelineVariants.mm). Sampled variants can use stand-ins while pending; other variants hold the queue head until they publish or fail (TegraGM20BMetalSchedule.mm
// variantReadiness). A variant failed for want of a compiler is asked again by the next pass that needs it (reopen).
static gm20b::Variant<gm20b::Pipeline> *renderVariant(SwitchMetalPipeline *p, const gm20b::RenderVariant &forms)
{
    std::shared_ptr<gm20b::Variant<gm20b::Pipeline>> variant;
    {
        std::lock_guard<gm20b::UnfairMutex> lock(p->_variantLock);
        auto [at, inserted] = p->_variants.try_emplace(forms.name());
        if (inserted)
            at->second = std::make_shared<gm20b::Variant<gm20b::Pipeline>>();
        else if (!at->second->reopen())
            return at->second.get();
        variant = at->second;
    }
    gm20bPlaceVariant(p, variant, forms);
    return variant.get();
}

static void awaitSettled(const std::vector<gm20b::Variant<gm20b::Pipeline> *> &variants)
{
    if (variants.empty())
        return;

    struct State {
        gm20b::ChangeWord changed;
        std::atomic<uint32_t> waitGen{1};
    };

    auto shared = std::make_shared<State>();
    for (gm20b::Variant<gm20b::Pipeline> *variant : variants) {
        if (variant->observe([shared] {
                shared->changed.changed();
            }) == gm20b::Variant<gm20b::Pipeline>::State::Pending) {
            variant->observeWait([shared] {
                shared->waitGen.fetch_add(1, std::memory_order_acq_rel);
                shared->changed.changed();
            });
        }
    }
    const auto allSettled = [&] {
        for (gm20b::Variant<gm20b::Pipeline> *variant : variants)
            if (variant->state() == gm20b::Variant<gm20b::Pipeline>::State::Pending)
                return false;
        return true;
    };
    uint32_t triedGen = 0;
    while (!allSettled()) {
        const uint32_t gen = shared->waitGen.load(std::memory_order_acquire);
        if (gen != triedGen) {
            bool retry = false;
            for (gm20b::Variant<gm20b::Pipeline> *variant : variants) {
                if (variant->state() == gm20b::Variant<gm20b::Pipeline>::State::Pending && variant->waitPending()) {
                    retry = true;
                    break;
                }
            }
            if (!retry)
                triedGen = gen;
            continue;
        }
        shared->changed.waitOnce([&] {
            return allSettled() || shared->waitGen.load(std::memory_order_acquire) != triedGen;
        });
    }
}

// The test classes drawnTests (shader_test_push.h) can give a draw of `pl` in a pass of the creation's depth and
// stencil formats, whatever its depth/stencil state
static std::vector<uint8_t> testClasses(const gm20b::Pipeline &pl, uint32_t depth, uint32_t stencil)
{
    using gm20b::FragmentTests;
    const bool hasDepth = gm20b::zetaHasDepth(depth) || gm20b::zetaHasDepth(stencil);
    const bool hasStencil = gm20b::zetaHasStencil(gm20b::testedZeta(depth)) || gm20b::zetaHasStencil(stencil);
    if (pl.fetchBits && pl.depthFormat && !pl.storageBits) {
        std::vector<uint8_t> classes = {uint8_t(FragmentTests::Pass), uint8_t(FragmentTests::ReadOnly)};
        if (hasDepth)
            classes.push_back(uint8_t(FragmentTests::DepthWrite));
        if (hasStencil)
            classes.push_back(uint8_t(FragmentTests::StencilWrite));
        return classes;
    }
    if (gm20b::shadowStencil(pl.rec->shaderTests) && hasDepth)
        return {uint8_t(FragmentTests::StencilWrite)};
    return {};
}

// Every test class a draw can take: a packed state's are the pack's (directpack.mm makeState, which leaves out a
// shader-side stencil's). Asked together and waited for as the state's own record was, so no draw of them waits on a
// compile at its queue's head.
void gm20bMadeVariants(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d)
{
    const std::vector<uint8_t> classes =
        testClasses(*p->_raw, uint32_t(d.depthAttachmentPixelFormat), uint32_t(d.stencilAttachmentPixelFormat));
    std::vector<gm20b::Variant<gm20b::Pipeline> *> made;
    gm20b::RenderVariant forms;
    for (const uint8_t tests : classes) {
        forms.fragmentTests = tests;
        made.push_back(renderVariant(p, forms));
    }
    awaitSettled(made);
}

// The textures a draw of `pl` writes in other formats than its record's programs store (written_textures.h)
static void reboundTextures(const gm20b::Pipeline &pl, SwitchMetalTexture *const textures[gm20b::kStageTextures],
                            std::vector<gm20b::WrittenTexture> &out)
{
    out.clear();
    for (uint32_t slots = pl.writtenSlots; slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        const uint32_t format = textures[slot] ? uint32_t(textures[slot]->_descriptor.pixelFormat) : 0u;
        if (format && format != pl.rec->writtenFormat[slot])
            out.push_back({uint8_t(slot), format});
    }
}

// The slots of each stage (vertex, fragment: gm20b::handleStage) a draw of `pl` samples a two-plane RGB10A8_2P_XR10
// texture at (xr_pass.h), or (`srgb`) a two-plane RGB10A8_2P_XR10_sRGB one, which no variant reads in its planes: its
// sRGB codes are read through its stand-in alone (xr.h xrConversionOf)
static std::array<gm20b::StageTextureMask, 2>
xrSampledSlots(const gm20b::Pipeline &pl, SwitchMetalTexture *const textures[2][gm20b::kStageTextures],
               bool srgb = false)
{
    std::array<gm20b::StageTextureMask, 2> slots{};
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const gm20b::PackEntry &e = pl.entries[i];
        __unsafe_unretained SwitchMetalTexture *t = e.kind == gm20b::EntryTexture && e.slot < gm20b::kStageTextures
                                                        ? textures[gm20b::handleStage(e)][e.slot]
                                                        : nil;
        if (t && t->_raw.xrPlanes && !t->_raw.memoryless && (t->_raw.format == gm20b::kRgb10A8XrSrgb) == srgb)
            gm20b::addSlot(slots[gm20b::handleStage(e)], e.slot);
    }
    return slots;
}

static bool sameForms(const gm20b::RenderVariant &a, const gm20b::RenderVariant &b)
{
    const auto sameSampler = [](const gm20b::CubicBinds::Sampler &x, const gm20b::CubicBinds::Sampler &y) {
        return x.slot == y.slot && x.s == y.s && x.t == y.t && x.r == y.r && x.border == y.border &&
               x.normalized == y.normalized && x.stage == y.stage;
    };
    const auto sameTexture = [](const gm20b::CubicBinds::Texture &x, const gm20b::CubicBinds::Texture &y) {
        return x.slot == y.slot && x.format == y.format && x.stage == y.stage;
    };
    return a.fragmentTests == b.fragmentTests && a.xrSlots == b.xrSlots && a.written == b.written &&
           std::equal(a.cubic.samplers.begin(), a.cubic.samplers.end(), b.cubic.samplers.begin(),
                      b.cubic.samplers.end(), sameSampler) &&
           std::equal(a.cubic.textures.begin(), a.cubic.textures.end(), b.cubic.textures.begin(),
                      b.cubic.textures.end(), sameTexture);
}

// The state's variant for `forms` as this encoder found it before, else from the state's map. The encoder's command
// buffer retains each state it set, and a state's map never drops a variant, so what it found holds for its pass.
static gm20b::Variant<gm20b::Pipeline> *preparedVariant(__unsafe_unretained SwitchMetalRenderEncoder *e,
                                                        const gm20b::RenderVariant &forms)
{
    SwitchMetalRenderSlots &slots = e->_cb->_renderSlots;
    if (slots.preparedUsed)
        for (const SwitchMetalPreparedVariant &prepared : slots.prepared)
            if (prepared.pipe == e->_pipe && sameForms(prepared.forms, forms))
                return prepared.variant;
    slots.preparedUsed = true;
    SwitchMetalPreparedVariant &slot = slots.prepared[slots.preparedNext++ % slots.prepared.size()];
    slot.pipe = e->_pipe;
    slot.forms = forms;
    slot.variant = renderVariant(e->_pipe, forms);
    return slot.variant;
}

@implementation SwitchMetalRenderEncoder (Cubic)

- (void)prepareDrawnRecord
{
    SwitchMetalRenderSlots &slots = _cb->_renderSlots;
    gm20b::RenderVariant &forms = slots.forms;
    gm20b::Pipeline *base = !_cb->_encFailed && _pipe ? _pipe->_raw : nullptr;
    _formVariant = _sampledVariant = nullptr;
    _sampledSlots = {};
    if (!base || !(_cubicMask[0] | _cubicMask[1])) {
        forms.cubic.samplers.clear();
        forms.cubic.textures.clear();
        _cubicTextures[0] = {};
        _cubicTextures[1] = {};
    } else {
        cubicBinds(self, *base, forms.cubic);
    }
    forms.written.clear();
    forms.fragmentTests = 0;
    forms.xrSlots = 0;
    if (!base) {
        _formSamplers[0] = _formSamplers[1] = 0;
        _hasForms = false;
        return;
    }
    SwitchMetalPass &pass = _cb->_passes.own(_passAt);
    // the pass's depth/stencil attachment the draw is tested against: its own, or the hidden one a pass of none is given
    // where a state set in it tests depth or stencil (depth_stencil_state.h testsHiddenZeta)
    const uint32_t zeta = _zeta ? _zeta : pass.testsHiddenZeta ? gm20b::kZetaFormat : 0;
    forms.fragmentTests = gm20b::drawnTests(base->fetchBits && base->depthFormat && !base->storageBits,
                                            base->rec->shaderTests, _ds, gm20b::testedZeta(zeta));
    // a Depth16Unorm a draw tests in its shader renders converted (TegraGM20BMetalRangeForms.mm depth16Tested)
    if ((forms.fragmentTests && gm20b::zetaHasDepth(zeta)) || (base->rec->shaderTests.flags & gm20b::kShaderTestDepth))
        pass.shaderTestsDepth = true;
    if (forms.fragmentTests == uint8_t(gm20b::FragmentTests::StencilWrite) ||
        (base->rec->shaderTests.flags & gm20b::kShaderTestStencil))
        pass.testsStencil = true;
    if (base->writtenSlots)
        reboundTextures(*base, slots.tex[1], forms.written);
    if (!forms.cubic.empty() || forms.fragmentTests || !forms.written.empty())
        _formVariant = preparedVariant(self, forms);
    if (pass.xrPlaneBinds)
        _sampledSlots = xrSampledSlots(*base, slots.tex);
    forms.xrSlots = gm20b::xrPairSlots(_sampledSlots[1]);
    if (forms.xrSlots)
        _sampledVariant = preparedVariant(self, forms);
    _hasForms = _formVariant != nullptr || _sampledVariant != nullptr || !forms.cubic.empty() ||
                forms.fragmentTests != 0 || !forms.written.empty() || forms.xrSlots != 0 ||
                gm20b::anySlot(_sampledSlots[0]) || gm20b::anySlot(_sampledSlots[1]);
}

- (const gm20b::Pipeline *)drawnRecord
{
    SwitchMetalRenderSlots &slots = _cb->_renderSlots;
    gm20b::Pipeline *base = _pipe->_raw;
    auto *variant = _formVariant;
    std::array<gm20b::StageTextureMask, 2> standIns = {_sampledSlots[0], gm20b::pastXrPairSlots(_sampledSlots[1])};
    if (_cb->_passes.own(_passAt).xrPlaneBinds) {
        const std::array<gm20b::StageTextureMask, 2> srgb = xrSampledSlots(*base, slots.tex, true);
        for (uint32_t stage = 0; stage < 2; stage++)
            gm20b::forEachSlot(srgb[stage], [&](uint32_t slot) {
                gm20b::addSlot(standIns[stage], slot);
            });
    }
    if (_sampledVariant) {
        if (_sampledVariant->state() == gm20b::Variant<gm20b::Pipeline>::State::Ready) {
            variant = _sampledVariant;
        } else {
            standIns[1] = _sampledSlots[1];
        }
    }
    if (gm20b::anySlot(standIns[0]) || gm20b::anySlot(standIns[1])) {
        gm20bPipelineStop(_cb, VetoShadows);
        for (uint32_t stage = 0; stage < 2; stage++)
            gm20b::forEachSlot(standIns[stage], [&](uint32_t slot) {
                _cb->_xrStandIns.insert(slots.tex[stage][slot]);
            });
    }
    if (base != _drawn || variant != _drawnVariant) {
        Cmd state = cmd(OpPipeline);
        state.obj = (__bridge void *)_pipe;
        state.c = variant ? 0 : uintptr_t(base);
        state.obj2 = variant;
        [_cb record:state];
        _drawn = base;
        _drawnVariant = variant;
    }
    return base;
}

@end
