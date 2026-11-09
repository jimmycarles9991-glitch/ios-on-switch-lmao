// SPDX-License-Identifier: MIT
// A state's record (raw_record.h) from its parts, by pack/mkpack.py's rules: what the probe's capture (raw_record.cpp) and the
// direct compile (raw_direct.h) each gather of a state, made into the same record.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "raw_record.h"

namespace raw {

enum class Tex : uint8_t {
    None,
    Slot,
    Fetch,
    Texture3D,
    Unknown
};

enum class Smp : uint8_t {
    None,
    Slot,
    Derived,
    Constexpr
};

// A word range of set 0 the state's draw reads: a buffer's descriptor, or a texture and sampler handle
struct LayoutEntry {
    uint32_t offset = 0;
    bool buffer = false;
    bool colorTable = false;      // m2v's sRGB colour table (EntryColorTable)
    uint8_t stage = 0, slot = 0;  // a buffer's
    Tex tex = Tex::None;
    Smp smp = Smp::None;
    uint8_t texSlot = 0, smpSlot = 0;  // the probe's texture3d: kNoSlot (its Metal index is unknown)
    bool headerKnown = false;          // a constexpr sampler's header
    std::array<uint32_t, 8> header = {};
};

struct RecordParts {
    const CapturedProgram *programs[2] = {};  // vertex, fragment
    std::vector<LayoutEntry> layout;          // by offset
    // By set-0 byte of a buffer entry: the range its descriptor holds with 16- and 4096-byte payloads; by the value a slot's
    // residual has in the bind-offset draw (probeBindOffset, a storage slot's probeStorageCode), the root word (0x700 + 4 x
    // word) that holds it
    std::map<uint32_t, uint32_t> sizes16, sizes4096, residuals;
    uint8_t vertexSlot = 0xFF;
    uint16_t vertexStride = 0;
    std::map<uint32_t, uint32_t> regs;  // the 3D registers at the state's draw
    std::map<uint32_t, uint32_t> push;  // the root words [0x700, 0x780) the draw writes
    uint16_t setBytes = 0;
};

// The color attachments a key names (mkpack.py targets)
uint32_t targets(const std::string &key);
// A color target's own register for a target the state does not have (mkpack.py foreign)
bool foreign(uint32_t method, uint32_t targets);

// Whether both programs are known and bind only what the raw path binds (mkpack.py raw_bindable); false: why not
bool programsBindable(const RecordParts &parts, std::string &why);
// mkpack.py's record of `parts`, the state of pack key `key`; false: set aside (`why`)
bool assembleRecord(const RecordParts &parts, const std::string &key, const PackRules &rules, RuntimeRecord &out,
                    std::string &why);

}  // namespace raw
