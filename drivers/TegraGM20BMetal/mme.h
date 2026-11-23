// SPDX-License-Identifier: MIT
#pragma once
#include "../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h"
#include "SwitchGM20BMacros.h"
#include "push.h"
#include <iterator>

namespace gm20b::mme {

// Native GM20B macro ABI v6, at the slots kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h names (11..16 NVK's draw macros'
// slots, which the kernel's push profile counts as draws), and their contiguous instruction words from zero. Arguments:
// instance count (preloaded R1), first vertex/index, element count, then for the parameter macros the base instance (a
// draw of vertices) or the base vertex and base instance (an indexed one). The caller owns topology, restart and index
// binding. The plain macros change only BEGIN, vertex/index first+count and END; the parameter macros first write the
// root table's draw parameters and the base registers as the direct path wrote them. Past them the colour matrix
// constants, the indirect command buffers', the amplified draws' and a layered pass's clear. They are NVK's pre-Pascal
// draw loops, built with Mesa's MME builder (kits/SwitchGM20BEmit: gen/SwitchGM20BMacros.h).
constexpr uint32_t kDraw = SWITCH_GM20B_MACRO_DRAW, kDrawIndexed = SWITCH_GM20B_MACRO_DRAW_INDEXED,
                   kIndirect = SWITCH_GM20B_MACRO_INDIRECT, kIndexedIndirect = SWITCH_GM20B_MACRO_INDEXED_INDIRECT,
                   kDrawParams = SWITCH_GM20B_MACRO_DRAW_PARAMS,
                   kIndexedParams = SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS, kMatrix = SWITCH_GM20B_MACRO_VCM_CONSTANTS,
                   kIcbRange = SWITCH_GM20B_MACRO_ICB_RANGE, kIcbDraw = SWITCH_GM20B_MACRO_ICB_DRAW,
                   kIcbDrawIndexed = SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED, kIcbGate = SWITCH_GM20B_MACRO_ICB_GATE,
                   kIcbBind = SWITCH_GM20B_MACRO_ICB_BIND, kIcbPatches = SWITCH_GM20B_MACRO_ICB_PATCHES,
                   kAmplify = SWITCH_GM20B_MACRO_AMPLIFY, kAmplifyIndexed = SWITCH_GM20B_MACRO_AMPLIFY_INDEXED,
                   kClearLayers = SWITCH_GM20B_MACRO_CLEAR_LAYERS, kFetchDraw = SWITCH_GM20B_MACRO_FETCH_DRAW,
                   kFetchDrawIndexed = SWITCH_GM20B_MACRO_FETCH_DRAW_INDEXED;

constexpr bool countedAsDraw(uint32_t slot)
{
    return slot >= SWITCH_GM20B_MACRO_DRAWS_FIRST && slot <= SWITCH_GM20B_MACRO_DRAWS_LAST;
}

static_assert(countedAsDraw(kDraw) && countedAsDraw(kDrawIndexed) && countedAsDraw(kDrawParams) &&
                  countedAsDraw(kIndexedParams),
              "the kernel's push profile counts a draw macro's call as a draw");
static_assert(countedAsDraw(kIndirect) && countedAsDraw(kIndexedIndirect), "and an indirect draw's");
static_assert(!countedAsDraw(kMatrix) && !countedAsDraw(kIcbRange) && !countedAsDraw(kIcbDraw) &&
                  !countedAsDraw(kIcbDrawIndexed) && !countedAsDraw(kIcbGate) && !countedAsDraw(kIcbBind) &&
                  !countedAsDraw(kIcbPatches) && !countedAsDraw(kAmplify) && !countedAsDraw(kAmplifyIndexed) &&
                  !countedAsDraw(kClearLayers) && !countedAsDraw(kFetchDraw) && !countedAsDraw(kFetchDrawIndexed),
              "no other macro sits in the draw range; the push scan's metalMacro() counts ICB, patch, amplify and "
              "layered-clear calls by slot");
static_assert(kFetchDrawIndexed < SWITCH_GM20B_MACRO_METAL_END &&
                  SWITCH_GM20B_MACRO_METAL_END <= SWITCH_GM20B_MACRO_SLOTS,
              "every raw slot is one CALL_MME_MACRO takes");
static_assert(kDraw > SWITCH_GM20B_MACRO_NVK_SET_ANTI_ALIAS && kDraw > SWITCH_GM20B_MACRO_NVK_CLEAR,
              "no raw slot is one the profile reads as NVK's clear or anti-alias");
// An indirect command buffer's draws (icb.h) and dispatches (icb_compute.h): a draw passes its index before an
// indirect draw's arguments and draws only inside the range the shadow scratch words hold, which an execute writes (from the
// CPU's range, or by kIcbRange from the range buffer the GPU reads); a dispatch passes its index to kIcbGate, which lets the
// launches after it run only inside that range. An execute of dispatches that inherit the encoder's buffers writes them to
// scratch (three words a Metal buffer index from kScratchBuffers) and kIcbBind writes each command's set-0 descriptors
// and root table slot entries from them (icb_compute.h). A patch command passes its control-stage words to
// kIcbPatches, which loads them where the scratch words of its execute's layout (icbPatchLayout) say.
constexpr uint32_t kScratchLocation = SWITCH_GM20B_SCRATCH_ICB_LOCATION,
                   kScratchLength = SWITCH_GM20B_SCRATCH_ICB_LENGTH, kScratchBuffers = SWITCH_GM20B_SCRATCH_ICB_BUFFERS,
                   kScratchSetHi = SWITCH_GM20B_SCRATCH_ICB_SET_HI, kNoEntry = SWITCH_GM20B_ICB_NO_ENTRY,
                   kScratchSetBytes = SWITCH_GM20B_SCRATCH_ICB_SET_BYTES, kIcbRootBytes = SWITCH_GM20B_ICB_ROOT_BYTES;
constexpr uint32_t kMacroCall = NVB197_CALL_MME_MACRO(0), kRamPointer = NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER,
                   kRam = NVB197_LOAD_MME_INSTRUCTION_RAM, kStartPointer = NVB197_LOAD_MME_START_ADDRESS_RAM_POINTER,
                   kStart = NVB197_LOAD_MME_START_ADDRESS_RAM;

struct Program {
    const uint32_t *words;
    uint32_t size;
};

template <size_t N> constexpr Program program(const uint32_t (&words)[N])
{
    return {words, uint32_t(N)};
}

// Metal indirect arguments have exactly Vulkan's draw/indexed-draw word ordering. Unlike Turing, X1 reads them through a
// non-prefetched GPFIFO continuation, not an MME memory load. The CPU supplies no copy and never reads GPU-produced counts.
constexpr Program kDrawProgram = program(switch_gm20b_macro_draw),
                  kIndexedProgram = program(switch_gm20b_macro_draw_indexed),
                  kIndirectProgram = program(switch_gm20b_macro_indirect),
                  kIndexedIndirectProgram = program(switch_gm20b_macro_indexed_indirect),
                  kDrawParamsProgram = program(switch_gm20b_macro_draw_params),
                  kIndexedParamsProgram = program(switch_gm20b_macro_draw_indexed_params),
                  kMatrixProgram = program(switch_gm20b_macro_vcm_constants),
                  kIcbRangeProgram = program(switch_gm20b_macro_icb_range),
                  kIcbDrawProgram = program(switch_gm20b_macro_icb_draw),
                  kIcbDrawIndexedProgram = program(switch_gm20b_macro_icb_draw_indexed),
                  kIcbGateProgram = program(switch_gm20b_macro_icb_gate),
                  kIcbBindProgram = program(switch_gm20b_macro_icb_bind),
                  kIcbPatchesProgram = program(switch_gm20b_macro_icb_patches),
                  kAmplifyProgram = program(switch_gm20b_macro_amplify),
                  kAmplifyIndexedProgram = program(switch_gm20b_macro_amplify_indexed),
                  kClearLayersProgram = program(switch_gm20b_macro_clear_layers),
                  kFetchDrawProgram = program(switch_gm20b_macro_fetch_draw),
                  kFetchDrawIndexedProgram = program(switch_gm20b_macro_fetch_draw_indexed);
constexpr size_t kProgramWords =
    std::size(switch_gm20b_macro_draw) + std::size(switch_gm20b_macro_draw_indexed) +
    std::size(switch_gm20b_macro_indirect) + std::size(switch_gm20b_macro_indexed_indirect) +
    std::size(switch_gm20b_macro_draw_params) + std::size(switch_gm20b_macro_draw_indexed_params) +
    std::size(switch_gm20b_macro_vcm_constants) + std::size(switch_gm20b_macro_icb_range) +
    std::size(switch_gm20b_macro_icb_draw) + std::size(switch_gm20b_macro_icb_draw_indexed) +
    std::size(switch_gm20b_macro_icb_gate) + std::size(switch_gm20b_macro_icb_bind) +
    std::size(switch_gm20b_macro_icb_patches) + std::size(switch_gm20b_macro_amplify) +
    std::size(switch_gm20b_macro_amplify_indexed) + std::size(switch_gm20b_macro_clear_layers) +
    std::size(switch_gm20b_macro_fetch_draw) + std::size(switch_gm20b_macro_fetch_draw_indexed);
static_assert(kProgramWords <= SWITCH_GM20B_MACRO_CODE_WORDS, "install() loads every program from code word 0");
// each program's 5 load words, the zero page's 2, the root address's 3
constexpr size_t kContextWords = 18 * 5 + kProgramWords + 2 + 3;
constexpr size_t kCallWords = 4, kParamsCallWords = 6;

// Every program from code word 0, over NVK's, then the zero page's address (256-byte aligned below 2^40, >> 8) into the
// scratch word the bind macro reads it from, and the root table's address into the scratch words the patch macro
// selects it by (NVK's SELECT_CB0 reads the same words, which the context init wrote with the same address)
inline void install(Push &push, uint64_t root, uint64_t zero)
{
    uint32_t offset = 0;
    auto load = [&](uint32_t slot, const uint32_t *words, uint32_t count) {
        push.inc(kStartPointer, {slot, offset});
        push.m(kRamPointer, offset);
        push.nonInc(kRam, words, count);
        offset += count;
    };
    load(kDraw, kDrawProgram.words, kDrawProgram.size);
    load(kDrawIndexed, kIndexedProgram.words, kIndexedProgram.size);
    load(kIndirect, kIndirectProgram.words, kIndirectProgram.size);
    load(kIndexedIndirect, kIndexedIndirectProgram.words, kIndexedIndirectProgram.size);
    load(kDrawParams, kDrawParamsProgram.words, kDrawParamsProgram.size);
    load(kIndexedParams, kIndexedParamsProgram.words, kIndexedParamsProgram.size);
    load(kMatrix, kMatrixProgram.words, kMatrixProgram.size);
    load(kIcbRange, kIcbRangeProgram.words, kIcbRangeProgram.size);
    load(kIcbDraw, kIcbDrawProgram.words, kIcbDrawProgram.size);
    load(kIcbDrawIndexed, kIcbDrawIndexedProgram.words, kIcbDrawIndexedProgram.size);
    load(kIcbGate, kIcbGateProgram.words, kIcbGateProgram.size);
    load(kIcbBind, kIcbBindProgram.words, kIcbBindProgram.size);
    load(kIcbPatches, kIcbPatchesProgram.words, kIcbPatchesProgram.size);
    load(kAmplify, kAmplifyProgram.words, kAmplifyProgram.size);
    load(kAmplifyIndexed, kAmplifyIndexedProgram.words, kAmplifyIndexedProgram.size);
    load(kClearLayers, kClearLayersProgram.words, kClearLayersProgram.size);
    load(kFetchDraw, kFetchDrawProgram.words, kFetchDrawProgram.size);
    load(kFetchDrawIndexed, kFetchDrawIndexedProgram.words, kFetchDrawIndexedProgram.size);
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ZERO_PAGE), {uint32_t(zero >> 8)});
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ROOT_HI), {uint32_t(root >> 32), uint32_t(root)});
}

// The colour matrix macro's call: its arguments are the next GPFIFO entry's words (the uniform's)
inline void matrixHeader(Push &push)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kMatrix, SWITCH_GM20B_MACRO_VCM_ARGUMENTS);
}

inline void indirectHeader(Push &push, bool indexed)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kIndexedIndirect : kIndirect), indexed ? 5 : 4);
}

// A command's call: its index and then its arguments as data
inline void icbDrawHeader(Push &push, bool indexed)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kIcbDrawIndexed : kIcbDraw), indexed ? 6 : 5);
}

// A patch command's call: its index, patch count and instances, the descriptors and push words, the first patch and base
// instance as data
constexpr uint32_t kIcbPatchesArguments = 3 + 3 * 3 + 6 + 2;

inline void icbPatchesHeader(Push &push)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kIcbPatches, kIcbPatchesArguments);
}

// An execute's layout of the inherited tessellation state for its patch commands, as scratch words: set 0's address, each
// control-stage buffer's set-0 byte offset (kNoEntry: none), the root byte offset of the control stage's first push word
// and set 0's size, which the patch macro selects it at. The root table's address is install()'s.
inline void icbPatchLayout(Push &push, uint64_t set, uint32_t setBytes, const uint32_t (&entries)[3], uint32_t control)
{
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(kScratchSetHi),
             {uint32_t(set >> 32), uint32_t(set), entries[0], entries[1], entries[2], control});
    push.m(NVB197_SET_MME_SHADOW_SCRATCH(kScratchSetBytes), setBytes);
}

// A dispatch command's gate: its index is the call's one data word
inline void icbGateHeader(Push &push)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kIcbGate, 1);
}

// A command's descriptors, bind strides and root table entries to write from the inherited buffers in scratch: the
// call's data is the descriptors' count, seven words each, the strides' count, three words each, then the entries'
// count, three words each
inline void icbBindHeader(Push &push, uint32_t descriptors, uint32_t strides, uint32_t roots)
{
    *push.at++ =
        Push::hdr(5, SUBC_3D, kMacroCall + 8 * kIcbBind, SWITCH_GM20B_ICB_BIND_WORDS(descriptors, strides, roots));
}

// The range a CPU execute names, written as the scratch words themselves
inline void icbRange(Push &push, uint32_t location, uint32_t length)
{
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(kScratchLocation), {location, length});
}

// The range read from memory: the call's two data words are a GPFIFO continuation the caller appends
inline void icbRangeHeader(Push &push)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kIcbRange, 2);
}

// CLEAR_SURFACE's `payload` in each of `layers` layers from the payload's RT_ARRAY_INDEX on (a layered pass's clear)
inline void clearLayers(Push &push, uint32_t payload, uint32_t layers)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kClearLayers, 2);
    *push.at++ = payload;
    *push.at++ = layers;
}

inline void draw(Push &push, bool indexed, uint32_t first, uint32_t count, uint32_t instances)
{
    const uint32_t method = kMacroCall + 8 * (indexed ? kDrawIndexed : kDraw);
    // ONE_INC sends the first argument to CALL_MME_MACRO and all remaining arguments to its data method.
    *push.at++ = Push::hdr(5, SUBC_3D, method, 3);
    *push.at++ = instances;
    *push.at++ = first;
    *push.at++ = count;
}

// The draw parameters a channel holds: the root table's first word (a draw of vertices: its first vertex; an indexed
// draw: its base vertex) and base instance, and the base vertex registers. `unknown`: an indirect draw's macro set them.
struct DrawParams {
    uint32_t root = 0, baseInstance = 0, baseVertex = 0;
    bool unknown = false;
};

// A draw of `count` vertices or indices from `first`, `instances` times, as one macro call: the plain macro where the
// channel holds the draw's parameters, else the macro that writes them first
inline void draw(Push &push, DrawParams &held, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                 uint32_t root, uint32_t baseInstance)
{
    const uint32_t baseVertex = indexed ? root : 0;
    if (!held.unknown && held.root == root && held.baseInstance == baseInstance && held.baseVertex == baseVertex) {
        draw(push, indexed, first, count, instances);
        return;
    }
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kIndexedParams : kDrawParams), indexed ? 5 : 4);
    *push.at++ = instances;
    *push.at++ = first;
    *push.at++ = count;
    if (indexed)
        *push.at++ = baseVertex;
    *push.at++ = baseInstance;
    held = {root, baseInstance, baseVertex, false};
}

// The fetch macros' BEGIN words (SwitchGM20BMacroSlots.h FETCH_BEGIN, NEXT, CONTINUED), by BEGIN's SPLIT_MODE (30:29),
// INSTANCE_ID (27:26) and PRIMITIVE_ID (24:24) fields: an instance's first BEGIN opens its split draw, the first instance's
// resetting the instance ID and a later one's stepping it; every BEGIN after it in the instance continues the split, the
// primitive ID and the instance ID. The macro flips FETCH_BEGIN's bit to end one.
constexpr uint32_t kBeginSplit = 29, kBeginInstance = 26, kBeginPrimitive = 24;
constexpr uint32_t kFetchBeginFirst = NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_OPEN_END << kBeginSplit,
                   kFetchBeginNext = kFetchBeginFirst | NVB197_BEGIN_INSTANCE_ID_SUBSEQUENT << kBeginInstance,
                   kFetchBeginContinued = NVB197_BEGIN_SPLIT_MODE_OPEN_BEGIN_OPEN_END << kBeginSplit |
                                          NVB197_BEGIN_INSTANCE_ID_UNCHANGED << kBeginInstance |
                                          NVB197_BEGIN_PRIMITIVE_ID_UNCHANGED << kBeginPrimitive;
static_assert((NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_OPEN_END ^ NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_NORMAL_END) ==
                      NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_OPEN_END &&
                  (NVB197_BEGIN_SPLIT_MODE_OPEN_BEGIN_OPEN_END ^ NVB197_BEGIN_SPLIT_MODE_OPEN_BEGIN_NORMAL_END) ==
                      NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_OPEN_END,
              "flipping FETCH_BEGIN's bit ends an open split draw");
constexpr uint32_t kFetchShapeWords = 6, kFetchCallWords = 6;

// The scratch words a fetch macro's call of MTLPrimitiveType `primitive` reads: the vertices of its first primitive, the
// vertices that complete one more, the BEGIN words
inline void fetchShape(Push &push, uint32_t primitive)
{
    static constexpr uint32_t first[5] = {1, 2, 2, 3, 3}, step[5] = {1, 2, 1, 3, 1};
    const uint32_t k = primitive < 5 ? primitive : 3;
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_FIRST),
             {first[k], step[k], kFetchBeginFirst, kFetchBeginNext, kFetchBeginContinued});
}

// An indirect command buffer's execute of a state the fetch macros draw: its commands' draw macros draw a primitive at a
// time (`on`), each command's sizes from the topology it sets, until the execute is over (`on` false)
constexpr uint32_t kFetchCommandsWords = 7;

inline void fetchCommands(Push &push, bool on)
{
    if (on)
        push.inc(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_BEGIN),
                 {kFetchBeginFirst, kFetchBeginNext, kFetchBeginContinued, 1});
    else
        push.m(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_ICB), 0);
}

// A fetch macro's call: its words, the call's data or the GPFIFO continuation the caller appends
inline void fetchHeader(Push &push, bool indexed)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kFetchDrawIndexed : kFetchDraw), indexed ? 5 : 4);
}

inline void fetchDraw(Push &push, DrawParams &held, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                      uint32_t root, uint32_t baseInstance)
{
    fetchHeader(push, indexed);
    *push.at++ = count;
    *push.at++ = instances;
    *push.at++ = first;
    if (indexed)
        *push.at++ = root;
    *push.at++ = baseInstance;
    held = {root, baseInstance, indexed ? root : 0, false};
}

// Vertex amplification (amplification.cpp): a draw under it is one call of an amplify macro, whose words are in
// Vulkan's indirect order (count, instances, first, an indexed draw's vertex offset, the first instance): the call's
// data, or for an indirect draw the GPFIFO continuation the caller appends. It leaves the draw parameters it wrote, the
// AmplificationId word at the last amplification's id and the layer and viewport offset words at its offsets.
constexpr uint32_t kAmplifyCallWords = 6;
// a count and its mappings, and a state's words
constexpr uint32_t kAmplificationScratchWords = 2 + SWITCH_GM20B_AMP_MAX + 6;

inline void amplifyHeader(Push &push, bool indexed)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kAmplifyIndexed : kAmplify), indexed ? 5 : 4);
}

inline void amplify(Push &push, DrawParams &held, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                    uint32_t root, uint32_t baseInstance)
{
    amplifyHeader(push, indexed);
    *push.at++ = count;
    *push.at++ = instances;
    *push.at++ = first;
    if (indexed)
        *push.at++ = root;
    *push.at++ = baseInstance;
    held = {root, baseInstance, indexed ? root : 0, false};
}

// The amplify macros' scratch words: the count and each amplification's mapping word (its render target and viewport
// offsets), and the bound state's words (root byte offsets, 0: none), its layer (SWITCH_GM20B_AMP_LAYER_* or its word's
// offset) and whether two amplifications need a pixel barrier between them
// A view mapping's offset in its 16-bit half of a map word. An index at or past the pass's layers (2048 at most) or the
// encoder's viewports (16) draws into slice or viewport 0 (an M4 Max's, as m2v's ArrayLength and ViewportCount words
// do), so an offset past 0xFFFF draws as 0xFFFF does: alike unless the vertex function's own index wraps the 32-bit sum
// back into range, which only an offset within 2^16 of 2^32 can
constexpr uint32_t mappedOffset(uint32_t offset)
{
    return offset < 0xffff ? offset : 0xffff;
}

inline void amplification(Push &push, uint32_t count, const uint32_t (&layers)[SWITCH_GM20B_AMP_MAX],
                          const uint32_t (&viewports)[SWITCH_GM20B_AMP_MAX])
{
    uint32_t words[1 + SWITCH_GM20B_AMP_MAX] = {count};
    for (uint32_t a = 0; a < SWITCH_GM20B_AMP_MAX; a++)
        words[1 + a] = mappedOffset(layers[a]) | mappedOffset(viewports[a]) << SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT;
    push.incArray(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_COUNT), words, 1 + SWITCH_GM20B_AMP_MAX);
}

struct AmplificationWords {
    uint32_t count = 0, id = 0, viewport = 0, layer = SWITCH_GM20B_AMP_LAYER_FIRST, barrier = 0;

    bool same(const AmplificationWords &o) const
    {
        return count == o.count && id == o.id && viewport == o.viewport && layer == o.layer && barrier == o.barrier;
    }
};

inline void amplificationWords(Push &push, const AmplificationWords &w)
{
    push.inc(NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD),
             {w.count, w.id, w.viewport, w.layer, w.barrier});
}

}  // namespace gm20b::mme
