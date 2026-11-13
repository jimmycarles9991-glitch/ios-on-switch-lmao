// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#include <atomic>
#include <cstdint>

// ---- refusal diagnostics (r10-rawsweep): which predicate sends work to m2v, with its values ----
// Every refusal site names its predicate. A command buffer keeps a mask of every predicate it hit (not only the first, which
// decides its reason) and the first one's values. At submission each predicate hit is counted once per command buffer
// (gPredCbs), and once more when it was the command buffer's only one (gPredOnly: what fixing it alone would gain); the 5 s rates
// line shows both. The first hit of each predicate in the process logs one line with its values (and, for a resource refused
// when made, the resource's details). Hot path: an OR into the mask and a relaxed load; strings only on a predicate's first hit.
enum RMPred : uint8_t {
    PredNone = 0,
    // textures the raw path does not keep, decided when made (a bind hits the texture's own predicate)
    PredTexShape,  // rawShape: a = failing term (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format), b = format
    PredTexPoolAlloc,  // no pool span or texture header
    PredTexSurfPlane,  // a plane of a planar surface (not the 4:4:4 one): a = plane, b = plane count
    PredTexSurfPitch,  // bytesPerRow or plane offset & 15, or short: a = bytesPerRow, b = offset (when off 16) or width x bpp
    PredTexSurfBounds,  // offset + rows past the allocation
    PredTexSurfBase,    // base address off a page: a = base & 0xfff
    PredTexSurfImport,  // the import or the header slot refused
    PredTexPlanar,      // the 4:4:4 two-plane texture refused
    PredTexBufBase,     // (va + offset) & 255: a = that, b = offset
    PredTexBufPitch,    // bytesPerRow & 31, or short: a = bytesPerRow, b = width x bpp
    PredTexBufOther,    // memoryless, past the buffer, header slot
    PredTexView,  // a view of a texture the raw path does not keep, or of another texel size: a = format, b = parent raw
    PredTexForeign,  // not an RMTexture (m2v's own: over an m2v buffer, or a creation selector the facade does not answer)
    // passes
    PredPassAttFormat,  // a color attachment not kept or not renderable: a = format, b = attachment index | kind << 8
    PredPassAttLevel,   // slice, depth plane or resolve, or a level of a texture with no chain
    PredPassRTSize,     // a = renderTargetWidth << 16 | renderTargetHeight, b = attachment width << 16 | height
    PredPassDepthStencil,  // a = depth format, b = stencil format
    PredPassOther,  // a = renderTargetArrayLength (layered) | its own sample positions << 32, b = a rasterization rate map
    // states and calls
    PredStateForeign,
    PredStateNotPackable,  // a state whose functions are not this device's (neither in the pack nor made by the runtime)
    PredStateNotInPack,
    PredStateBeingMade,  // a state the pack lacks whose record the runtime is making (its first uses: RawComplete.h)
    PredCallVisibility,
    PredCallBufForeign,      // a = stage << 8 | index (0xffff: an index buffer)
    PredCallSamplerForeign,  // a sampler the raw path does not express (its reason logged when it was made)
    PredCallVertexSampler,
    PredCallDepthBias,     // a = bias, b = slope (float bits)
    PredCallInvocation,    // an encoder selector recorded as an invocation (each selector logged once)
    PredCallCBInvocation,  // a command buffer selector recorded as an invocation
    PredCallBlitOptions,
    PredCallBufCopyRange,
    PredBlitTexCopy,
    PredBlitBufToTex,
    PredBlitTexToBuf,
    PredBlitWholeTexCopy,
    PredBlitTexFill,       // a fillTexture: the raw path does not draw: a = pixel format, b = level or other format
    PredCompute,           // a kernel's buffer, texture or sampler not made by this device
    PredComputeOff16,      // a buffer off 16 bytes where the kernel's record names no residual word: a = slot
    PredComputeKernel,     // no pack kernel at the dispatch's shape, memory, textures and samplers
    PredComputeImage,      // a texture sampled through an image of its own: a = slot
    PredComputeView,       // a texture where m2v binds a view of another texel size: a = slot
    PredComputeEdge,       // dispatchThreads: of a grid with an edge threadgroup: a = the grid's edge
    PredComputePredicate,  // predication raw cannot run (a loop, a misused record, a word outside a raw buffer): the command buffer fails; a = kind or offset
    PredMipmaps,
    PredMipmapsFormat,  // a mipmapped texture's generateMipmaps in a format neither the 2D engine nor a mip state filters: a = format
    PredEvent,
    // found while encoding (the raw encode ran up to here and was thrown away)
    PredEncPlanarCopy,
    PredEncPlanarClamp,
    PredEncPlanarRT,
    PredEncPassRender,
    PredEncKernel,
    PredEncIndexedRange,  // a = offset, b = index size
    PredEncColorFormats,  // a = attachment, b = pass format, c in the line: state format
    PredEncUboOffset,     // a = stage << 8 | slot, b = va & 255
    PredEncNoTic,
    PredEncDepthStencil,  // a = the state's depth format, b = the pass's | a state that tests without the attachment << 32
    PredEncSamples,       // a = the state's samples, b = the pass's | the state's shading passes << 32
    PredEncNoChannel,     // the kernel refused the channel or its context (logged with its answer)
    PredEncGeometry,      // a = primitive, b = count | instances << 32; no native raster-order plan
    PredEncStorageView,  // a = attachment, b = format | width << 32: no storage view and no barrier form to take
    PredPassRotated,     // a = raw::RotatedRefusal, b = the rotation (a pass's, or the state's at encode)
    PredCount
};

static_assert(PredCount <= 64, "one mask word");
static constexpr const char *kPredNames[] = {"-",
                                             "tex.shape",
                                             "tex.pool",
                                             "tex.surf.plane",
                                             "tex.surf.pitch",
                                             "tex.surf.bounds",
                                             "tex.surf.base",
                                             "tex.surf.imp",
                                             "tex.planar",
                                             "tex.buf.base",
                                             "tex.buf.pitch",
                                             "tex.buf.other",
                                             "tex.view",
                                             "tex.foreign",
                                             "pass.fmt",
                                             "pass.level",
                                             "pass.rtsize",
                                             "pass.ds",
                                             "pass.other",
                                             "state.foreign",
                                             "state.notpack",
                                             "state.pack",
                                             "state.making",
                                             "call.vis",
                                             "call.buf",
                                             "call.sampler",
                                             "call.vsampler",
                                             "call.dbias",
                                             "call.inv",
                                             "call.cbinv",
                                             "call.blitopt",
                                             "call.bufcopy",
                                             "blit.tex",
                                             "blit.b2t",
                                             "blit.t2b",
                                             "blit.whole",
                                             "blit.fill",
                                             "compute",
                                             "compute.off16",
                                             "compute.nokernel",
                                             "compute.image",
                                             "compute.view",
                                             "compute.edge",
                                             "compute.predicate",
                                             "mipmaps",
                                             "mipmaps.fmt",
                                             "event",
                                             "enc.planarcopy",
                                             "enc.planarclamp",
                                             "enc.planarrt",
                                             "enc.pass",
                                             "enc.kernel",
                                             "enc.idxrange",
                                             "enc.colorfmt",
                                             "enc.ubo",
                                             "enc.notic",
                                             "enc.ds",
                                             "enc.samples",
                                             "enc.nochannel",
                                             "enc.geometry",
                                             "enc.storage",
                                             "pass.rotated"};
static_assert(sizeof kPredNames / sizeof *kPredNames == PredCount, "a name for each predicate");

// What makes a command buffer go to m2v (counted, one log line each kind)
enum RMReason : uint8_t {
    WhyRaw = 0,
    WhyState,
    WhyTexture,
    WhyPass,
    WhyCall,
    WhyCompute,
    WhyMipmaps,
    WhyEvent,
    WhyEncode,
    WhyCount
};

static const char *const kReasonNames[WhyCount] = {"raw",     "state",   "texture", "pass",  "call",
                                                   "compute", "mipmaps", "event",   "encode"};

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
extern std::atomic<uint64_t> gPredCbs[PredCount], gPredOnly[PredCount];
#endif
extern std::atomic<uint64_t> gPredLogged;

static inline bool predLogged(RMPred p)
{
    return gPredLogged.load(std::memory_order_relaxed) >> p & 1;
}

// The predicate's first hit in the process: one line with its values (`detail`: the resource's, when it was made)

void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail);
