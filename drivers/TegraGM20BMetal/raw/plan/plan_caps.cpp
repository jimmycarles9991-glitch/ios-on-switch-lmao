// SPDX-License-Identifier: MIT
#include "plan_caps.h"

namespace raw::plan {

namespace {

// Rows a route runs on today rest on the grade their caps row has; a route the runtime never took needs its probe.
constexpr RuleEvidence kEvidence[] = {
    {Rule::Recorded, SWITCH_GM20B_FEATURE_3D_B8, Grade::Source, Probe::None},
    {Rule::CopyEngine, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::None},
    {Rule::CopyEngineCompressed, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::CeCompressed},
    {Rule::CopyTwoD, SWITCH_GM20B_FEATURE_2D_1, Grade::Host, Probe::TwodScale},
    {Rule::FillCopyEngine, SWITCH_GM20B_FEATURE_CE_4, Grade::Host, Probe::None},
    {Rule::ClearThreeD, SWITCH_GM20B_FEATURE_3D_G8, Grade::Chip, Probe::None},
    {Rule::ClearTwoD, SWITCH_GM20B_FEATURE_2D_4, Grade::Source, Probe::TwodFill},
    {Rule::ResolveDraw, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::ResolveTwoD, SWITCH_GM20B_FEATURE_2D_11, Grade::Source, Probe::TwodResolve},
    {Rule::MipTwoD, SWITCH_GM20B_FEATURE_2D_1, Grade::Host, Probe::None},
    {Rule::MipTwoDProbed, SWITCH_GM20B_FEATURE_2D_1, Grade::Host, Probe::MipFilter},
    {Rule::MipDraw, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::PlanarCodes, SWITCH_GM20B_FEATURE_CE_4, Grade::Host, Probe::None},
    {Rule::PlanarStandInLoad, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::PlanarStandInStore, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::YcbcrPlaneFill, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::None},
    {Rule::YcbcrImage, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::ShadowRefresh, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::None},
    {Rule::ExtendedRangeStandIn, SWITCH_GM20B_FEATURE_3D_G1, Grade::Source, Probe::None},
    {Rule::ComputeShadowLoad, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::None},
    {Rule::ComputeShadowStore, SWITCH_GM20B_FEATURE_CE_3, Grade::Host, Probe::None},
    {Rule::FetchBarrier, SWITCH_GM20B_FEATURE_3D_A6, Grade::Chip, Probe::None},
    {Rule::FetchBlend, SWITCH_GM20B_FEATURE_3D_G4, Grade::Chip, Probe::BlendExact},
    {Rule::FetchIterated, SWITCH_GM20B_FEATURE_3D_G5, Grade::Source, Probe::IteratedBlend},
    {Rule::FetchInterlock, SWITCH_GM20B_FEATURE_3D_G12, Grade::Chip, Probe::None},
    {Rule::PassPixelBarrier, SWITCH_GM20B_FEATURE_3D_A6, Grade::Chip, Probe::PassPixelBarrier},
    {Rule::EntryNoDrain, SWITCH_GM20B_FEATURE_HOST_4, Grade::Chip, Probe::IdleJobBoundary},
    {Rule::SwitchDrain, SWITCH_GM20B_FEATURE_HOST_9, Grade::Chip, Probe::SwitchDrain},
};
static_assert(sizeof kEvidence / sizeof *kEvidence == size_t(Rule::Count), "evidence for each rule");

constexpr bool ordered()
{
    for (size_t i = 0; i < size_t(Rule::Count); ++i)
        if (kEvidence[i].rule != Rule(i))
            return false;
    return true;
}

static_assert(ordered(), "evidence in Rule order");

}  // namespace

const RuleEvidence &evidence(Rule rule)
{
    return kEvidence[size_t(rule)];
}

const switch_gm20b_format *Caps::format(uint16_t mtl)
{
    for (const switch_gm20b_format &f : switch_gm20b_formats)
        if (f.mtl == mtl)
            return &f;
    return nullptr;
}

bool Caps::formatHas(uint16_t mtl, uint32_t flag)
{
    const switch_gm20b_format *f = format(mtl);
    return f && (f->flags & flag);
}

bool Caps::hasFeature(switch_gm20b_feature f, Grade at)
{
    const switch_gm20b_feature_row &row = switch_gm20b_features[f];
    return row.present && row.grade >= uint8_t(at);
}

bool Caps::selectable(Rule rule) const
{
    const RuleEvidence &e = evidence(rule);
    return hasFeature(e.feature, e.grade) && passed(e.probe);
}

}  // namespace raw::plan
