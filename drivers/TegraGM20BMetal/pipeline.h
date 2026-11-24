// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "band_rates.h"
#include "fetch_free.h"
#include "heap_tables.h"
#include "pack.h"
#include "set_entries.h"
#include "state_segment.h"
#include "stats.h"

namespace gm20b {

struct SplitState;
struct SplitSource;
struct TurnedSource;

#if SWITCH_METAL_DIAGNOSTICS
// The keyline probe (keyline_probe.h) for 30 s or 256 lines: its budgets start again, and every Core Animation state's
// thin draws log their line
void armKeylineProbe();
#endif

// Whether a pack key is one of the driver's own states (own_states.h)
bool isOwnState(uint64_t keyHash);

struct Pipeline {
    const PackPipeline *rec = nullptr;
    SetEntries entries;  // its record's, with its far block
    uint32_t vertexInstructions = 1, fragmentInstructions = 1;
    // vertex, fragment: its program can write memory a buffer binding names (program_stores.h)
    bool writesMemory[2] = {true, true};
    std::vector<uint32_t> methods;  // the state's registers as ready push words (program offsets relocated)
    uint32_t viewportClip = 0;      // its SET_VIEWPORT_CLIP_CONTROL
    bool namesViewportClip = false;
    bool namesViewportClipRange = false;   // it sets SET_VIEWPORT_CLIP_MIN_Z or MAX_Z
    StateSegment segment;                  // the same words in the device's segment heap; empty: they go inline
    uint32_t push[32] = {}, pushMask = 0;  // root table [0x700, 0x780): the words m2v's push ranges write, and which
    // the constant buffer slot the vertex, pixel, tessellation init and tessellation program read set 0 at (0: none), whose
    // bind groups are kSetGroups'
    uint8_t setSlot[4] = {};
    uint16_t streamStride[kPackVertexStreams] =
        {};  // each vertex stream's static stride (its SET_VERTEX_STREAM_A_FORMAT)

    // the uniform buffers of set 0 its programs read as constant buffers: the group (0 vertex, 4 pixel), the slot and the set's
    // entry that names the buffer; false where a program reads one no entry names (its draws are refused)
    struct UboCbuf {
        uint8_t group, slot;
        uint16_t entry;
    };

    UboCbuf ubos[4 * (sizeof(PackProgram::cbufType) - 1)] = {};  // constant buffers 1.. of each program
    uint8_t uboCount = 0;
    bool ubosNamed = true;
    // set 0's entries its programs read from the set's upload: the record's (PackPipeline::memoryEntries), and those a
    // variant's or a split twin's own programs read
    uint32_t memoryEntries = ~0u;
    uint8_t writeBits = 0;  // colors its draws write: SET_CT_WRITE(i) with a channel on
    // colors its fragment shader reads, and may write, as storage images inside the pixel interlock (metal2vulkan's storage
    // form of a fetch: the state turns the interlock on with SET_PIXEL_SHADER_INTERLOCK_CONTROL); the ROP writes none of them
    uint8_t storageBits = 0;
    uint8_t storageWriteBits = 0;
    // of storageWriteBits, those whose fixed-function blend the fragment shader computes inside the interlock
    uint8_t storageBlendBits = 0;
    uint32_t storageTic[8] = {};
    // the same state's barrier form, which orders its fetches with barriers where a pass's attachments cannot be viewed as
    // the storage images of this one (storage_form.h); resolved with the state, null where the pack has none
    Pipeline *barrierTwin = nullptr;
    // a storage form that computes the pipeline's blend: its barrier form, which a draw takes only a triangle at a time
    // (rule fetch.twin, twin_rule.h), not where an attachment cannot be viewed as its storage images
    Pipeline *blendTwin = nullptr;
    // a state of bind-stride vertex streams: the same state with those layouts pulled in the shader (RuntimeRecord's
    // strideTwin), which a draw takes where a stream's bind stride is past SET_VERTEX_STREAM_A_FORMAT's 12 bits
    Pipeline *strideTwin = nullptr;
    // the same state's fetch-free form (fetch_free.h), which a draw takes where its rung's rule holds; null where the
    // pack has none. On that form: the rung it draws on and the colour it blends
    Pipeline *fetchFree = nullptr;
    FetchFreeForm freeForm;
    bool constantBlend = false;     // a blend factor of a blended color is the encoder's blend color
    bool blendKill = false;         // its SET_ITERATED_BLEND_OPTIMIZATION drops zero-source pixels (blend_kill.h)
    bool fragmentDepth = false;     // its fragment program writes depth (SPH OMAP_DEPTH)
    bool viewportIndex = false;     // its vertex program writes ViewportIndex (SPH OMAP, viewports.h)
    bool writesLayer = false;       // its last stage before the rasterizer writes Layer: SET_RT_LAYER takes it
    uint32_t colorFormats[8] = {};  // MTLPixelFormat of each color attachment the state was made for (0: none)
    // its vertex function is Core Animation's fixed_vert_l?h_spc (bounds.h), and attribute 0 (the position) as its key
    // gives it: floats from the stream's vertex start (0: no float position)
    bool caTransform = false;
    uint8_t positionComponents = 0;
    uint16_t positionOffset = 0;
    // its draws may leave a pixel of their triangles unwritten: the fragment program discards (SPH KILLS_PIXELS) or writes
    // the sample mask, or the state tests depth, stencil or alpha to coverage, or rasterizes more than one sample
    bool dropsPixels = true;
    bool primitiveId = false;  // fragment input observes primitive numbering, which a split BEGIN would reset
    uint8_t samples = 1;       // its key's rasterSampleCount: a pass of attachments of that many samples draws it
    // SET_HYBRID_ANTI_ALIAS_CONTROL's passes: more than one where it shades per sample, whose sample masks differ (samples.h)
    uint8_t samplePasses = 1;
    // its registers set SET_ANTI_ALIAS_ALPHA_CONTROL; owned: for its own draws, whatever its Metal state asks
    // (alpha_control.h)
    bool namesAlphaControl = false, ownsAlphaControl = false;
    // one of the driver's own states (own_states.h): its draws read a texture's codes as they lie, as the conversion
    // that draws a stand-in reads the codes the stand-in holds the values of
    bool own = false;
    mutable BandRate bandRate;  // what its draws cost the GPU, learned from its measured ones (band_rates.h)
    uint8_t readBits = 0;       // colors its draws read as they were: blended into, or written through a partial mask
    uint8_t fetchBits = 0;      // colors it reads by framebuffer fetch
    uint32_t depthFormat =
        0;  // its key's depth attachment format, else its stencil one (ds=<format>/<format>; 0: none)
    // push words setting back the registers outside the pack's varying ones its methods set (record.h extras, a pack
    // record's in its blob), to their value in every other state: emitted before the next state's methods
    std::vector<uint32_t> restore;
    bool stagesOff = false;  // its methods turn the pixel stage or the rasteriser off (stage_switches.h)
    // where its fragment program is split (split.cpp): what its twin is made of, and the twin, made at the first draw that
    // takes it (both read without the device's lock); on that twin, its split
    const SplitSource *splitSource = nullptr;
    SplitState *split = nullptr;
    const SplitState *splitOf = nullptr;
    // DEVELOPMENT: a glass background or generic merge state's name until its uniforms show a non-finite float (one such
    // value makes the whole glass band black), and whether non-finite halves alone were reported
    const char *uniformProbe = nullptr;
    bool uniformHalvesReported = false;
    bool genericState = false;  // DEVELOPMENT: Core Animation's generic merge state (..Xghfc), for the fetch keys line
    // its stages' program variants (variants.cpp), and their prepared pipelines, vertex choice major (the last: this one)
    const PackVariant *variants[2] = {};
    uint8_t variantCount[2] = {};
    std::vector<Pipeline *> chosen;

    // DEVELOPMENT: a Core Animation state's name, whose draws log their form and quads (RenderEncoder::probeDraw): a solid
    // fill into its three-colour pass (..Xmw_A3Xhfc, the scroll edge keyline's state: fillState) its first draws and thin
    // ones always, any other state its thin ones while the keyline probe is armed (armKeylineProbe); the probe's generation
    // this form's lines count in, and its lines there, of any draw and of thin ones
    const char *caName = nullptr;
    bool fillState = false;
    const char *spanName = nullptr;  // DEVELOPMENT: the driver's own states' name in the long span line (span_marks.h)
    uint32_t probeGeneration = 0;
    uint8_t probeLines = 0, probeThinLines = 0;
    // rotation.h: its fragment program has a turned variant (AssumeTurned), or reads a turned plane's axes with none; a
    // runtime record's turned variant's pipeline (a pack record's is one of `chosen`)
    bool turnedVariant = false, turnRefused = false;
    // the fragment slots its programs read two-plane RGB10A8_2P_XR10 textures at through their header pairs (PackPipeline)
    uint16_t xrSlots = 0;
    uint16_t writtenSlots = 0;  // the fragment slots its programs write textures at (written_textures.h)
    Pipeline *turnedTwin = nullptr;
    const TurnedSource *turnedSource = nullptr;  // a runtime record's turned variant, built at its first turned draw
    // its vertex program writes memory (SPH DOES_GLOBAL_STORE), which a reach twin or bands would write again
    bool vertexStores = false;
    Pipeline *reachTwin = nullptr;  // Device::reachTwin's
    // its record's set 1 (heap_tables.h, null: none) and the constant buffer slot its vertex, pixel, tessellation init
    // and tessellation program reads it at (0: none), whose bind groups are kSetGroups'; its draws refused where the device
    // had no memory for it
    const HeapSet *heapSet = nullptr;
    uint8_t heapSlot[4] = {};
    bool heapMissing = false;
    // its argument tables' words (spill_table.h), vertex then fragment: the pack's or its runtime record's; null: none
    const uint32_t *spill[2] = {};
};

// A state's fragment program split (pack.h PackSplit): the twin that draws of rec.thresholdPixels pixels or more take, and
// the prologue placed in the heap
struct SplitState {
    PackSplit rec = {};
    Pipeline pipeline;
    uint32_t prologue = 0;  // QMD PROGRAM_OFFSET
    uint64_t prologueData = 0;
    uint32_t prologueDataBytes = 0;
};

// A split draw's prologue: the constant buffers its launch binds (PackSplit)
struct SplitLaunch {
    const SplitState *split = nullptr;
    uint32_t batchRecord = ~0u;
    uint64_t cbuf[8] = {};
    uint32_t cbufBytes[8] = {};
};

// Pipeline::restore's push words of `dwords` words of (method, value) pairs
std::vector<uint32_t> restoreWords(const uint32_t *pairs, uint32_t dwords);

// A program in the heap: its record, its SET_PIPELINE_PROGRAM value, its shader data's GPU address (0: none) and its header
struct PlacedProgram {
    const PackProgram *record = nullptr;
    uint32_t program = 0;
    uint64_t data = 0;
    const uint8_t *header = nullptr;    // its shader header, as the CPU holds it
    const uint16_t *farUbos = nullptr;  // its far uniform buffers (PackProgram::farUbos), null: none
};

// The pixel program of a state of none (stage_switches.h): it binds and reads nothing, its header all zeros
PlacedProgram noPixelProgram();

// What a state's split twin is made of: the pack's split (its programs by index) or a runtime record's programs, and the
// state's own inputs to Device::build
struct SplitSource {
    PackSplit rec = {};
    bool packed = false;
    // a runtime record's programs as heap slots: the per-pixel one laid out as the pack lays one (its PackProgram at heap
    // offset 0), the prologue's code with its shader data at prologueDataAt
    std::vector<uint8_t> fsSlot, prologueSlot;
    PackProgram fsInfo = {};
    uint32_t prologueDataAt = 0, prologueDataBytes = 0;
    const PackPipeline *pp = nullptr;
    const uint32_t *blob = nullptr, *far = nullptr;
    const char *keyString = nullptr;
    PlacedProgram vs;
    bool caTransform = false;
    // a runtime record's slots once placed (heap offsets, ~0: not yet), so a twin asked again after no room places neither
    // twice; under the device's lock
    mutable uint64_t fsPlaced = ~0ull, prologuePlaced = ~0ull;
};

struct RuntimeRecord;
struct CapturedProgram;

// A runtime record's program as a heap slot (Device::placeCompleted), placed once per program and constant buffer layout
struct CompletedSlot {
    std::string key;  // programSlotKey
    std::vector<uint8_t> bytes;
    PackProgram info = {};
    uint8_t header[80] = {};
    std::vector<uint16_t> farUbos;  // CapturedProgram::farUbos
};

// A runtime record's turned fragment variant (AssumeTurned), placed and built at its state's first turned draw
// (Device::makeTurned), and the state's own inputs to Device::build
struct TurnedSource {
    CompletedSlot fs;
    const PackPipeline *pp = nullptr;
    const uint32_t *blob = nullptr, *spills = nullptr, *far = nullptr;
    const char *keyString = nullptr;
    PlacedProgram vs, tess[2];
    bool tessellates = false, caTransform = false;
    Pipeline *turned = nullptr;  // where it is built
};

}  // namespace gm20b
