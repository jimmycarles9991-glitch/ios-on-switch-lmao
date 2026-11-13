// SPDX-License-Identifier: MIT
// Barriers from hazards: what one access needs after an earlier access of the same memory, as a union of clauses. Each
// clause is one reason a barrier exists, with the place NVK (third_party/mesa/src/nouveau/vulkan) emits it.
#pragma once
#include "plan_caps.h"

namespace raw::plan {

struct Access {
    Path path = Path::Texture;
    bool write = false;
    Stage stage = Stage::Fragment;
    Engine engine = Engine::ThreeD;
};

enum class Clause : uint8_t {
    Execution,
    FlushStorage,
    TiledCache,
    TextureRead,
    StorageRead,
    ConstantRead,
    IndirectRead,
    PixelOrdered,
    InPassStorage,
    InterlockOrder,
    CopyOrder,
    WriteAfterRead,
    CrossPassPixel,
    FetchOnly,
    Count
};

struct ClauseInfo {
    Clause clause;
    const char *name;
    const char *nvk;  // where NVK emits it, or what it does instead
    const char *raw;  // where the raw runtime emits it today
};

const ClauseInfo &clauseInfo(Clause c);

enum class Scope : uint8_t {
    InPass,
    AcrossNodes
};

struct Requirement {
    Barriers barriers = 0;  // what the selectable rules need
    Barriers relaxed = 0;   // what the probe-gated relaxation needs instead, where one applies
    uint32_t clauses = 0;   // bit per Clause that contributed

    bool none() const
    {
        return !barriers;
    }
};

// Whether two accesses conflict at all: something writes, and neither is an interlock storage access ordered by the ticket
bool conflicts(const Access &earlier, const Access &later, Scope scope);

Requirement hazard(const Access &earlier, const Access &later, Scope scope, bool tiledCache);

// The barriers a node needs, the relaxation applied where its probe passed
Barriers required(const Requirement &r, const Caps &caps);

// The engines bound as subchannels of the GPU channel; a change from one to another drains the engine left behind
// (nvk_cmd_buffer.c:678-687)
bool onChannel(Engine e);
bool switchDrains(Engine from, Engine to);

}  // namespace raw::plan
