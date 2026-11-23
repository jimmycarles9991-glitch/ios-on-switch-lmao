// SPDX-License-Identifier: MIT
// A fetching state's fetch-free form (pack.h PipelineFetchFreeForm): the same state with its program writing the
// blend's source and the ROP blending it with the colour, in place of the storage form's fetch and in-shader blend inside
// the pixel interlock. Rung 2 blends with the form's own registers (its pipeline blend); rung 3 runs the iterated blend
// program its record names, the per-target blend off and the colour targets narrowed to the one it blends for its draws.
// A draw takes the form where its rule holds (fetch_rungs.h), else the storage form.
#pragma once
#include <cstdint>
#include <vector>
#include "../../kits/SwitchGM20BEmit/SwitchGM20BEmitIterated.h"
#include "fetch_rungs.h"
#include "pack.h"
#include "set_entries.h"
#include "push.h"

namespace gm20b {

namespace fetch_free {

constexpr uint32_t kBlendEnable = 0x1360;  // SET_BLEND(j)
constexpr uint32_t kPsSaturate = 0x13a8;   // SET_PS_SATURATE: OUTPUT<j> at bit 4j, its CLAMP_RANGE (0: 0 to 1) above it
constexpr uint32_t kOglFuncAdd = 0x8006, kOglOne = 0x4001, kOglZero = 0x4000;

template <typename Emit, typename... Args> void emit(std::vector<uint32_t> &out, Emit e, Args &&...args)
{
    uint32_t words[256];
    nv_push n;
    nv_push_init(&n, words, sizeof words / sizeof *words, SUBC_MASK_ALL);
    e(&n, static_cast<Args &&>(args)...);
    out.insert(out.end(), words, n.end);
}

}  // namespace fetch_free

// What a Pipeline made of a fetch-free form's record draws under
struct FetchFreeForm {
    uint8_t rung = FetchNone;  // FetchBlend or FetchIteratedBlend; FetchNone: not a fetch-free form
    uint8_t colour = 0;        // the colour it blends
    uint8_t programWords = 0, constants = 0;
    uint64_t program = 0;        // the iterated blend program's name (PackIteratedBlend::name)
    uint32_t sampledSlots = 0;   // the fragment texture slots its program samples
    bool samplesPast32 = false;  // it samples a fragment texture sampledSlots cannot name (24B119's slots 32 to 127)
    // a colour matrix form's: the fragment buffer the matrix is in, its bytes from the bind offset and its words there,
    // which each draw's macro call reads (kNoUniform: the program is the record's)
    uint8_t matrixSlot = kNoUniform;
    uint16_t matrixOffset = 0;
    uint8_t matrixWords = 0;
};

// The colour matrix macro's program at its longest (programs_long.py vcm.scale) and its constants; its uniform's five
// half4 rows, which the clamp pair follows where the build's QuartzCore has one (pack/fetchfree_link.py)
constexpr uint8_t kMatrixProgramWords = 38, kMatrixConstants = 8, kMatrixRowWords = 10, kMatrixWords = 11;

// The form a record describes; rung FetchNone where it is not one, or names an iterated program the pack lacks
// `far`: the record's far block (set_entries.h), null where it has kPackEntries entries or fewer
inline FetchFreeForm fetchFreeForm(const PackPipeline &pp, const PackIteratedBlend *iterated,
                                   const uint32_t *far = nullptr)
{
    FetchFreeForm f;
    if (!(pp.flags & PipelineFetchFreeForm))
        return f;
    for (uint8_t c = 0; c < 8; c++) {
        const uint8_t rung = pp.fetchRung[c] & kFetchRungMask;
        if (rung != FetchBlend && rung != FetchIteratedBlend)
            continue;
        const bool matrix = rung == FetchIteratedBlend && iterated && !iterated->count && iterated->uniformSlot < 31;
        if (rung == FetchIteratedBlend && !matrix &&
            (!iterated || !iterated->count || iterated->count > kIteratedProgramWords))
            return f;
        f.rung = rung;
        f.colour = c;
        const SetEntries entries(pp, far);
        for (uint32_t k = 0; k < entries.size(); k++) {
            const PackEntry &e = entries[k];
            if ((e.kind == EntryTexture || e.kind == EntryTexture3d) && handleStage(e) == 1) {
                if (e.slot < 32)
                    f.sampledSlots |= 1u << e.slot;
                else
                    f.samplesPast32 = true;
            }
        }
        f.program = rung == FetchIteratedBlend ? iterated->name : 0;
        f.programWords = rung != FetchIteratedBlend ? 0 : matrix ? kMatrixProgramWords : iterated->count;
        f.constants = rung != FetchIteratedBlend ? 0 : matrix ? kMatrixConstants : iterated->constants;
        if (matrix) {
            if (iterated->uniformWords < kMatrixRowWords || iterated->uniformWords > kMatrixWords)
                return FetchFreeForm{};
            f.matrixSlot = iterated->uniformSlot;
            f.matrixOffset = iterated->uniformOffset;
            f.matrixWords = iterated->uniformWords;
        }
        return f;
    }
    return f;
}

// A rung-3 form's iterated blend: `on` after its registers (the blend enabled on its colour, the program loaded and
// enabled; a colour matrix form's program is each draw's), `off` where the next state's registers go (iterated blend off,
// the per-target blend back as the context has it). `format` is the blended colour's MTLPixelFormat. Metal clamps the
// colour of an 8-bit unorm target to [0, 1] before the blend and leaves an RGBA16Float one's as it is; the iterated blend
// takes the shader's colour as it is, so an 8-bit target's form saturates the shader's output for the draw
// (SET_PS_SATURATE: device-proven to clamp, BENCH3 section 6)
inline void iteratedBlendWords(const PackIteratedBlend &b, uint8_t colour, uint32_t format, std::vector<uint32_t> &on,
                               std::vector<uint32_t> &off)
{
    uint16_t constants[kIteratedConstants * 3];
    for (uint32_t j = 0; j < kIteratedConstants; j++)
        for (uint32_t lane = 0; lane < 3; lane++)
            constants[3 * j + lane] = b.constant[j][lane];
    switch_gm20b_iterated_program program = {};
    program.program = b.program;
    program.count = b.count;
    program.alpha = b.enable & 2;
    program.common.color_op = fetch_free::kOglFuncAdd;
    program.common.color_source = fetch_free::kOglOne;
    program.common.color_dest = fetch_free::kOglZero;
    program.common.alpha_op = fetch_free::kOglFuncAdd;
    program.common.alpha_source = b.alphaSource;
    program.common.alpha_dest = b.alphaDest;
    program.constants = constants;
    program.constant_count = 3u * b.constants;
    on.push_back(Push::hdr(1, SUBC_3D, fetch_free::kBlendEnable + 4 * colour, 1));
    on.push_back(1);
    const bool saturate = format == 70 || format == 80;  // RGBA8Unorm, BGRA8Unorm
    if (saturate) {
        on.push_back(Push::hdr(1, SUBC_3D, fetch_free::kPsSaturate, 1));
        on.push_back(1u << (4 * colour));
    }
    if (b.count)
        fetch_free::emit(on, switch_gm20b_iterated_blend, &program);
    else
        fetch_free::emit(on, switch_gm20b_iterated_blend_common, &program.common);
    fetch_free::emit(off, switch_gm20b_iterated_blend_off);
    if (saturate) {
        off.push_back(Push::hdr(1, SUBC_3D, fetch_free::kPsSaturate, 1));
        off.push_back(0);
    }
}

// SET_CT_SELECT of `count` colour targets, each slot mapped to itself: a pass's, and one for a rung-3 form's draws
inline uint32_t targetSelect(uint32_t count)
{
    uint32_t word = count & 0xf;
    for (uint32_t j = 0; j < 8; j++)
        word |= j << (4 + 3 * j);
    return word;
}

// The draw's facts a form's rule reads
struct FetchFreeDraw {
    uint32_t format = 0;           // the blended colour's MTLPixelFormat in the pass
    bool readable = false;         // the target is CPU-visible memory the client may read as data
    uint8_t ropLayers = 0;         // ROP blends of rung-2 forms already over this target in the pass
    bool eightBitSources = false;  // every image the form samples holds unorm8 codes (eightBitSamples)
    bool accumulates = false;      // the pass loads the target over rung-2 blends of an earlier command buffer
};

// MTLPixelFormats of 8-bit unorm codes, read as they are stored (no sRGB decode)
constexpr bool unorm8Image(uint32_t format)
{
    return format == 1 || format == 10 || format == 30 || format == 70 || format == 80;
}

// Whether the textures bound at `slots` are all images of unorm8 codes: what blends is 8-bit content, texture samples of
// unorm8 images (x1-units UNITS.md's 8-bit sources), within one code of Metal at any depth; false where it samples none
template <class Texture> bool eightBitSamples(uint32_t slots, const Texture *const bound[])
{
    if (!slots)
        return false;
    for (; slots; slots &= slots - 1) {
        const Texture *t = bound[__builtin_ctz(slots)];
        if (!t || !unorm8Image(t->format))
            return false;
    }
    return true;
}

// Whether the form's program is worth its passes (kIteratedProven)
inline bool iteratedPassesProven(const FetchFreeForm &f)
{
    IteratedAsk ask;
    ask.candidate = f.rung == FetchIteratedBlend;
    ask.colourTargets = 1;
    ask.programWords = f.programWords;
    ask.constants = 3u * f.constants;
    ask.program = f.program;
    return iteratedBlendUsable(ask);
}

// Whether a pipeline places its state's form (its programs in the heap): one its rule can take, release and debug alike,
// or any while HW Test's probe channel asks (`probe`: the rung or the storage form it forces, 0 none)
inline bool fetchFreePlaced(const FetchFreeForm &f, uint8_t probe = 0)
{
    if (f.rung == FetchNone)
        return false;
    return (f.rung == FetchBlend ? fixedBlendProven() : iteratedPassesProven(f)) || probe;
}

// Whether a draw takes the form (rule fetch.blend or fetch.iterated); `forced`: a debug build's probe rung (2 or 3) that
// takes a form of that rung whatever its rule says, 4 none
inline bool fetchFreeTaken(const FetchFreeForm &f, const FetchFreeDraw &draw, uint8_t forced = 0)
{
    if (f.rung == FetchNone || forced == 4)
        return false;
    if (forced == f.rung)
        return true;
    if (f.rung == FetchBlend) {
        FixedBlendAsk ask;
        ask.format = uint16_t(draw.format);
        ask.readable = draw.readable;
        ask.standardFactors = true;
        ask.eightBitSources = draw.eightBitSources;
        ask.accumulates = draw.accumulates;
        ask.computedLayers = uint8_t(draw.ropLayers + 1);
        return fixedBlendAcceptable(ask);
    }
    return iteratedPassesProven(f);
}

}  // namespace gm20b
