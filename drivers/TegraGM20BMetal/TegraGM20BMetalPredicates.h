// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cstdint>

// ---- refusal diagnostics (r10-rawsweep): which predicate a command buffer hit, with its values ----
// Every refusal site names its predicate. A command buffer keeps the first one it hit, with its values. The first hit of each
// predicate in the process logs one line with its values (and, for a resource refused when made, the resource's details).
// Hot path: a relaxed load; strings only on a predicate's first hit.
enum SwitchMetalPred : uint8_t {
    PredNone = 0,
    PredTexForeign,  // a texture this device did not make
    // passes
    PredPassAttFormat,  // a color attachment not kept or not renderable: a = format, b = attachment index | kind << 8
    PredPassAttLevel,   // slice, depth plane or resolve, or a level of a texture with no chain
    PredPassDepthStencil,  // a = depth format, b = stencil format
    // states and calls
    PredStateForeign,
    PredCallVisibility,
    PredCallBufForeign,      // a = stage << 8 | index (0xffff: an index buffer)
    PredCallSamplerForeign,  // a sampler the driver does not express (its reason logged when it was made)
    PredCallBufCopyRange,
    PredCompute,           // a kernel's buffer, texture or sampler not made by this device
    PredComputeKernel,     // no pack kernel at the dispatch's shape, memory, textures and samplers
    PredComputeImage,      // a texture sampled through an image of its own: a = slot
    PredComputePredicate,  // predication raw cannot run (a misused record, a word outside a raw buffer): the command buffer fails; a = kind or offset
    PredEvent,
    // found while encoding (the raw encode ran up to here and was thrown away)
    PredEncPlanarClamp,
    PredEncPlanarRT,
    PredEncPassRender,
    PredEncKernel,
    PredEncIndexedRange,  // a = offset, b = index size
    PredEncColorFormats,  // a = attachment, b = pass format, c in the line: state format
    PredEncNoTic,
    PredEncDepthStencil,  // a = the state's depth format, b = the pass's | a state that tests without the attachment << 32
    PredEncSamples,       // a = the state's samples, b = the pass's | the state's shading passes << 32
    PredEncNoChannel,     // the kernel refused the channel or its context (logged with its answer)
    PredEncGeometry,      // a = primitive, b = count | instances << 32; no native raster-order plan
    PredEncStorageView,  // a = attachment, b = format | width << 32: no storage view and no barrier form to take
    PredPassRotated,     // a = gm20b::RotatedRefusal, b = the rotation (a pass's, or the state's at encode)
    PredPassPositions,   // sample positions Metal takes and draws nothing defined for: a = their count, b = the samples
    // calls Metal's validation stops on this device's own objects (c38-audit/failed-counts/probe/RESULTS.txt): a blit,
    // an indirect command buffer's encode, reset, copy or execute, a counter sample or its resolve
    PredCallBlit,
    PredCallIcb,
    PredCallCounter,  // a counter sample buffer of another device too
    PredEventTimeout,  // an untimed shared-event wait held its queue's head past Metal's bound: a = the value, b = reached
    PredCount
};

static_assert(PredCount <= 64, "one mask word");
static constexpr const char *kPredNames[] = {"-",
                                             "tex.foreign",
                                             "pass.fmt",
                                             "pass.level",
                                             "pass.ds",
                                             "state.foreign",
                                             "call.vis",
                                             "call.buf",
                                             "call.sampler",
                                             "call.bufcopy",
                                             "compute",
                                             "compute.nokernel",
                                             "compute.image",
                                             "compute.predicate",
                                             "event",
                                             "enc.planarclamp",
                                             "enc.planarrt",
                                             "enc.pass",
                                             "enc.kernel",
                                             "enc.idxrange",
                                             "enc.colorfmt",
                                             "enc.notic",
                                             "enc.ds",
                                             "enc.samples",
                                             "enc.nochannel",
                                             "enc.geometry",
                                             "enc.storage",
                                             "pass.rotated",
                                             "pass.positions",
                                             "call.blit",
                                             "call.icb",
                                             "call.counter",
                                             "event.timeout"};
static_assert(sizeof kPredNames / sizeof *kPredNames == PredCount, "a name for each predicate");

// The predicates whose command buffer fails, its work run nowhere, with MTLCommandBufferErrorDomain's code (0: not one):
// the kernel refusing a submission or the channel (plan/predicates.tsv cb-error). The client's errors (call.bufcopy,
// enc.idxrange, enc.colorfmt, enc.samples, pass.positions) complete as Apple's driver completes them
// (client_errors.h), skipped (gm20bSkipped), each rule logged once.
constexpr NSInteger predFailure(SwitchMetalPred p)
{
    switch (p) {
    case PredEncKernel:
    case PredEncNoChannel:
        return MTLCommandBufferErrorInternal;
    default:
        return 0;
    }
}

// A failed command buffer's predicate as its counter, its path and its line name it
constexpr const char *failedName(SwitchMetalPred p)
{
    return p ? kPredNames[p] : "none";
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// command buffers that completed with an error, by the predicate the failure that set it named (PredNone: none)
extern std::atomic<uint64_t> gPredFailed[PredCount];
// calls Metal's validation stops that the driver ran as Apple's driver runs them, by predicate (gm20bSkipped)
extern std::atomic<uint64_t> gPredSkipped[PredCount];
#endif
extern std::atomic<uint64_t> gPredLogged;

// `n` calls Metal's validation stops, run as Apple's driver runs them with validation off (nothing, or the part it
// runs: c38-audit/failed-counts/probe/RESULTS.txt): counted under `p` (switchMetalCounters skipped.<predicate>), never a
// failure. Each site logs its own rule once a process (a SWITCH_REFUSE_DETAIL keyed by the rule, or predSkipped()).
void gm20bSkipped(SwitchMetalPred p, uint64_t n = 1);
// ... a skip whose line is the predicate's: counted, and the process's first of each `rule` (a literal) logged with
// `a` and `b`
void predSkipped(SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule, uint64_t n = 1);

// A command buffer completed with `error`, failed by `p` (TegraGM20BMetalFinish.mm finished, its one funnel): counted, and the
// process's first of `p` logged (logFailure)
void commandBufferFailed(SwitchMetalPred p, NSError *error);
// The process's first failure of `p` (of each description, for PredNone): one error line, kept in release, naming the
// process, `p`, the error's code and its description (kits/SwitchRefusal, domain gm20b.failed)
void logFailure(SwitchMetalPred p, NSError *error);

static inline bool predLogged(SwitchMetalPred p)
{
    return gPredLogged.load(std::memory_order_relaxed) >> p & 1;
}

// The predicate's first hit in the process: one line with its values (`detail`: the resource's, when it was made)

void logPred(SwitchMetalPred p, uint64_t a, uint64_t b, NSString *detail);
