// SPDX-License-Identifier: MIT
// Engine selection: each ladder tries a fixed-function engine before a shader pass of the pack's own state, and takes the
// first selectable rung. A ladder with no selectable rung answers an invalid Choice, which validation reports.
#pragma once
#include "plan_caps.h"

namespace raw::plan {

struct Choice {
    Rule rule = Rule::Count;
    Engine engine = Engine::None;

    bool valid() const
    {
        return rule != Rule::Count;
    }
};

Choice copyEngine(const ResourceFacts &source, const ResourceFacts &destination, const Caps &caps);
Choice fillEngine(const ResourceFacts &buffer, const Caps &caps);
Choice clearEngine(const ResourceFacts &target, const Caps &caps);
Choice resolveEngine(const ResourceFacts &multisample, const ResourceFacts &into, const Caps &caps);
Choice mipEngine(const ResourceFacts &texture, const Caps &caps);

// The formats whose 2D-engine filtering matches Metal's on the device today (raw/fx/fx_mip.cpp twodFormat)
bool mipFilteredByTwoD(uint16_t format);

// The rung a fetched colour takes: the record's rung, or a lower one the record names as a candidate once its probe passed
Rung fetchRung(uint8_t recorded, const Caps &caps, Rule &rule);

}  // namespace raw::plan
