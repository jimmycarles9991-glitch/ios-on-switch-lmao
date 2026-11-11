// SPDX-License-Identifier: MIT
// A draw's cubic variant of its state (raw_cubic.h): the binds it is made for, and the variant itself, made at its first draw as
// the state is at its creation (RawComplete.h: from this process's or an earlier boot's record cache, else compiled), or asked
// again off the drawing thread where that compile could not run.
#import "RawMetalInternal.h"

raw::CubicBinds cubicBinds(const raw::Pipeline &pl, RMSampler *const samplers[16], RMTexture *const textures[16])
{
    raw::CubicBinds b;
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if ((e.kind != raw::EntryTexture && e.kind != raw::EntrySamplerOnly) || e.samplerKind != raw::SamplerSlot ||
            e.sampler >= 16)
            continue;
        RMSampler *s = samplers[e.sampler];
        if (!s || !s->_raw.desc.cubic)
            continue;
        const raw::SamplerDesc &d = s->_raw.desc;
        if (std::none_of(b.samplers.begin(), b.samplers.end(), [&](const auto &c) {
                return c.slot == e.sampler;
            }))
            b.samplers.push_back(
                {e.sampler, d.sAddress, d.tAddress, d.rAddress, uint8_t(s->_descriptor.borderColor & 3), !d.pixel});
        RMTexture *t = e.kind == raw::EntryTexture && e.slot < 16 ? textures[e.slot] : nil;
        if (t && std::none_of(b.textures.begin(), b.textures.end(), [&](const auto &c) {
                return c.slot == e.slot;
            }))
            b.textures.push_back({e.slot, uint32_t(t->_descriptor.pixelFormat)});
    }
    std::sort(b.samplers.begin(), b.samplers.end(), [](const auto &x, const auto &y) {
        return x.slot < y.slot;
    });
    std::sort(b.textures.begin(), b.textures.end(), [](const auto &x, const auto &y) {
        return x.slot < y.slot;
    });
    return b;
}

raw::Pipeline *cubicVariant(RMPipeline *p, const raw::CubicBinds &b, bool *making)
{
    *making = false;
    const std::string name = b.key();
    {
        std::lock_guard<std::mutex> l(p->_variantLock);
        if (const auto it = p->_variants.find(name); it != p->_variants.end())
            return it->second;
        if (p->_variantsAsked.count(name)) {
            *making = true;
            return nullptr;
        }
    }
    raw::Completion::Ask a;
    a.descriptor = p->_madeFrom ? twinDescriptor(p->_madeFrom) : nil;
    a.key = p->_key + name;
    a.caTransform = p->_caTransform;
    a.own = !p->_packable;
    a.cubic = b;
    bool retry = false;
    raw::Pipeline *made = a.descriptor && gRM->completion ? gRM->completion->makeNow(a, &retry) : nullptr;
    std::lock_guard<std::mutex> l(p->_variantLock);
    if (made || !retry) {
        p->_variants[name] = made;
        return made;
    }
    *making = true;
    p->_variantsAsked.insert(name);
    __weak RMPipeline *weak = p;
    gRM->completion->ask(std::move(a), [weak, name](raw::Pipeline *pl) {
        if (RMPipeline *strong = weak) {
            std::lock_guard<std::mutex> held(strong->_variantLock);
            strong->_variants[name] = pl;
            strong->_variantsAsked.erase(name);
        }
    });
    return nullptr;
}

@implementation RMRenderEncoder (Cubic)

// The record a draw takes: a cubic variant of the state where a cubic sampler is bound where it samples (raw_cubic.h), else
// the state's own; recorded where it changes (OpPipeline's c); null where the draw goes to m2v
- (const raw::Pipeline *)drawnRecord
{
    const raw::Pipeline *want = _pipe->_raw;
    if (_cubicMask) {
        const raw::CubicBinds b = cubicBinds(*_pipe->_raw, _samp, _tex);
        if (!b.empty()) {
            bool making = false;
            if (!(want = cubicVariant(_pipe, b, &making))) {
                [_cb takeReason:WhyState
                            why:making ? @"a cubic sampler's variant of the state, being made"
                                       : @"a cubic sampler's variant of the state the runtime refused"];
                hit(_cb, making ? PredStateBeingMade : PredCallSamplerForeign, b.samplers[0].slot);
                return nullptr;
            }
        }
    }
    if (want != _drawn) {
        Cmd m = cmd(OpPipeline);
        m.obj = (__bridge void *)_pipe;
        m.c = want == _pipe->_raw ? 0 : uint64_t(uintptr_t(want));
        [_cb record:m];
        _drawn = const_cast<raw::Pipeline *>(want);
    }
    return want;
}

@end
