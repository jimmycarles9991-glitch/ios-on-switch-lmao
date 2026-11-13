// SPDX-License-Identifier: MIT
// A state's record (raw_record.h) from its parts, by pack/mkpack.py's rules: what the probe's capture (raw_record.cpp) and the
// direct compile (raw_direct.h) each gather of a state, made into the same record.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "raw_descriptors.h"
#include "raw_record.h"

namespace raw {

enum class Tex : uint8_t {
    None,
    Slot,
    Fetch,
    Texture3D,
    DepthTest,
    StencilTest,
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
    bool colorTable = false;  // m2v's sRGB colour table (EntryColorTable)
    // a handle table (EntryTextureTable, EntrySamplerTable) or argument table (EntrySpillTable) of stage `stage`
    uint8_t table = 0;
    uint8_t stage = 0, slot = 0;  // a buffer's; a handle's stage alone (0: the fragment stage)
    Tex tex = Tex::None;
    Smp smp = Smp::None;
    uint8_t texSlot = 0, smpSlot = 0;  // the probe's texture3d: kNoSlot (its Metal index is unknown)
    // a texture the programs access as a storage image: the MTLPixelFormat they were compiled for (PackPipeline::writtenFormat)
    uint16_t writtenFormat = 0;
    bool smpCompare = false;   // a sampler slot the state samples by comparison
    bool headerKnown = false;  // a constexpr sampler's header
    int8_t tessBuffer = -1;    // a tessellation state's control-stage buffer (PackTessBuffer)
    std::array<uint32_t, 8> header = {};
};

// What a state's reads through argument buffers take of a draw: PackPipeline's root-table and heap fields
struct RecordHeap {
    uint8_t rootTableWord[2] = {}, statusWord[2] = {};
    uint32_t rootSlots[2] = {};
    uint8_t arrays = 0;
    uint32_t bytes = 0;
    PackHeapArray array[kPackHeapArrays] = {};
};

struct RecordParts {
    const CapturedProgram *programs[2] = {};      // vertex, fragment
    const CapturedProgram *tessPrograms[2] = {};  // a tessellation state's control and evaluation programs
    uint8_t controlWord = 0, controlPoints = 0, controlPointIndexBytes = 0;  // PackPipeline's
    std::vector<LayoutEntry> layout;                                         // by offset
    // By set-0 byte of a buffer entry: the range its descriptor holds with 16- and 4096-byte payloads; by the value a slot's
    // residual has in the bind-offset draw (probeBindOffset, a storage slot's probeStorageCode), the root word (0x700 + 4 x
    // word) that holds it
    std::map<uint32_t, uint32_t> sizes16, sizes4096, residuals;
    uint8_t vertexSlot = 0xFF;
    uint16_t vertexStride = 0;
    bool vertexStrideDynamic = false;
    uint8_t pulledStride[4][2] = {};                     // PackPipeline's
    uint8_t vertexStreams[kPackVertexStreams - 1] = {};  // PackPipeline's
    uint8_t amplificationMax = 0, amplificationIdWord = 0, amplificationCountWord = 0;
    uint8_t arrayLengthWord = 0, viewportCountWord = 0;
    std::map<uint32_t, uint32_t> regs;  // the 3D registers at the state's draw
    // a two-plane state's alpha targets (raw_xr_pass.h xrAlphaTargets): its registers there are its own, as those of the
    // colours its key names are
    uint32_t alphaTargets = 0;
    std::map<uint32_t, uint32_t> push;  // the root words [0x700, 0x780) the draw writes
    uint16_t setBytes = 0;
    bool pushMetadata = false;
    uint8_t blendColorWord = 0, halfBlendColorWord = 0;
    uint8_t storageMask = 0, storageWriteMask = 0, storageSamples = 0, storageTests = 0;
    uint8_t storageBlendMask = 0, storageBlendModel = 0;
    uint32_t storageTic[8] = {};
    PackShaderTests shaderTests = {};
    RecordHeap heap;
    std::vector<uint32_t> spill[2];  // each stage's argument table words (raw_spill_table.h), vertex then fragment
    // by stage and buffer slot, the record's word + 1 of the slot's residual as m2v's plan places it (NmbDirectDraw's
    // residualWord); where the plan names them, `residuals` (the probe's codes) names none
    uint8_t residualWord[2][32] = {};
    bool residualsPlanned = false;
};

// The color attachments a key names, bit j its c<j> (mkpack.py targets)
uint32_t targets(const std::string &key);
// A color target's own register for a target the state does not have, past its last or between two it has (mkpack.py
// foreign)
bool foreign(uint32_t method, uint32_t targets);

// Whether both programs are known and bind only what the raw path binds (mkpack.py raw_bindable); false: why not
bool programsBindable(const RecordParts &parts, std::string &why);
// mkpack.py's record of `parts`, the state of pack key `key`; false: set aside (`why`)
bool assembleRecord(const RecordParts &parts, const std::string &key, const PackRules &rules, RuntimeRecord &out,
                    std::string &why);

// The set-0 entries the record's final programs read: its two programs' (PackPipeline::memoryEntries), its variants'
// and its split's
uint32_t recordSetReads(const RuntimeRecord &rec);
// A fetch whose set-0 words none of the record's programs reads fetches nothing (the compiler removed every access):
// it leaves the record with the colour's storage view and rung, and the record fetches only where another one stays.
// Returns the colours dropped.
uint8_t dropUnreadFetches(RuntimeRecord &rec);
// Whether any of the record's programs loads a surface (SULD): a storage form whose programs load none only writes its
// colours, what its barrier form's program does through the ROP with no interlock
bool loadsSurface(const RuntimeRecord &rec);

}  // namespace raw
