// SPDX-License-Identifier: MIT
#include "plan_engine.h"

namespace raw::plan {

namespace {

constexpr uint8_t kRungMask = 0x0f, kCandidateBlend = 0x10, kCandidateIterated = 0x20;

Choice pick(const Caps &caps, Rule rule, Engine engine)
{
    return caps.selectable(rule) ? Choice{rule, engine} : Choice{};
}

template <typename... Rest> Choice first(Choice c, Rest... rest)
{
    if constexpr (sizeof...(rest) == 0)
        return c;
    else
        return c.valid() ? c : first(rest...);
}

bool sameTexels(const ResourceFacts &a, const ResourceFacts &b)
{
    if (a.kind == ResourceFacts::Buffer || b.kind == ResourceFacts::Buffer)
        return true;
    const switch_gm20b_format *fa = Caps::format(a.format), *fb = Caps::format(b.format);
    return fa && fb && fa->bytes == fb->bytes && fa->block_w == fb->block_w && fa->block_h == fb->block_h &&
           (fa->flags & SWITCH_GM20B_FORMAT_COPY);
}

}  // namespace

bool mipFilteredByTwoD(uint16_t format)
{
    switch (format) {
    case 80:
    case 70:
    case 115:
    case 10:
    case 30:
        return true;
    default:
        return false;
    }
}

Choice copyEngine(const ResourceFacts &source, const ResourceFacts &destination, const Caps &caps)
{
    if (!sameTexels(source, destination))
        return {};
    const bool compressed = source.has(ResourceFacts::Compressed) || destination.has(ResourceFacts::Compressed);
    if (!compressed)
        return pick(caps, Rule::CopyEngine, Engine::Copy);
    const bool twod = source.kind == ResourceFacts::Texture && destination.kind == ResourceFacts::Texture &&
                      source.format == destination.format && Caps::formatHas(source.format, SWITCH_GM20B_FORMAT_TWOD);
    return first(pick(caps, Rule::CopyEngineCompressed, Engine::Copy),
                 twod ? pick(caps, Rule::CopyTwoD, Engine::TwoD) : Choice{});
}

Choice fillEngine(const ResourceFacts &buffer, const Caps &caps)
{
    if (buffer.kind != ResourceFacts::Buffer)
        return {};
    return pick(caps, Rule::FillCopyEngine, Engine::Copy);
}

Choice clearEngine(const ResourceFacts &target, const Caps &caps)
{
    const bool twod = target.layout == Layout::Pitch && target.samples == 1 && !target.has(ResourceFacts::Compressed) &&
                      Caps::formatHas(target.format, SWITCH_GM20B_FORMAT_TWOD);
    return first(twod ? pick(caps, Rule::ClearTwoD, Engine::TwoD) : Choice{},
                 pick(caps, Rule::ClearThreeD, Engine::ThreeD));
}

Choice resolveEngine(const ResourceFacts &multisample, const ResourceFacts &into, const Caps &caps)
{
    const bool twod = multisample.format == into.format && Caps::formatHas(into.format, SWITCH_GM20B_FORMAT_TWOD) &&
                      !Caps::formatHas(into.format, SWITCH_GM20B_FORMAT_ZETA);
    const bool draw = caps.own.available(OwnState::Resolve, into.format);
    return first(twod ? pick(caps, Rule::ResolveTwoD, Engine::TwoD) : Choice{},
                 draw ? pick(caps, Rule::ResolveDraw, Engine::ThreeD) : Choice{});
}

Choice mipEngine(const ResourceFacts &texture, const Caps &caps)
{
    const bool today = texture.has(ResourceFacts::MipChain2D) && mipFilteredByTwoD(texture.format);
    const bool candidate = Caps::formatHas(texture.format, SWITCH_GM20B_FORMAT_TWOD) &&
                           Caps::formatHas(texture.format, SWITCH_GM20B_FORMAT_FILTER);
    const bool draw = caps.own.available(OwnState::Mip, texture.format);
    return first(today ? pick(caps, Rule::MipTwoD, Engine::TwoD) : Choice{},
                 candidate ? pick(caps, Rule::MipTwoDProbed, Engine::TwoD) : Choice{},
                 draw ? pick(caps, Rule::MipDraw, Engine::ThreeD) : Choice{});
}

Rung fetchRung(uint8_t recorded, const Caps &caps, Rule &rule)
{
    const Rung base = Rung(recorded & kRungMask);
    if ((recorded & kCandidateBlend) && caps.selectable(Rule::FetchBlend)) {
        rule = Rule::FetchBlend;
        return Rung::Blend;
    }
    if ((recorded & kCandidateIterated) && caps.selectable(Rule::FetchIterated)) {
        rule = Rule::FetchIterated;
        return Rung::Iterated;
    }
    switch (base) {
    case Rung::Barrier:
        rule = Rule::FetchBarrier;
        break;
    case Rung::Blend:
        rule = Rule::FetchBlend;
        break;
    case Rung::Iterated:
        rule = Rule::FetchIterated;
        break;
    case Rung::Interlock:
        rule = Rule::FetchInterlock;
        break;
    case Rung::None:
        rule = Rule::Recorded;
        break;
    }
    return base;
}

}  // namespace raw::plan
