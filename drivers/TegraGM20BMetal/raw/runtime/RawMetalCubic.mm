// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_fragment_tests.h"
#include "raw_render_variant.h"
#include "raw_shader_test_push.h"

static void cubicBinds(__unsafe_unretained RMRenderEncoder *encoder, const raw::Pipeline &pl, raw::CubicBinds &b)
{
    b.samplers.clear();
    b.textures.clear();
    encoder->_cubicTextures[0] = {};
    encoder->_cubicTextures[1] = {};
    encoder->_formSamplers[0] = encoder->_formSamplers[1] = 0;
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const raw::PackEntry &e = pl.entries[i];
        if ((e.kind != raw::EntryTexture && e.kind != raw::EntrySamplerOnly) || e.samplerKind != raw::SamplerSlot ||
            (e.stage != 1 && e.stage != 2) || e.sampler >= 16)
            continue;
        encoder->_formSamplers[e.stage - 1] |= uint16_t(1u << e.sampler);
        __unsafe_unretained RMSampler *s = encoder->_samp[e.stage - 1][e.sampler];
        if (!s || !s->_raw.desc.cubic)
            continue;
        const raw::SamplerDesc &d = s->_raw.desc;
        if (std::none_of(b.samplers.begin(), b.samplers.end(), [&](const auto &c) {
                return c.slot == e.sampler && c.stage == e.stage;
            }))
            b.samplers.push_back({e.sampler, d.sAddress, d.tAddress, d.rAddress,
                                  uint8_t(s->_descriptor.borderColor & 3), !d.pixel, e.stage});
        if (e.kind != raw::EntryTexture || e.slot >= raw::kStageTextures)
            continue;
        raw::addSlot(encoder->_cubicTextures[e.stage - 1], e.slot);
        __unsafe_unretained RMTexture *t = encoder->_tex[e.stage - 1][e.slot];
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

void packedVariants(RMPipeline *p, uint64_t keyHash)
{
    std::lock_guard<raw::Mutex> lock(p->_variantLock);
    for (const auto &[name, pl] : gRM->raw->variantPipelines(keyHash, p->_caTransform)) {
        auto variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        variant->publish(pl);
        p->_variants.emplace(name, std::move(variant));
    }
}

// A packed state's test class the pack does not hold (its builder could not make it, and no other form is asked): the
// state's own record draws it, as a refused device compile leaves it, and says so
static raw::Pipeline *untestedVariant(RMPipeline *p, const std::string &name)
{
    const std::string key = p->_key + name;
    SWITCH_REFUSE_DETAIL("raw", "state.tests_unpacked", raw::Pack::fnv(key.data(), key.size()),
                         "a packed state's test class the pack builder could not make: drawn without it: %s",
                         key.c_str());
    return p->_raw;
}

// Binding and pass preparation starts asynchronous variant work. Sampled variants can use stand-ins while
// pending; other variants hold the queue head until they publish or fail (RawMetalSchedule.mm variantReadiness).
static raw::Variant<raw::Pipeline> *renderVariant(RMPipeline *p, const raw::CubicBinds &binds, uint8_t tests,
                                                  bool planes, uint16_t xrSlots,
                                                  const std::vector<raw::WrittenTexture> &written,
                                                  uint8_t xrShadows = 0)
{
    const raw::RenderVariant forms = {binds, tests, planes, xrSlots, written, xrShadows};
    const std::string suffix = forms.untested().name(), name = forms.name();
    std::shared_ptr<raw::Variant<raw::Pipeline>> variant;
    {
        std::lock_guard<raw::Mutex> lock(p->_variantLock);
        if (const auto found = p->_variants.find(name); found != p->_variants.end()) {
            if (found->second->state() != raw::Variant<raw::Pipeline>::State::Failed || !gRM->completion)
                return found->second.get();
            p->_replacedVariants.push_back(std::move(found->second));
            p->_variants.erase(found);
        }
        variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        p->_variants.emplace(name, variant);
    }
    if (!raw::variantMadeAtDraw(p->_packed, !binds.empty(), planes, xrSlots != 0, !written.empty(), xrShadows != 0)) {
        variant->publish(untestedVariant(p, name));
        return variant.get();
    }
    raw::Completion::Ask request;
    request.descriptor = p->_madeFrom;
    request.facade = p->_madeFrom;
    request.key = p->_key + name;
    request.caTransform = p->_caTransform;
    request.own = !p->_packable;
    request.variant = forms;
    request.resolveDescriptor = true;
    // the state's own record draws a test class's draws it could not be made for, never a cubic, two-plane, written or
    // shadowed variant's
    raw::Pipeline *base = binds.empty() && !planes && !xrSlots && written.empty() && !xrShadows ? p->_raw : nullptr;
    raw::Completion *completion = gRM->completion.get();
    if (!request.descriptor || !completion) {
        variant->publish(base);
        return variant.get();
    }
    raw::Completion::Ask fallback = request;
    fallback.key = p->_key + suffix;
    fallback.variant = forms.untested();
    completion->askVariant(std::move(request), [variant, base, fallback, tests,
                                                completion](raw::Pipeline *made, const std::string &failure) {
        if (!made && !failure.empty()) {
            variant->fail();
        } else if (!made && tests && !base) {
            completion->askVariant(fallback, [variant](raw::Pipeline *untested, const std::string &failed) {
                if (untested || failed.empty())
                    variant->publish(untested);
                else
                    variant->fail();
            });
        } else {
            variant->publish(made ? made : base);
        }
    });
    return variant.get();
}

// The textures a draw of `pl` writes in other formats than its record's programs store (raw_written_textures.h)
static void reboundTextures(const raw::Pipeline &pl, RMTexture *const textures[raw::kStageTextures],
                            std::vector<raw::WrittenTexture> &out)
{
    out.clear();
    for (uint32_t slots = pl.writtenSlots; slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        const uint32_t format = textures[slot] ? uint32_t(textures[slot]->_descriptor.pixelFormat) : 0u;
        if (format && format != pl.rec->writtenFormat[slot])
            out.push_back({uint8_t(slot), format});
    }
}

// The slots of each stage (vertex, fragment: raw::handleStage) a draw of `pl` samples a two-plane RGB10A8_2P_XR10
// texture at (raw_xr_pass.h)
static std::array<raw::StageTextureMask, 2> xrSampledSlots(const raw::Pipeline &pl,
                                                           RMTexture *const textures[2][raw::kStageTextures])
{
    std::array<raw::StageTextureMask, 2> slots{};
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const raw::PackEntry &e = pl.entries[i];
        __unsafe_unretained RMTexture *t =
            e.kind == raw::EntryTexture && e.slot < raw::kStageTextures ? textures[raw::handleStage(e)][e.slot] : nil;
        if (t && t->_raw.xrPlanes && !t->_raw.memoryless)
            raw::addSlot(slots[raw::handleStage(e)], e.slot);
    }
    return slots;
}

raw::Variant<raw::Pipeline> *rmRenderVariant(RMPipeline *p, const raw::CubicBinds &binds, uint8_t tests, bool planes,
                                             uint16_t xrSlots, const std::vector<raw::WrittenTexture> &written,
                                             uint8_t xrShadows)
{
    return renderVariant(p, binds, tests, planes, xrSlots, written, xrShadows);
}

@implementation RMRenderEncoder (Cubic)

- (void)prepareDrawnRecord
{
    raw::Pipeline *base = !_cb->_encRefused && _pipe ? _pipe->_raw : nullptr;
    _formVariant = _sampledVariant = nullptr;
    _sampledSlots = {};
    if (!base || !(_cubicMask[0] | _cubicMask[1])) {
        _forms.cubic.samplers.clear();
        _forms.cubic.textures.clear();
        _cubicTextures[0] = {};
        _cubicTextures[1] = {};
    } else {
        cubicBinds(self, *base, _forms.cubic);
    }
    _forms.written.clear();
    _forms.fragmentTests = 0;
    _forms.xrSlots = 0;
    _forms.twoPlane = _xrPlanes;
    if (!base) {
        _formSamplers[0] = _formSamplers[1] = 0;
        return;
    }
    _forms.fragmentTests =
        raw::drawnTests(base->fetchBits && base->depthFormat && !base->storageBits, base->rec->shaderTests, _ds, _zeta);
    reboundTextures(*base, _tex[1], _forms.written);
    if (!_forms.cubic.empty() || _forms.fragmentTests || _forms.twoPlane || !_forms.written.empty())
        _formVariant = renderVariant(_pipe, _forms.cubic, _forms.fragmentTests, _forms.twoPlane, 0, _forms.written);
    if (_cb->_passes[_passAt].xrPlaneBinds)
        _sampledSlots = xrSampledSlots(*base, _tex);
    _forms.xrSlots = raw::xrPairSlots(_sampledSlots[1]);
    if (_forms.xrSlots)
        _sampledVariant =
            renderVariant(_pipe, _forms.cubic, _forms.fragmentTests, _forms.twoPlane, _forms.xrSlots, _forms.written);
}

- (const raw::Pipeline *)drawnRecord
{
    raw::Pipeline *base = _pipe->_raw;
    auto *variant = _formVariant;
    std::array<raw::StageTextureMask, 2> standIns = {_sampledSlots[0], raw::pastXrPairSlots(_sampledSlots[1])};
    uint16_t drawnSlots = 0;
    if (_sampledVariant) {
        if (_sampledVariant->state() == raw::Variant<raw::Pipeline>::State::Ready) {
            variant = _sampledVariant;
            drawnSlots = _forms.xrSlots;
        } else {
            standIns[1] = _sampledSlots[1];
        }
    }
    if (raw::anySlot(standIns[0]) || raw::anySlot(standIns[1]))
        rmPipelineVeto(_cb, VetoShadows);
    for (uint32_t stage = 0; stage < 2; stage++)
        raw::forEachSlot(standIns[stage], [&](uint32_t slot) {
            _cb->_xrStandIns.insert(_tex[stage][slot]);
        });
    rmXrShadowDraw(_cb, _pipe, *base, variant, _forms.cubic, _forms.fragmentTests, _forms.twoPlane, drawnSlots,
                   _forms.written);
    if (base != _drawn || variant != _drawnVariant) {
        Cmd state = cmd(OpPipeline);
        state.obj = (__bridge void *)_pipe;
        state.obj2 = variant;
        [_cb record:state];
        _drawn = base;
        _drawnVariant = variant;
    }
    return base;
}

@end
