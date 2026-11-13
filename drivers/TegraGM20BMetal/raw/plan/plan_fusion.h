// SPDX-License-Identifier: MIT
// Pass fusion with one pass of lookahead: a render encoder that only continues the one before it (same targets, the first
// stores what the next loads, no resolve, nothing between) is drawn as the same pass, with no barrier and no new targets.
#pragma once
#include "plan_recording.h"

namespace raw::plan {

bool fusible(const Recording &r, const Encoder &first, const Encoder &next);

}  // namespace raw::plan
