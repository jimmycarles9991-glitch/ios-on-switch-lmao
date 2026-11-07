// SPDX-License-Identifier: MIT
// See RawMetal.h. r7-raw wrote the first facade (render encoder, pipeline key); r8-rawca made it the render server's device: the
// m2v twins, the recorded command buffer and its replay on m2v, the order between the two drivers, the copies, the events,
// the lifetimes.
//
// A command buffer here is a tape: each encoder call appends a record (the bytes of setBytes copied in, every object retained by
// the command buffer until it completes). A call the raw path cannot draw exactly as m2v marks the command buffer for m2v, with a
// reason. At commit, under the device's submit lock and in commit order:
//  - raw: the tape is encoded into the raw rings (raw_gpu.cpp) and submitted, one Exec. The encoder can still refuse a draw (a
//    uniform buffer at an unaligned offset, a texture without a header); the raw work is then abandoned (the rings rewound)
//    and the command buffer goes to m2v. The completion thread sleeps in the kernel on each fence (the interrupt), then
//    runs the handlers.
//  - m2v: the tape is replayed into a command buffer of the m2v device with every object's twin, CPU lines of the buffers it
//    reads cleaned, and committed once the raw work committed before it has completed (raw's fence, a kernel wait on the
//    interrupt). Its completion handler runs ours.
//  - a raw command buffer waits for the m2v ones committed before it, and for the shared events it waits on: such a command
//    buffer, and every one committed after it while it waits, goes to the submit thread, which submits them in order once
//    they may run; the render thread never blocks for it. waitUntilScheduled waits for the submission, so the display's
//    copy (which waits on the GPU for every channel that imports the surface) never runs before the frame's work is submitted.
#import "RawMetal.h"
#import <IOSurface/IOSurfaceRef.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <os/log.h>
#include <pthread.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "SwitchFrameTimeline.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusalSite.h"
#include "RawAlias.h"
#include "raw_follow.h"
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusal.h"

// The m2v frontend's in-place textures over a buffer, of rows or the device's optimal layout (NativeBridgeMetalFrontend.mm):
// weak, so the host benchmark links without them.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options)
    __attribute__((weak_import));

#if SWITCH_FRAME_TIMELINE
static std::atomic<uint64_t> gSurfaceTextures{0},
    gTwinsMade{0};  // the frame timeline's churn counts (SwitchFrameTimeline.h)
#endif

static os_log_t rawLog()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    return l;
}

// A wait the plugin bounds (r9-hang: r7n's and r7o's freezes were a userland wait with no end in backboardd's render thread):
// past kWaitLimit, one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze, the userland twin of the kernel's missing-interrupt panic. Nothing waits that long when it works: within 10 s the kernel
// kills the channel a GPU wait is stuck on (or panics).
static constexpr auto kWaitLimit = std::chrono::seconds(20);

[[noreturn]] static void waitTooLong(const char *what, const void *object)
{
    os_log_fault(rawLog(), "raw: %{public}s %p not done after %lld s: abort() for the stack", what, object,
                 (long long)kWaitLimit.count());
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s %p not done after %lld s\n", what, object, (long long)kWaitLimit.count());
#endif
    abort();
}

// How long waitUntilScheduled waits before it asks whether a shared event holds the command buffer (waitUntilScheduled).
static constexpr auto kHeldGrace = std::chrono::milliseconds(500);

// One line per distinct text, then silence (shipped paths: no diagnostics but one-shot lines).
static void once(NSString *text)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> l(lock);
        if (seen.size() > 512 || !seen.insert(text.UTF8String).second)
            return;
    }
    os_log_error(rawLog(), "raw: %{public}@", text);
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s\n", text.UTF8String);  // host tests read their stderr
#endif
}

static double hostSeconds()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return double(mach_absolute_time()) * tb.numer / tb.denom / 1e9;
}

static uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

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
    PredPassOther,         // visibility buffer, layered target, rasterization rate map
    // states and calls
    PredStateForeign,
    PredStateNotPackable,  // a state whose functions are not of one framework's default.metallib (never looked up in the pack)
    PredStateNotInPack,
    PredCallVisibility,
    PredCallIndexedForm,     // a = instances, b = baseVertex | baseInstance << 32
    PredCallBufForeign,      // a = stage << 8 | index
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
    PredCompute,
    PredMipmaps,
    PredEvent,
    // found while encoding (the raw encode ran up to here and was thrown away)
    PredEncPlanarCopy,
    PredEncPlanarClamp,
    PredEncPlanarRT,
    PredEncPassRender,
    PredEncKernel,
    PredEncIndexedRange,  // a = offset, b = index size
    PredEncBaseInstance,
    PredEncColorFormats,  // a = attachment, b = pass format, c in the line: state format
    PredEncUboOffset,     // a = stage << 8 | slot, b = va & 255
    PredEncNoTic,
    PredEncDepthStencil,  // a = the state's depth format, b = the pass's | a state that tests without the attachment << 32
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
                                             "call.vis",
                                             "call.idxform",
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
                                             "compute",
                                             "mipmaps",
                                             "event",
                                             "enc.planarcopy",
                                             "enc.planarclamp",
                                             "enc.planarrt",
                                             "enc.pass",
                                             "enc.kernel",
                                             "enc.idxrange",
                                             "enc.baseinst",
                                             "enc.colorfmt",
                                             "enc.ubo",
                                             "enc.notic",
                                             "enc.ds"};
static_assert(sizeof kPredNames / sizeof *kPredNames == PredCount, "a name for each predicate");

// Each predicate's refusal key: domain "raw", the predicate's name as the reason (kits/SwitchRefusal)
static constexpr std::array<uint64_t, PredCount> predKeys()
{
    std::array<uint64_t, PredCount> keys{};
    for (int p = 0; p < PredCount; p++)
        keys[p] = switch_refusal_key("raw", kPredNames[p]);
    return keys;
}

static constexpr std::array<uint64_t, PredCount> kPredKeys = predKeys();
static std::atomic<uint64_t> gPredCbs[PredCount], gPredOnly[PredCount], gPredLogged{0};
static uint64_t gPredRates[PredCount];  // the rates line's last gPredCbs (its timer thread only)

static inline bool predLogged(RMPred p)
{
    return gPredLogged.load(std::memory_order_relaxed) >> p & 1;
}

// The predicate's first hit in the process: one line with its values (`detail`: the resource's, when it was made)
// Each predicate's first command buffer this process sends to m2v, through kits/SwitchRefusal (domain raw-metal, the
// predicate's name as the reason)
static void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail)
{
    const uint64_t bit = 1ull << p;
    if (gPredLogged.fetch_or(bit, std::memory_order_relaxed) & bit)
        return;
    switch_refusal_k(kPredKeys[p], "raw", kPredNames[p], "a=%llu (0x%llx) b=%llu (0x%llx)%s%s", (unsigned long long)a,
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)b, detail ? " " : "",
                     detail ? detail.UTF8String : "");
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: first %s: a=%llu b=%llu %s\n", kPredNames[p], (unsigned long long)a, (unsigned long long)b,
            detail ? detail.UTF8String : "");
#endif
}

// The type encoding of a selector some MTL* protocol declares (the plugin's answerUnimplemented uses the same lookup).
static const char *metalTypes(SEL selector)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *protocols = objc_copyProtocolList(&count);
    const char *types = nullptr;
    for (unsigned i = 0; i < count && !types; ++i) {
        if (strncmp(protocol_getName(protocols[i]), "MTL", 3) != 0)
            continue;
        for (int required = 1; required >= 0 && !types; --required) {
            const objc_method_description d = protocol_getMethodDescription(protocols[i], selector, required, YES);
            if (d.name)
                types = d.types;
        }
    }
    free(protocols);
    return types;
}

// A texture the raw path keeps: 2D, one level, one sample, a format it has a header for, not rotated (the descriptor's rotation SPI:
// Core Animation's window-server surfaces on a rotated display, CA::OGL::MetalContext::get_surface; m2v renders those rotated)
static bool rawShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned && raw::formatInfo(uint32_t(d.pixelFormat));
}

// A mipmapped texture the raw path keeps as a pool texture and a block-linear chain of its levels (raw::Device::createMipTexture,
// raw/fx): 2D, one sample, one layer, no rotation, of a format the 2D engine filters (not sRGB) and renders; chains on (the
// device's self-test). Private, memoryless (r7r: Core Animation makes 776x24 mipmapped surfaces memoryless too, kept here as
// private ones, a superset) or shared: CA::OGL::MetalContext::update_surface copies IOSurface content it draws with a trilinear
// minification filter into a texture of the surface texture's descriptor (shared) with every level, then generates them (on
// the 18:13 boot of 11-06, 49 command buffers of a 1280x720 one went to m2v). A shared one's CPU access to a later level reads
// and writes where the level is held (cpuLevel below).
static bool chainShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount > 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned &&
           (d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeMemoryless ||
            d.storageMode == MTLStorageModeShared) &&
           f && f->rt && !f->srgb && raw::fx::twodFormat(uint32_t(d.pixelFormat));
}

// A memoryless Depth32Float_Stencil8 texture, 2D, one level, one sample, no rotation: the raw path's zeta (raw_zeta.h). A stored
// one stays m2v's: its twin could not hold the raw path's texels (NVK's image is its own memory), and a command buffer m2v
// replays would read another depth than the one the raw path left.
static bool zetaDescriptor(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned && d.pixelFormat == MTLPixelFormat(raw::kZetaFormat) &&
           d.storageMode == MTLStorageModeMemoryless;
}

// Diagnostics: which rawShape term fails (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format; 0 none)
static uint32_t rawShapeTerm(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType != MTLTextureType2D           ? 1
           : d.mipmapLevelCount > 1                    ? 2
           : d.sampleCount > 1                         ? 3
           : (d.arrayLength > 1 || d.depth > 1)        ? 4
           : turned                                    ? 5
           : !raw::formatInfo(uint32_t(d.pixelFormat)) ? 6
                                                       : 0;
}

static NSString *describe(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return [NSString
        stringWithFormat:@"%lux%lu format %lu type %lu levels %lu samples %lu array %lu rotation %lu usage 0x%lx "
                         @"storage %lu",
                         (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.arrayLength, (unsigned long)turned, (unsigned long)d.usage,
                         (unsigned long)d.storageMode];
}
// A texture the raw path does not keep: its predicate, and the predicate's first line with the texture's details
@class RMTexture;
static NSString *
surfaceShape(RMTexture *t);  // a surface texture's pixel format, planes and YCbCr verdict (after RMTexture)
static void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra);

// MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes)
static size_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33:
        return 4;
    case 4:
    case 30:
    case 34:
        return 8;
    case 6:
    case 32:
    case 36:
        return 16;
    case 16:
    case 37:
    case 41:
        return 2;
    case 17:
    case 38:
    case 42:
        return 4;
    case 19:
    case 40:
    case 44:
        return 8;
    case 45:
    case 49:
    case 53:
        return 1;
    case 46:
    case 50:
    case 54:
        return 2;
    case 48:
    case 52:
    case 56:
        return 4;
    case 81:
    case 85:
        return 8;
    case 82:
    case 86:
        return 16;
    case 84:
    case 88:
        return 32;
    default:
        return 0;
    }
}

@protocol RMConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (RMPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

@class RMDevice, RMCommandBuffer;

// The process's one raw device (the render server's): its state. The device object Core Animation holds is an instance of a
// subclass of the m2v device's own class (raw_metal_device_open), so it is an _MTLDevice that conforms to Metal's protocols as
// the m2v device does and answers every selector the facade does not override with the m2v device's own method; it has no
// instance variables of its own (the base class's ABI is fixed), so its state lives here.
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

struct Stats {
    std::atomic<uint64_t> raw{0}, m2v{0}, deferred{0}, aborted{0}, encodeUs{0}, draws{0}, passes{0}, copies{0},
        reasons[WhyCount], switches{0}, cpuOrdered{0}, acquires{0}, eventsReleased{0}, heldReturns{0};
    // diagnostics (r10-rawsweep): the raw encode thrown away by aborted command buffers, the m2v replay's time on the committing
    // thread (replay + commit), and the bytes replay cleans
    std::atomic<uint64_t> abortUs{0}, replayUs{0}, replayClean{0};
    // command buffers submitted in segments (planSegments), and their segments on m2v
    std::atomic<uint64_t> splits{0}, splitM2v{0};
    // command buffers completed (finish): with the commits, what the stall line (rawStall) watches
    std::atomic<uint64_t> finished{0};
#if SWITCH_FRAME_TIMELINE
    // the m2v replay's parts (noteReplay)
    std::atomic<uint64_t> replaySlotUs{0}, replayTwinUs{0}, replayCommitUs{0}, replayMaxUs{0};
#endif
};

struct RawState {
    raw::Device *raw = nullptr;
    id<MTLDevice> fallback;                 // the m2v device
    id device;                              // the raw device object
    std::mutex submit;                      // one command buffer encodes and submits at a time, in commit order
    std::deque<RMCommandBuffer *> waiting;  // committed, not yet submitted
    std::condition_variable waitingChanged;
    std::mutex m2vLock;
    std::condition_variable m2vChanged;
    uint32_t m2vPending = 0;      // m2v command buffers committed and not completed
    uint32_t m2vUnscheduled = 0;  // and not scheduled yet: their kernel submission (NVK's Exec) is not made
    // The two drivers' order (submit lock): on the GPU when the kernel has Order (TegraGM20BInfoOrder), else on the CPU (the
    // m2v work before a raw command buffer completes first, and the raw work before an m2v one).
    bool gpuOrder = false;
    bool rawSinceM2v =
        false;  // raw work submitted since the last m2v commit: the next m2v one waits for it (BeforeTask)
    bool m2vSinceRaw =
        false;           // m2v work submitted since the last raw submission: the next raw one waits for it (AfterTask)
    int lastDriver = 0;  // 1 raw, 2 m2v: the last command buffer's, for the switch count
    NSMapTable *rawSignals,
        *m2vSignals;  // shared event -> highest value a submitted command buffer of each driver signals
    MTLSharedEventListener *listener;  // the CPU-ordered remainder's event waits
    std::mutex doneLock;
    std::condition_variable doneChanged;
    std::deque<RMCommandBuffer *> inflight;  // raw command buffers submitted, in seqno order
    dispatch_queue_t handlers;               // where scheduled handlers run (in order, never inside commit)
    Stats stats;
};

static RawState *gRM;
static uint64_t signalled(NSMapTable *map, void *event);  // the order section's (below)

// An m2v twin made outside its object's lock, kept if no other thread kept one first (theirs is returned, this one dropped). The
// object's lock is never held across a call into m2v: a thread inside m2v holding one of its locks (its handlers run Core
// Animation's, which may ask for a twin) and a thread here holding the object's lock asking m2v for one of its own would wait for
// each other for good (r7p8's freeze: a wait with no bound).
static id twinPublish(id object, __strong id &slot, id made)
{
    @synchronized(object) {
        if (!slot)
            slot = made;
        return slot;
    }
}

// ---- the common base: an m2v twin and the selectors this facade does not answer itself ----
@interface RMObject : NSObject {
@public
    __weak id _device;
    id _twin;  // the m2v object over the same memory (made on first need)
}
@property (nonatomic, copy) NSString *label;
- (id)rmTwin;  // made now if needed; nil if it cannot be
@end

@interface RMDevice : NSObject  // the methods the device class gets (raw_metal_device_open); no state of its own
- (void)rmCommit:(RMCommandBuffer *)cb;
- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled;
@end

@implementation RMObject

- (id)rmTwin
{
    return _twin;
}

- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}

// A selector this class does not answer goes to the twin (an m2v object: its answer is today's). respondsToSelector: says what the
// twin's class says, so Core Animation's optional paths choose as they do on m2v.
- (id)forwardingTargetForSelector:(SEL)selector
{
    id twin = [self rmTwin];
    if (!twin)
        SWITCH_REFUSE_DETAIL("raw", "twin.missing", uintptr_t(selector), "-[%s %s] with no m2v twin",
                             class_getName(object_getClass(self)), sel_getName(selector));
    return twin;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    if (_twin)
        return [_twin respondsToSelector:selector];
    Class c = [[self class] twinClass];
    return c ? [c instancesRespondToSelector:selector] : NO;
}

+ (Class)twinClass
{
    return nil;
}

@end

// ---- functions and libraries: the m2v library and functions, with the name and constants text the pack keys by ----
// The libraries the pack's states come from are frameworks' default.metallib (QuartzCore's, RenderBox's): a state of their
// functions is looked up by its key, which names the framework but QuartzCore's (raw_metal_pipeline_key)
@interface RMLibrary : RMObject {
@public
    std::string _framework;  // "QuartzCore", "RenderBox"...; empty: not a framework's default library (never looked up)
}
@end
@interface RMFunction : RMObject {
@public
    std::string _constants;
    std::string _framework;
}
@property (nonatomic, copy) NSString *name;
@end
@implementation RMFunction

+ (Class)twinClass
{
    return NSClassFromString(@"NBFunction");
}

- (MTLFunctionType)functionType
{
    return [(id<MTLFunction>)_twin functionType];
}

@end

static std::string constantsText(MTLFunctionConstantValues *values)
{
    std::string text;
    if (!values)
        return text;
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *groups[2] = {[snapshot newIndexedConstantArray], [snapshot newNamedConstantArray]};
    for (int g = 0; g < 2; g++)
        for (id<RMConstantValue> v in groups[g]) {
            const size_t n = constantBytes(v.dataType);
            char head[48];
            if (g)
                text += std::string(text.empty() ? "" : " ") + v.name.UTF8String;
            else {
                snprintf(head, sizeof head, "%s#%lu", text.empty() ? "" : " ", (unsigned long)v.index);
                text += head;
            }
            snprintf(head, sizeof head, ":%lu:", (unsigned long)v.dataType);
            text += head;
            for (size_t i = 0; i < n; i++) {
                snprintf(head, sizeof head, "%02x", ((const uint8_t *)v.data)[i]);
                text += head;
            }
        }
    return text;
}

@implementation RMLibrary

+ (Class)twinClass
{
    return NSClassFromString(@"NBLibrary");
}

- (RMFunction *)wrap:(id<MTLFunction>)twin name:(NSString *)name constants:(MTLFunctionConstantValues *)values
{
    if (!twin)
        return nil;
    RMFunction *f = [RMFunction new];
    f->_device = _device;
    f->_twin = twin;
    f->_constants = constantsText(values);
    f->_framework = _framework;
    f.name = name;
    return f;
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name] name:name constants:nil];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name
                                                                   constantValues:values
                                                                            error:error]
                                  name:name
                             constants:values];
}

- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    RMLibrary *me = self;
    MTLFunctionConstantValues *copy = [values copy];
    [(id<MTLLibrary>)_twin newFunctionWithName:name
                                constantValues:values
                             completionHandler:^(id<MTLFunction> f, NSError *e) {
                                 handler((id<MTLFunction>)[me wrap:f name:name constants:copy], e);
                             }];
}

- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithDescriptor:d error:error]
                                  name:d.name
                             constants:d.constantValues];
}

@end

static RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path)
{
    if (!twin)
        return nil;
    RMLibrary *l = [RMLibrary new];
    l->_device = device;
    l->_twin = twin;
    if ([path hasSuffix:@".framework/default.metallib"])
        l->_framework =
            path.stringByDeletingLastPathComponent.lastPathComponent.stringByDeletingPathExtension.UTF8String;
    return l;
}

NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *d)
{
    // the journal's R-line fields in its order (NativeBridgeMetalFrontend.mm journalRenderPipeline), but the label, each with a
    // blank after it, then the functions; no trailing blank (the pack's keys: pack/probelog.py)
    char head[160];
    snprintf(head, sizeof head, "samples=%lu topo=%lu ds=%lu/%lu ", (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string key = head;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        snprintf(head, sizeof head, "c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        key += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            snprintf(head, sizeof head, "a%u=%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, (unsigned long)l.stride,
                     (unsigned long)l.stepFunction, (unsigned long)l.stepRate);
            key += head;
        }
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    key += " | v " + std::string(vf ? vf.name.UTF8String : "-") + " " + (vf ? vf->_constants : "");
    key += " | f " + std::string(ff ? ff.name.UTF8String : "-") + " " + (ff ? ff->_constants : "");
    if (vf && vf->_framework != "QuartzCore" && !vf->_framework.empty())
        key += " | l " + vf->_framework;
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    return [NSString stringWithUTF8String:key.c_str()];
}

// ---- resources ----
@interface RMHeap : RMObject {
@public
    MTLHeapDescriptor *_descriptor;
    std::atomic<uint64_t> _used;
}
@end

@interface RMBuffer : RMObject {
@public
    raw::Buffer _raw;
    MTLResourceOptions _options;
    std::atomic<bool> _gpuDirty;  // the GPU wrote it since the CPU last read it
    void (^_deallocator)(void *, NSUInteger);
    RMHeap *_heap;
    MTLPurgeableState _purgeable;
}
@end
@interface RMBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row;
@end

@implementation RMBuffer

+ (Class)twinClass
{
    return NSClassFromString(@"NBBuffer");
}

- (void)dealloc
{
    _twin = nil;  // m2v's import over these pages goes first
    void *bytes = _raw.cpu;
    const NSUInteger length = NSUInteger(_raw.length);
    if (gRM)
        gRM->raw->destroyBuffer(_raw);
    if (_heap)
        _heap->_used -= length;
    if (_deallocator)
        _deallocator(bytes, length);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    // made outside this object's lock (twinPublish): in place over the same memory (a pool span or the caller's no-copy pages),
    // Shared whatever this buffer's storage mode (the memory is CPU-visible; m2v makes no Private buffer over host memory)
    const size_t page = size_t(getpagesize());
    const MTLResourceOptions options = (_options & ~MTLResourceStorageModeMask) | MTLResourceStorageModeShared;
    raw::Alias a;
    return twinPublish(self, _twin,
                       gRM->raw->alias(_raw, a)
                           ? rawAliasBuffer(a, _raw.cpu, (NSUInteger(_raw.length) + page - 1) & ~(page - 1), options)
                           : nil);
}

// A CPU read after a GPU write: the GPU's bytes, not lines the CPU cached before (clean+invalidate keeps any CPU write)
- (void)invalidateForCpu
{
    if (!_gpuDirty.exchange(false) || _raw.wc)
        return;  // (write-combined: the CPU reads memory itself)
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63); a < uintptr_t(_raw.cpu) + _raw.length; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

- (void *)contents
{
    [self invalidateForCpu];
    return _raw.cpu;
}

- (NSUInteger)length
{
    return NSUInteger(_raw.length);
}

- (NSUInteger)allocatedSize
{
    return NSUInteger(_raw.span.valid() ? _raw.span.bytes : _raw.length);
}

- (uint64_t)gpuAddress
{
    return _raw.va;
}

- (MTLResourceOptions)resourceOptions
{
    return _options;
}

- (MTLStorageMode)storageMode
{
    return MTLStorageMode((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheMode((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return MTLHazardTrackingMode((_options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift);
}

- (void)didModifyRange:(NSRange)range
{
    (void)range;  // the draws that read it clean what they read (raw_gpu.cpp CommandBuffer::clean)
}

// The pages are never given back while the buffer lives: Volatile and Empty keep the contents (a later NonVolatile answers what
// was asked before, never Empty), which is what a caller that refills on Empty needs.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}

- (id<MTLHeap>)heap
{
    return (id<MTLHeap>)_heap;
}

- (void)makeAliasable
{}

- (BOOL)isAliasable
{
    return NO;
}

- (IOSurfaceRef)iosurface
{
    return nullptr;
}

@end

typedef NS_ENUM(uint8_t, RMTextureKind) {
    RMTexturePool,
    RMTextureSurface,
    RMTextureBuffer,
    RMTextureView,
    RMTextureMemoryless,
    RMTextureTwinOnly
};

@interface RMTexture : RMObject {
@public
    raw::Texture _raw;
    bool _rawOk;  // the raw path can draw with it (else every command buffer using it goes to m2v)
    RMTextureKind _kind;
    std::atomic<bool> _gpuWrites;  // its surface was declared GPU-written (surfaceGpuWrites)
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _surface;
    NSUInteger _plane;
    // a planar texture's image (4:2:0 converted, 4:4:4 codes) holds its surface at this seed (_imageCurrent): converted
    // again only once the surface's seed moves (r8a: a YouTube page's thumbnails converted again at every command buffer)
    uint32_t _imageSeed;
    bool _imageCurrent;
    RMTexture *_parent;  // a view's
    RMBuffer *_buffer;   // a texture over a buffer's
    // A mipmapped texture's (raw::Texture::chain): _twin is m2v's in-place texture over level 0 (one level: where m2v renders and
    // copies level 0), _mipTwin m2v's own texture of every level (where m2v samples, copies other levels and generates), kept
    // as the chain holds them by the replay (syncMipTwin): _mipL0 its level 0 holds level 0, _mipLevels its later levels the last
    // generateMipmaps (the submit lock: the GPU's order)
    id _mipTwin;
    bool _mipL0, _mipLevels;
    // Per later level (index = level), a pool texture of the level's size (rmMirror). Neither driver can read the other's
    // copy of the later levels (the chain is the raw path's block-linear memory, _mipTwin m2v's own image), so a level one of
    // them rendered reaches the other through its mirror: the raw path copies its chain's levels in before an m2v command
    // buffer needs them (mirrorsForM2v); m2v copies a level it rendered in, which the raw path copies into the chain before it
    // next reads the chain (raw::Texture::Chain::mirrorLevels).
    NSMutableArray *_mirrors;
    bool
        _mirrored;  // the mirrors hold the chain's later levels, for the m2v command buffer being replayed (mirrorsForM2v)
    NSUInteger _offset, _rowBytes;
    std::atomic<bool> _gpuDirty;
    MTLPurgeableState _purgeable;
    uint8_t _pred;  // why the raw path does not keep it (RMPred; 0: it does)
    // A shared mipmapped texture's CPU writes the chain's states do not show yet (foldCpuLevels, at the next submission, under
    // the submit lock): bit l, level l written where the raw path holds it (level 0: its pages, later: the chain); bit 16 + l,
    // later level l written into its mirror (m2v held it)
    std::atomic<uint32_t> _cpuLevels;
}
@end

// Before the raw path first renders into a surface texture: the kernel takes the surface's imports as only sampled until
// then, so a CPU lock of a surface the GPU never writes invalidates nothing.
static void declareGpuWrites(RMTexture *t)
{
    if (!t->_surface || t->_gpuWrites.exchange(true))
        return;
    if (gRM && gRM->raw && gRM->raw->surfaceGpuWrites(IOSurfaceGetID(t->_surface)))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log_error(rawLog(),
                     "raw: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(t->_surface));
}

// A shared mipmapped texture's CPU writes reach its chain's states (raw::Texture::Chain, _mipL0/_mipLevels: the submit lock's) at
// the next submission, before it encodes: Metal leaves a CPU access racing GPU work that uses the texture undefined, so every
// command buffer committed before the write has completed and every later one encodes after the fold.
static std::mutex gCpuLevelsLock;
static std::vector<RMTexture *> gCpuLevelsPending;
static std::atomic<bool> gCpuLevelsAny{false};

static void noteCpuLevels(RMTexture *t, uint32_t bits);

#if SWITCH_FRAME_TIMELINE
// A YCbCr or planar surface's image, made from the surface's planes by GPU work at the seed it had then: when its texture
// is made and at each conversion, with the seed before (none: never converted). The kernel's IOSurface lines name the
// CPU locks and writing unlocks of the same surface ID around it; a seed that moves with no unlock there is a writer the
// locks never see (a hardware engine, or a CPU write without a lock).
static void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed)
{
    if (!t->_surface)
        return;
    const OSType f = IOSurfaceGetPixelFormat(t->_surface);
    os_log(rawLog(), "raw: IOSurface %u %ux%u '%c%c%c%c' image %s at seed %u (before: %s%u)",
           IOSurfaceGetID(t->_surface), unsigned(IOSurfaceGetWidth(t->_surface)),
           unsigned(IOSurfaceGetHeight(t->_surface)), char(f >> 24), char(f >> 16), char(f >> 8), char(f), what, seed,
           t->_imageCurrent ? "" : "none ", t->_imageCurrent ? t->_imageSeed : 0u);
}
#endif
static void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write);
static void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);

@implementation RMTexture

+ (Class)twinClass
{
    return NSClassFromString(@"NBTexture");
}

// SPI: the descriptor's (0 for every texture the raw path keeps, rawShape)
- (NSUInteger)rotation
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [_descriptor respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(_descriptor, rotation)
                                                     : 0;
}

- (void)dealloc
{
    _twin = nil;
    if (_rawOk)
        if (gRM)
            gRM->raw->destroyTexture(_raw);
    if (_surface)
        CFRelease(_surface);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    id<MTLDevice> m2v = gRM->fallback;
    id made = nil;  // outside this object's lock (twinPublish)
    {
        switch (_kind) {
        case RMTextureSurface:
            made = [m2v newTextureWithDescriptor:_descriptor iosurface:_surface plane:_plane];
            break;
        case RMTexturePool: {  // m2v's in-place texture over the same pool memory (as an IOSurface's)
            const size_t page = size_t(getpagesize());
            raw::Alias a;
            id<MTLBuffer> pages =
                gRM->raw->alias(_raw, a)
                    ? rawAliasBuffer(a, _raw.span.cpu, (NSUInteger(_raw.span.bytes) + page - 1) & ~(page - 1),
                                     MTLResourceStorageModeShared)
                    : nil;
            MTLTextureDescriptor *d = _descriptor;
            if (_raw.chain) {  // level 0 (the chain's levels are the m2v texture of every level's: rmMipTwin)
                d = [_descriptor copy];
                d.mipmapLevelCount = 1;
                if (d.storageMode == MTLStorageModeMemoryless)
                    d.storageMode = MTLStorageModePrivate;  // (memory here)
            }
            if (pages && _raw.blockLinear && native_bridge_metal_texture_tiled_in_place)
                made = native_bridge_metal_texture_tiled_in_place(pages, d, 0);
            else if (pages && !_raw.blockLinear && native_bridge_metal_texture_in_place)
                made = native_bridge_metal_texture_in_place(pages, d, 0, _raw.pitch);
            break;
        }
        case RMTextureBuffer:
            if (id<MTLBuffer> b = [_buffer rmTwin]) {
                made = native_bridge_metal_texture_in_place
                           ? native_bridge_metal_texture_in_place(b, _descriptor, _offset, _rowBytes)
                           : [b newTextureWithDescriptor:_descriptor offset:_offset bytesPerRow:_rowBytes];
            }
            break;
        case RMTextureView:
            made = [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat];
            break;
        case RMTextureMemoryless:
            made = [m2v newTextureWithDescriptor:_descriptor];
            break;
        case RMTextureTwinOnly:
            break;
        }
    }
    id twin = twinPublish(self, _twin, made);
    if (!twin)
        SWITCH_REFUSE_DETAIL("raw", "twin.texture", _descriptor.pixelFormat,
                             "no m2v twin for a %ux%u texture of format %lu (kind %u)", _raw.width, _raw.height,
                             (unsigned long)_descriptor.pixelFormat, unsigned(_kind));
    return twin;
}

// A mipmapped texture's m2v texture of every level (made on first need)
- (id)rmMipTwin
{
    @synchronized(self) {
        if (_mipTwin)
            return _mipTwin;
    }
    return twinPublish(self, _mipTwin, [gRM->fallback newTextureWithDescriptor:_descriptor]);
}

// Level `level`'s mirror (_mirrors): a pitch pool texture of the level's size and format, with its m2v in-place twin. nil:
// none, and the levels cross between the drivers by being generated from level 0.
- (RMTexture *)rmMirror:(NSUInteger)level
{
    if (!_raw.chain || !level || level >= _raw.chain->layout.levels)
        return nil;
    @synchronized(self) {
        if (_mirrors && level < _mirrors.count && [_mirrors[level] isKindOfClass:[RMTexture class]])
            return _mirrors[level];
    }
    const raw::fx::MipLevel &l = _raw.chain->layout.level[level];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_descriptor.pixelFormat
                                                                                 width:l.width
                                                                                height:l.height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id made = [(RMDevice *)_device rmNewTexture:d tiled:false];  // (rows: the 2D engine's copies of a level are pitch)
    RMTexture *m = [made isKindOfClass:[RMTexture class]] ? (RMTexture *)made : nil;
    if (!m || !m->_rawOk || m->_raw.memoryless || ((m->_raw.va | m->_raw.pitch) & 127) || ![m rmTwin]) {
        once([NSString
            stringWithFormat:@"no mirror for level %lu (%ux%u format %lu) of a mipmapped texture: its levels "
                             @"cross between the drivers by generating them from level 0",
                             (unsigned long)level, l.width, l.height, (unsigned long)_descriptor.pixelFormat]);
        return nil;
    }
    @synchronized(self) {
        if (!_mirrors)
            _mirrors = [NSMutableArray array];
        while (_mirrors.count <= level)
            [_mirrors addObject:[NSNull null]];
        if ([_mirrors[level] isKindOfClass:[RMTexture class]])
            return _mirrors[level];
        _mirrors[level] = m;
    }
    return m;
}

- (MTLTextureType)textureType
{
    return _descriptor.textureType;
}

- (MTLPixelFormat)pixelFormat
{
    return _descriptor.pixelFormat;
}

- (NSUInteger)width
{
    return _descriptor.width;
}

- (NSUInteger)height
{
    return _descriptor.height;
}

- (NSUInteger)depth
{
    return _descriptor.depth;
}

- (NSUInteger)mipmapLevelCount
{
    return _descriptor.mipmapLevelCount;
}

- (NSUInteger)arrayLength
{
    return _descriptor.arrayLength;
}

- (NSUInteger)sampleCount
{
    return _descriptor.sampleCount;
}

- (MTLTextureUsage)usage
{
    return _descriptor.usage;
}

- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}

- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}

- (BOOL)isFramebufferOnly
{
    return NO;
}

- (BOOL)allowGPUOptimizedContents
{
    return _descriptor.allowGPUOptimizedContents;
}

- (IOSurfaceRef)iosurface
{
    return _surface;
}

- (NSUInteger)iosurfacePlane
{
    return _plane;
}

- (id<MTLTexture>)parentTexture
{
    return (id<MTLTexture>)_parent;
}

- (id<MTLBuffer>)buffer
{
    return (id<MTLBuffer>)_buffer;
}

- (NSUInteger)bufferOffset
{
    return _offset;
}

- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}

- (NSUInteger)parentRelativeLevel
{
    return 0;
}

- (NSUInteger)parentRelativeSlice
{
    return 0;
}

- (BOOL)isShareable
{
    return NO;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}

- (void)makeAliasable
{}

- (BOOL)isAliasable
{
    return NO;
}

- (id<MTLHeap>)heap
{
    return nil;
}

// CPU writes into the texture's own pages (level 0, slice 0, a 2D texture), cleaned to memory at once: a later command buffer's
// GPU reads see them. A shared mipmapped texture's later levels: cpuLevel. Other textures are m2v's (their twin's replaceRegion).
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    if (_rawOk && _raw.chain && level && !slice && r.size.depth <= 1) {
        cpuLevel(self, r, level, (uint8_t *)bytes, row, true);
        return;
    }
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] replaceRegion:r
                                         mipmapLevel:level
                                               slice:slice
                                           withBytes:bytes
                                         bytesPerRow:row
                                       bytesPerImage:image];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, true);
        return;
    }
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *to = _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp;
        memcpy(to, (const uint8_t *)bytes + y * row, line);
#if defined(__aarch64__)
        if (!_raw.wc)  // (write-combined: in memory once the DSB below drains it)
            for (uintptr_t a = uintptr_t(to) & ~uintptr_t(63); a < uintptr_t(to) + line; a += 64)
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    if (_raw.chain)
        noteCpuLevels(self, 1);
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}

// CPU reads of what the GPU wrote: its lines invalidated first (once per GPU write), then the rows
- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    if (_rawOk && _raw.chain && level && !slice && r.size.depth <= 1) {
        cpuLevel(self, r, level, (uint8_t *)bytes, row, false);
        return;
    }
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] getBytes:bytes
                                    bytesPerRow:row
                                  bytesPerImage:image
                                     fromRegion:r
                                    mipmapLevel:level
                                          slice:slice];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, false);
        return;
    }
#if defined(__aarch64__)
    if (_gpuDirty.exchange(false) && !_raw.wc) {
        for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63);
             a < uintptr_t(_raw.cpu) + uint64_t(_raw.pitch) * _raw.height; a += 64)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)bytes + y * row, _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp, line);
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}

// A view: the same memory under another format's header (raw), or m2v's view of the twin
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_kind = RMTextureView;
    v->_parent = self;
    if (_rawOk && !_raw.memoryless && raw::formatInfo(uint32_t(format)))
        v->_rawOk = gRM->raw->createView(_raw, uint32_t(format), v->_raw);
    if (_raw.chain) {  // a view of a mipmapped texture: m2v's view of its texture of every level (the replay keeps that current)
        v->_twin = [(id<MTLTexture>)[self rmMipTwin] newTextureViewWithPixelFormat:format];
        refuseTexture(v, PredTexView, format, 2, @"view", @"of a mipmapped texture");
        return v->_twin ? (id<MTLTexture>)v : nil;
    }
    if (!v->_rawOk)
        refuseTexture(v, PredTexView, format, _rawOk, @"view",
                      [NSString stringWithFormat:@"parent kind %u pred %s", unsigned(_kind), kPredNames[_pred]]);
    if (!v->_rawOk && ![v rmTwin])
        return nil;
    return (id<MTLTexture>)v;
}

@end

static void noteCpuLevels(RMTexture *t, uint32_t bits)
{
    if (t->_cpuLevels.fetch_or(bits, std::memory_order_acq_rel))
        return;
    std::lock_guard<std::mutex> l(gCpuLevelsLock);
    gCpuLevelsPending.push_back(t);
    gCpuLevelsAny.store(true, std::memory_order_release);
}

// The submit lock held: the pending CPU writes into the chains' states. A later level the CPU wrote into the chain is the
// chain's own (rawLevels: m2v copies it in, never generates it), newer than its mirror, and makes the chain's levels the ones
// held when nothing generated them before (m2v held none: cpuLevel wrote into the chain only then).
static void foldCpuLevels()
{
    if (!gCpuLevelsAny.load(std::memory_order_acquire))
        return;
    std::vector<RMTexture *> list;
    {
        std::lock_guard<std::mutex> l(gCpuLevelsLock);
        list.swap(gCpuLevelsPending);
        gCpuLevelsAny.store(false, std::memory_order_relaxed);
    }
    for (RMTexture *t : list) {
        const uint32_t m = t->_cpuLevels.exchange(0, std::memory_order_acq_rel);
        raw::Texture::Chain &c = *t->_raw.chain;
        if (m & 1) {
            c.l0Fresh = false;
            t->_mipL0 = false;
        }
        if (const uint16_t own = uint16_t(m & 0xfffe)) {
            c.levelsFresh = true;
            c.rawLevels |= own;
            c.mirrorLevels &= uint16_t(~own);
            t->_mipLevels = false;
        }
        c.mirrorLevels |= uint16_t(m >> 16);
    }
}

static void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(from) & ~uintptr_t(63); a < uintptr_t(from) + bytes; a += 64)
        if (invalidate)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)from;
    (void)bytes;
    (void)invalidate;
#endif
}

// replaceRegion/getBytes of later level `level` of a shared mipmapped texture, where the level is held (the states as the work
// committed before left them, and the CPU writes since): m2v's texture of every level after m2v generated them (a write also goes
// to the level's mirror, whole, so the raw path takes it in after generating: Chain::mirrorLevels); the mirror after m2v wrote
// the level (and m2v's texture); else the chain, in the CPU's view the device's self-test chose.
// A block-linear texture's CPU copies (replaceRegion, getBytes of level 0) through TegraGM20BLayout.h: the block rows they
// reach are invalidated before a read of what the GPU wrote, cleaned after a write
static void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    tgm_layout l;
    if (!r.size.width || !r.size.height || !tgm_layout_tiled(t->_raw.width, t->_raw.height, t->_raw.bpp, &l))
        return;
    const uint64_t blockRow = uint64_t(l.row_bytes) * (8u << l.y_log2);
    const uint64_t from = r.origin.y / (8u << l.y_log2) * blockRow,
                   to = std::min<uint64_t>((r.origin.y + r.size.height + (8u << l.y_log2) - 1) / (8u << l.y_log2) *
                                               blockRow,
                                           uint64_t(l.row_bytes) * l.rows);
    if (write) {
        tgm_layout_write(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                         uint32_t(r.size.height), bytes, row);
        cacheLines(t->_raw.cpu + from, to - from, false);
        return;
    }
    if (t->_gpuDirty.exchange(false))
        cacheLines(t->_raw.cpu, l.row_bytes * uint64_t(l.rows), true);
    tgm_layout_read(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                    uint32_t(r.size.height), bytes, row);
}

static void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture::Chain &c = *t->_raw.chain;
    if (level >= c.layout.levels)
        return;
    const raw::fx::MipLevel &lv = c.layout.level[level];
    if (r.origin.x + r.size.width > lv.width || r.origin.y + r.size.height > lv.height)
        return;
    const uint32_t bit = 1u << level, pending = t->_cpuLevels.load(std::memory_order_acquire);
    id<MTLTexture> all = t->_mipTwin;
    const bool m2v = !(pending & bit) && !(pending >> 16 & bit) && !c.levelsFresh && all;
    RMTexture *mirror = nil;
    if (m2v || (pending >> 16 & bit) || (!(pending & bit) && (c.mirrorLevels & bit)))
        mirror = [t rmMirror:level];
    if (m2v || mirror) {
        if (write && all)
            [all replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
        if (!mirror) {
            if (write)
                once(@"a CPU write to a level of a mipmapped texture m2v holds has no mirror: the raw path's copy of "
                     @"the "
                     @"level stays generated from level 0");
            else
                [all getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
            return;
        }
        const raw::Texture &m = mirror->_raw;
        const uint64_t span = uint64_t(m.pitch) * lv.height;
        if (m2v && write) {
            [all getBytes:m.cpu
                  bytesPerRow:m.pitch
                bytesPerImage:0
                   fromRegion:MTLRegionMake2D(0, 0, lv.width, lv.height)
                  mipmapLevel:level
                        slice:0];
        } else if (m2v) {
            [all getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
            return;
        } else {
            if (!write)
                cacheLines(m.cpu, span, true);
            const size_t line = r.size.width * m.bpp;
            for (NSUInteger y = 0; y < r.size.height; y++) {
                uint8_t *at = m.cpu + (r.origin.y + y) * m.pitch + r.origin.x * m.bpp;
                if (write)
                    memcpy(at, bytes + y * row, line);
                else
                    memcpy(bytes + y * row, at, line);
            }
        }
        if (write) {
            cacheLines(m.cpu, span, false);
            noteCpuLevels(t, bit << 16);
        }
        return;
    }
    uint8_t *chain = c.span.cpu;
    const bool fermi = gRM->raw->selfTestFermiView;
    const uint64_t at = lv.offset, span = uint64_t(lv.rowBytes) * lv.rows;
    if (write) {
        raw::fx::writeLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                            uint32_t(r.size.width), uint32_t(r.size.height), bytes, row, fermi);
        cacheLines(chain + at, span, false);
        noteCpuLevels(t, bit);
    } else {
        cacheLines(chain + at, span, true);
        raw::fx::readLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                           uint32_t(r.size.width), uint32_t(r.size.height), bytes, row, fermi);
    }
}

static const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out);

static NSString *surfaceShape(RMTexture *t)
{
    IOSurfaceRef s = t->_surface;
    if (!s)
        return @"";
    const OSType fourcc = IOSurfaceGetPixelFormat(s);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    NSMutableString *out =
        [NSMutableString stringWithFormat:@" surface '%c%c%c%c' %zux%zu planes %zu plane %lu", char(fourcc >> 24),
                                          char(fourcc >> 16), char(fourcc >> 8), char(fourcc), IOSurfaceGetWidth(s),
                                          IOSurfaceGetHeight(s), planes, (unsigned long)t->_plane];
    for (size_t k = 0; k < planes && k < 3; k++)
        [out appendFormat:@" p%zu %zux%zu/%zuB row %zu", k, IOSurfaceGetWidthOfPlane(s, k),
                          IOSurfaceGetHeightOfPlane(s, k), IOSurfaceGetBytesPerElementOfPlane(s, k),
                          IOSurfaceGetBytesPerRowOfPlane(s, k)];
    if (raw::isYcbcrFormat(uint32_t(t->_descriptor.pixelFormat))) {
        raw::YcbcrPlan plan;
        const char *why = ycbcrPlan(t->_descriptor, s, t->_plane, plan);
        [out appendFormat:@" YCbCr not taken: %s", why ?: "taken (refused later: the import or the pool)"];
    }
    return out;
}

static void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra)
{
    t->_pred = p;
    if (!predLogged(p))
        logPred(p, a, b,
                [NSString
                    stringWithFormat:@"(%@ %@%@%@)", kind, describe(t->_descriptor), extra ? @" " : @"", extra ?: @""]);
    // and one line per new shape of texture the raw path leaves to m2v (the predicate's first line names only the first:
    // r7r's Tips replays were tex.shape of textures no line described), up to 32 per process: the predicate, its term, the
    // descriptor, and for a surface its pixel format, planes and what the 4:2:0 path did not take
    static std::mutex lock;
    static NSMutableSet<NSString *> *seen;
    MTLTextureDescriptor *d = t->_descriptor;
    NSString *shape = [NSString
        stringWithFormat:@"%s %llu %@ %lux%lu f%lu t%lu l%lu s%lu u%lx m%lu%@", kPredNames[p], (unsigned long long)a,
                         kind, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.usage, (unsigned long)d.storageMode, surfaceShape(t)];
    {
        std::lock_guard<std::mutex> l(lock);
        if (!seen)
            seen = [NSMutableSet set];
        if (seen.count >= 32 || [seen containsObject:shape])
            return;
        [seen addObject:shape];
    }
    os_log(rawLog(), "raw: texture on m2v: %{public}@", shape);
}

@implementation RMBuffer (Textures)

// A texture over the buffer's bytes: the same memory under a texture header (raw), or m2v's in-place texture over the twin
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    RMTexture *t = [RMTexture new];
    t->_device = _device;
    t->_descriptor = [d copy];
    t->_kind = RMTextureBuffer;
    t->_buffer = self;
    t->_offset = offset;
    t->_rowBytes = row;
    if (rawShape(d) && d.storageMode != MTLStorageModeMemoryless)
        t->_rawOk = gRM->raw->createBufferTexture(_raw, offset, uint32_t(d.width), uint32_t(d.height), uint32_t(row),
                                                  uint32_t(d.pixelFormat), t->_raw);
    if (!t->_rawOk) {  // which term (createBufferTexture's, recomputed: not the hot path)
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0), at = (_raw.va + offset) & 31;
        NSString *extra = [NSString stringWithFormat:@"offset %lu bytesPerRow %lu buffer %llu", (unsigned long)offset,
                                                     (unsigned long)row, (unsigned long long)_raw.length];
        if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"buffer", extra);
        else if (at)
            refuseTexture(t, PredTexBufBase, at, offset, @"buffer", extra);
        else if ((row & 31) || row < packed)
            refuseTexture(t, PredTexBufPitch, row, packed, @"buffer", extra);
        else
            refuseTexture(t, PredTexBufOther, offset + uint64_t(row) * d.height, _raw.length, @"buffer", extra);
    }
    if (!t->_rawOk && ![t rmTwin])
        return nil;
    return (id<MTLTexture>)t;
}

// SPI (Core Animation): the same, one 2D image
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    (void)image;
    return [self newTextureWithDescriptor:d offset:offset bytesPerRow:row];
}

@end

// Resource calls Core Animation makes on every texture and buffer that need no m2v twin (forwarding them would make one, an m2v
// import of memory the raw path draws from). Ownership is accounting only (the twin's, when it has one); no content is protected
// on this device; waitUntilComplete waits for the raw work submitted so far (the kernel's fence interrupt) and the twin's.
@interface RMObject (Resource)
@end
@implementation RMObject (Resource)

- (kern_return_t)setOwnerWithIdentity:(mach_port_t)identity
{
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd])
        return ((kern_return_t (*)(id, SEL, mach_port_t))objc_msgSend)(twin, _cmd, identity);
    return KERN_SUCCESS;
}

- (uint64_t)protectionOptions
{
    return 0;
}

- (void)waitUntilComplete
{
    if (gRM && gRM->raw)
        gRM->raw->wait(gRM->raw->submitted());
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd])
        ((void (*)(id, SEL))objc_msgSend)(twin, _cmd);
}

@end

@interface RMSampler : RMObject {
@public
    raw::Sampler _raw;
    MTLSamplerDescriptor *_descriptor;
}
@end
@implementation RMSampler

+ (Class)twinClass
{
    return NSClassFromString(@"NBSampler");
}

- (void)dealloc
{
    if (gRM)
        gRM->raw->destroySampler(_raw);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [gRM->fallback newSamplerStateWithDescriptor:_descriptor]);
}

@end

@interface RMPipeline : RMObject {
@public
    raw::Pipeline *_raw;  // null: not in the pack (every command buffer using it goes to m2v)
    bool _packable;       // its functions are one framework's default library's (looked up in the pack)
}
@end
@implementation RMPipeline

+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderPipeline");
}

@end

@interface RMTwinObject : RMObject  // compute states, and any m2v object the facade only carries
@end
@implementation RMTwinObject

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    return [super conformsToProtocol:protocol] || [_twin conformsToProtocol:protocol];
}

@end

@interface RMDepthStencil : RMTwinObject {
@public
    raw::DepthStencilDesc _raw;
}
@end
@implementation RMDepthStencil
@end

@implementation RMHeap

- (id)rmTwin
{
    return nil;  // the facade's heap is its own: its buffers and textures are ordinary facade resources
}

- (NSUInteger)size
{
    return _descriptor.size;
}

- (NSUInteger)usedSize
{
    return NSUInteger(_used.load());
}

- (NSUInteger)currentAllocatedSize
{
    return _descriptor.size;
}

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    (void)alignment;
    const uint64_t used = _used.load();
    return used >= _descriptor.size ? 0 : NSUInteger(_descriptor.size - used);
}

- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}

- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}

- (MTLHeapType)type
{
    return _descriptor.type;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    (void)state;
    return MTLPurgeableStateNonVolatile;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if ([b isKindOfClass:[RMBuffer class]]) {
        ((RMBuffer *)b)->_heap = self;
        _used += length;
    }
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    (void)offset;
    return [self newBufferWithLength:length options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [(id<MTLDevice>)_device newTextureWithDescriptor:d];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    (void)offset;
    return [self newTextureWithDescriptor:d];
}

@end

// ---- the tape ----
namespace {
enum Op : uint8_t {
    OpRender,
    OpBlit,
    OpCompute,
    OpEnd,
    OpPipeline,
    OpBytes,
    OpBuffer,
    OpBufferOffset,
    OpTexture,
    OpSampler,
    OpScissor,
    OpViewport,
    OpDraw,
    OpCull,
    OpWinding,
    OpDepthStencil,
    OpFillMode,
    OpBlendColor,
    OpStencilRef,
    OpDepthBias,
    OpBarrier,
    OpUse,
    OpUpdateFence,
    OpWaitFence,
    OpCopyTexture,
    OpCopyBuffer,
    OpCopyBufferToTexture,
    OpCopyTextureToBuffer,
    OpMipmaps,
    OpInvocation,
    OpSignalEvent,
    OpWaitEvent,
    OpDrawIndexed,
    OpVisibility,
};

// one record: `stage` 1 vertex, 2 fragment; `obj` unretained (the command buffer keeps it); `x` into the extras
struct Cmd {
    Op op;
    uint8_t stage;
    uint16_t index;
    uint32_t a, b;
    uint64_t c, x;
    void *obj, *obj2;
};

inline Cmd cmd(Op op, uint8_t stage = 0, uint16_t index = 0)
{
    Cmd c = {};
    c.op = op;
    c.stage = stage;
    c.index = index;
    return c;
}
}  // namespace

@interface RMQueue : RMObject {
@public
    dispatch_queue_t _completionQueue;
}
@end

// A part of a command buffer submitted on its own (RMCommandBuffer::_segs): tape [from, to), on the raw path or m2v
struct RMSegment {
    uint32_t from, to;
    bool raw;
};

@interface RMCommandBuffer : RMObject {
@public
    uint64_t _protection;  // setProtectionOptions: (kept, no effect)
    // Its encoders: each one's first command (OpRender, OpBlit, OpCompute) carries index 1 where it goes to m2v (a refusal taken
    // while it was the active encoder: _encRefused, written into it as the next encoder begins or at commit, _encAt the
    // command's place). A refusal outside any encoder keeps the command buffer whole (_whole). One whose encoders are some raw's
    // and some m2v's is submitted in segments (_segs, planSegments): each run of encoders of one driver on its own, in order on
    // the GPU as two command buffers of the drivers are (the kernel's Order), so a refused pass costs m2v that pass, not the
    // frame. The command buffer completes with its last segment.
    size_t _encAt;
    bool _encOpen, _encRefused, _whole;
    std::vector<RMSegment> _segs;
    size_t _segNext;
    NSError *_segError;  // an m2v segment's error, reported at completion
    bool
        _sampledShadows;  // it binds a texture sampled through a pool image (raw::Texture::sampled): encodeRaw refreshes them
    bool _chains;         // it uses a mipmapped texture (raw::Texture::chain): encodeRaw and the replay keep its levels
    std::vector<Cmd> _tape;
    std::vector<uint8_t> _bytes;
    std::vector<uint64_t> _extra;
    std::vector<id> _owners;  // everything the tape names, retained until completion
    RMReason _reason;
    NSString *_why;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    MTLCommandBufferStatus _status;
    NSError *_error;
    RMQueue *_queue;
    bool _committed, _scheduledDone, _completedDone, _m2v;
    // scheduled() and finish() ran (their handlers taken): a handler added after goes at once, and scheduled() runs once (a
    // command buffer held for a shared event is scheduled at its commit, eventHeld)
    bool _scheduledSent, _completedSent;
    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _seqno;
    double _gpuStart, _gpuEnd;
    id _active;               // the encoder being recorded
    uint64_t _preds;          // every refusal predicate it hit (1 << RMPred)
    uint64_t _predA, _predB;  // the first one's values
    uint8_t _pred0;           // the first one
#if SWITCH_FRAME_TIMELINE
    switch_frame::Stamps _frame;
#endif
}
- (void)takeReason:(RMReason)reason why:(NSString *)why;
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why;
- (void)closeEncoder;
- (void)record:(const Cmd &)cmd;
- (void)retain:(id)object;
@end

// A refusal predicate hit while recording (or encoding) `cb`: into its mask; the first one's values kept; the process's first hit
// of `p` logged with them
static inline void hit(RMCommandBuffer *cb, RMPred p, uint64_t a = 0, uint64_t b = 0)
{
    cb->_preds |= 1ull << p;
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    if (!predLogged(p))
        logPred(p, a, b, nil);
}

// ---- encoders: they record ----
@interface RMEncoder : RMObject {
@public
    RMCommandBuffer *_cb;
    bool _ended;
}
@end
@implementation RMEncoder

- (id)rmTwin
{
    return nil;
}

// What the m2v encoder of the same kind answers (respondsToSelector: as Core Animation's optional paths ask it today)
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = [[self class] twinClass];
    return c && [c instancesRespondToSelector:selector];
}

// A selector this encoder does not record itself (one the m2v encoder implements, or some MTL protocol declares): recorded as an
// invocation (objects mapped to twins at replay) and the command buffer goes to m2v.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = [[self class] twinClass];
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    [inv retainArguments];
    [_cb retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    [_cb record:c];
    [_cb takeReason:WhyCall why:@"an encoder call recorded as an invocation"];
    hit(_cb, PredCallInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.invocation", uintptr_t(inv.selector), "-[%s %s] recorded as an invocation",
                         class_getName(object_getClass(self)), sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

+ (Class)twinClass
{
    return nil;
}

- (void)endEncoding
{
    SWITCH_API(ApiEncoder);
    if (_ended)
        return;
    _ended = true;
    [_cb record:cmd(OpEnd)];
    _cb->_active = nil;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}

- (void)updateFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpUpdateFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

- (void)waitForFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpWaitFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

@end

// A render encoder keeps what its draws need to decide, as they are recorded, whether the raw path draws them exactly as m2v
// would: the state, each bound buffer's address, the textures, the pass's formats. A draw it would not is the command buffer's
// reason then, so encodeRaw never meets it (a fallback costs no encoding).
@interface RMRenderEncoder : RMEncoder {
@public
    RMPipeline *_pipe;
    uint64_t _bufVa[3]
                   [31];  // stage 1 vertex, 2 fragment: the bound buffer's address plus offset (setBytes: 0, aligned)
    RMBuffer *_buf[3][31];
    RMTexture *_tex[16];   // fragment textures (unretained: the command buffer retains what it binds)
    uint32_t _formats[8];  // the pass's color attachments' formats as the raw path holds them (0: none)
    uint32_t _count;       // its attachment count (the last attachment's index + 1)
    uint32_t _zeta;        // its depth/stencil attachment's format as the raw path holds it (0: none)
    raw::DepthStencilDesc _ds;
}
@end
@interface RMBlitEncoder : RMEncoder
@end
@interface RMComputeEncoder : RMEncoder
@end

static inline void setBinding(RMCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t c)
{
    if (object)
        [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = c;
    [cb record:m];
}

@implementation RMRenderEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderEncoder");
}

// Core Animation's texture-and-sampler SPI (CA::OGL::MetalContext::draw, every textured draw), as the frontend answers it
- (void)setFragmentTexture:(id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    [self texture:2 object:t index:ti];
    [self sampler:2 object:s index:si];
}

// Core Animation sets the mode Disabled when its encoder state changes (update_encoder_state): nothing to do; any other mode
// counts occlusion, which the raw path does not
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    if (mode != MTLVisibilityResultModeDisabled) {
        [_cb takeReason:WhyCall why:@"a visibility result mode"];
        hit(_cb, PredCallVisibility, mode);
    }
    Cmd m = cmd(OpVisibility);
    m.a = uint32_t(mode);
    m.c = offset;
    [_cb record:m];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    SWITCH_API(ApiDraw);
    if (![(id)buffer isKindOfClass:[RMBuffer class]] || instances != 1 || baseVertex || baseInstance) {
        [_cb takeReason:WhyCall why:@"an indexed draw of several instances or a base vertex"];
        hit(_cb, PredCallIndexedForm, instances, uint64_t(uint32_t(baseVertex)) | uint64_t(baseInstance) << 32);
    } else if ((indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) ||
               offset % (indexType == MTLIndexTypeUInt16 ? 2 : 4) ||
               offset + uint64_t(count) * (indexType == MTLIndexTypeUInt16 ? 2 : 4) >
                   ((RMBuffer *)buffer)->_raw.length) {
        [_cb takeReason:WhyEncode why:@"an indexed draw the raw path does not take"];
        hit(_cb, PredEncIndexedRange, offset, indexType == MTLIndexTypeUInt16 ? 2 : 4);
    }
    [self checkDraw:0];
    if (buffer)
        [_cb retain:buffer];
    Cmd m = cmd(OpDrawIndexed, uint8_t(type));
    m.a = uint32_t(count);
    m.b = uint32_t(indexType);
    m.c = offset;
    m.obj = (__bridge void *)buffer;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(instances);
    _cb->_extra.push_back(uint64_t(baseVertex));
    _cb->_extra.push_back(baseInstance);
    [_cb record:m];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
{
    SWITCH_API(ApiDraw);
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:instances
                     baseVertex:0
                   baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    SWITCH_API(ApiState);
    RMPipeline *p = (RMPipeline *)state;
    if (![(id)p isKindOfClass:[RMPipeline class]]) {
        [_cb takeReason:WhyState why:@"a render state not made by this device"];
        hit(_cb, PredStateForeign);
    } else if (!p->_raw) {
        [_cb takeReason:WhyState why:@"a state not in the pack (its label: the line it was made with)"];
        hit(_cb, p->_packable ? PredStateNotInPack : PredStateNotPackable);
    }
    _pipe = [(id)p isKindOfClass:[RMPipeline class]] ? p : nil;
    setBinding(_cb, OpPipeline, 0, 0, state, 0);
}

// What raw::RenderEncoder::prepare would refuse of a draw with the encoder's bindings, decided here (the same tests)
- (void)checkDraw:(NSUInteger)baseInstance
{
    // no raw draw to decide (m2v drops a draw without a state too), or this encoder goes to m2v already (its pass refused: its
    // formats are not recorded, and every draw would hit enc.colorfmt for nothing, r7p5's "a=0 b=0")
    if (!_pipe || !_pipe->_raw || _cb->_encRefused)
        return;
    const raw::Pipeline &pl = *_pipe->_raw;
    if (baseInstance) {
        [_cb takeReason:WhyEncode why:@"draw with a base instance"];
        return hit(_cb, PredEncBaseInstance, baseInstance);
    }
    for (uint32_t j = 0; j < 8; j++) {
        const uint32_t f = j < _count ? _formats[j] : 0;
        if (f != pl.colorFormats[j] && !(f && pl.colorFormats[j] == 0)) {
            [_cb takeReason:WhyEncode why:@"a state whose color formats differ from the pass's"];
            return hit(_cb, PredEncColorFormats, j, f);
        }
    }
    if (pl.depthFormat != _zeta || (!_zeta && _ds.active())) {  // (raw::RenderEncoder::prepare's test)
        [_cb takeReason:WhyEncode why:@"a depth/stencil state or format the pass's attachment does not take"];
        return hit(_cb, PredEncDepthStencil, pl.depthFormat, _zeta | uint64_t(_ds.active()) << 32);
    }
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (e.kind == raw::EntryUbo) {
            // m2v's SlotBindOffset residual is not in the pack (every probe bound at offset 0); the device answers 256 for
            // minConstantBufferAlignmentBytes, so Core Animation's buffer pool binds there
            if (!e.samplerKind && e.stage < 3 && e.slot < 31 && (_bufVa[e.stage][e.slot] & 255)) {
                [_cb takeReason:WhyEncode why:@"a uniform buffer bound at an offset that is not a multiple of 256"];
                return hit(_cb, PredEncUboOffset, uint64_t(e.stage) << 8 | e.slot, _bufVa[e.stage][e.slot] & 255);
            }
        } else if (e.kind == raw::EntryTexture && e.samplerKind == raw::SamplerConstexpr && e.slot < 16 &&
                   _tex[e.slot] && _tex[e.slot]->_raw.addressMap) {
            [_cb takeReason:WhyEncode why:@"a planar texture sampled through a constexpr sampler"];
            return hit(_cb, PredEncPlanarClamp, e.slot, _tex[e.slot]->_raw.addressMap);
        }
    }
}

- (void)bytes:(int)stage data:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (stage >= 1 && stage <= 2 && index < 31) {
        _bufVa[stage][index] = 0;  // its own 256-byte-aligned upload chunk
        _buf[stage][index] = nil;
    }
    RMCommandBuffer *cb = _cb;
    Cmd m = cmd(OpBytes, uint8_t(stage), uint16_t(index));
    m.a = uint32_t(cb->_bytes.size());
    m.b = uint32_t(length);
    cb->_bytes.insert(cb->_bytes.end(), (const uint8_t *)bytes, (const uint8_t *)bytes + length);
    [cb record:m];
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:1 data:bytes length:length index:index];
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:2 data:bytes length:length index:index];
}

- (void)buffer:(int)stage object:(id<MTLBuffer>)b offset:(NSUInteger)offset index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (b && ![(id)b isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCall why:@"a buffer not made by this device"];
        hit(_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length);
    }
    if (stage >= 1 && stage <= 2 && index < 31) {
        RMBuffer *x = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
        _buf[stage][index] = x;
        _bufVa[stage][index] = x ? x->_raw.va + offset : 0;
    }
    setBinding(_cb, OpBuffer, stage, index, b, offset);
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:1 object:b offset:offset index:index];
}

- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:2 object:b offset:offset index:index];
}

- (void)bufferOffset:(int)stage offset:(NSUInteger)offset index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (index < 31 && _buf[stage][index])
        _bufVa[stage][index] = _buf[stage][index]->_raw.va + offset;
    setBinding(_cb, OpBufferOffset, stage, index, nil, offset);
}

- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:1 offset:offset index:index];
}

- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:2 offset:offset index:index];
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:1 object:b[k] offset:o[k] index:r.location + k];
}

- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:2 object:b[k] offset:o[k] index:r.location + k];
}

- (void)texture:(int)stage object:(id<MTLTexture>)t index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (t && (!x || !x->_rawOk || !x->_raw.tic)) {
        [_cb takeReason:WhyTexture why:@"a texture the raw path does not keep"];
        if (!x) {
            if (!predLogged(PredTexForeign))
                logPred(PredTexForeign, t.pixelFormat, 0,
                        [NSString stringWithFormat:@"class %s", class_getName(object_getClass(t))]);
            hit(_cb, PredTexForeign, t.pixelFormat);
        } else
            hit(_cb, RMPred(x->_pred ?: (x->_rawOk ? PredEncNoTic : PredTexShape)), x->_descriptor.pixelFormat,
                x->_kind);
    }
    if (stage == 2 && index < 16)
        _tex[index] = x;
    if (x && x->_raw.sampled)
        _cb->_sampledShadows = true;
    if (x && (x->_raw.chain || (x->_parent && x->_parent->_raw.chain)))
        _cb->_chains = true;
    setBinding(_cb, OpTexture, stage, index, t, 0);
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:2 object:t index:index];
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:1 object:t index:index];
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self texture:2 object:t[k] index:r.location + k];
}

- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self texture:1 object:t[k] index:r.location + k];
}

- (void)sampler:(int)stage object:(id<MTLSamplerState>)s index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (s && ![(id)s isKindOfClass:[RMSampler class]]) {
        [_cb takeReason:WhyCall why:@"a sampler not made by this device"];
        hit(_cb, PredCallSamplerForeign, index);
    }
    if (stage == 1 && s) {
        [_cb takeReason:WhyCall why:@"a vertex sampler"];
        hit(_cb, PredCallVertexSampler, index);
    }
    setBinding(_cb, OpSampler, stage, index, s, 0);
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:2 object:s index:index];
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:1 object:s index:index];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self sampler:2 object:s[k] index:r.location + k];
}

- (void)setScissorRect:(MTLScissorRect)r
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpScissor);
    m.a = uint32_t(r.x);
    m.b = uint32_t(r.y);
    m.c = uint64_t(r.width) << 32 | uint32_t(r.height);
    [_cb record:m];
}

- (void)setViewport:(MTLViewport)v
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpViewport);
    m.x = _cb->_extra.size();
    const double d[6] = {v.originX, v.originY, v.width, v.height, v.znear, v.zfar};
    for (double x : d) {
        uint64_t u;
        memcpy(&u, &x, 8);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

- (void)setCullMode:(MTLCullMode)mode
{
    Cmd m = cmd(OpCull);
    m.a = uint32_t(mode);
    [_cb record:m];
}

- (void)setFrontFacingWinding:(MTLWinding)w
{
    Cmd m = cmd(OpWinding);
    m.a = uint32_t(w);
    [_cb record:m];
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
    if (s && ![(id)s isKindOfClass:[RMDepthStencil class]]) {
        [_cb takeReason:WhyCall why:@"a depth/stencil state not made by this device"];
        hit(_cb, PredStateForeign);
    }
    _ds = [(id)s isKindOfClass:[RMDepthStencil class]] ? ((RMDepthStencil *)s)->_raw : raw::DepthStencilDesc();
    setBinding(_cb, OpDepthStencil, 0, 0, s, 0);
}

// SPI: lines one pixel wide are what the raw path draws (its default); any other width goes to m2v with the call
- (void)setLineWidth:(float)width
{
    if (width == 1.0f)
        return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[self methodSignatureForSelector:_cmd]];
    inv.selector = _cmd;
    [inv setArgument:&width atIndex:2];
    [self forwardInvocation:inv];
}

// No tile shaders (as m2v: the device says so); their calls stay m2v's, which refuses them
- (NSUInteger)tileWidth
{
    return 0;
}

- (NSUInteger)tileHeight
{
    return 0;
}

- (void)setTriangleFillMode:(MTLTriangleFillMode)mode
{
    Cmd m = cmd(OpFillMode);
    m.a = uint32_t(mode);
    [_cb record:m];
}

- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
    Cmd m = cmd(OpBlendColor);
    m.x = _cb->_extra.size();
    const float f[4] = {r, g, b, a};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

- (void)setStencilReferenceValue:(uint32_t)value
{
    Cmd m = cmd(OpStencilRef);
    m.a = value;
    m.b = value;
    [_cb record:m];
}

- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
    Cmd m = cmd(OpStencilRef);
    m.a = front;
    m.b = back;
    [_cb record:m];
}

- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp
{
    if (bias != 0 || slope != 0 || clamp != 0) {
        uint32_t b, k;
        memcpy(&b, &bias, 4);
        memcpy(&k, &slope, 4);
        [_cb takeReason:WhyCall why:@"a depth bias"];
        hit(_cb, PredCallDepthBias, b, k);
    }
    Cmd m = cmd(OpDepthBias);
    m.x = _cb->_extra.size();
    const float f[3] = {bias, slope, clamp};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)base
{
    SWITCH_API(ApiDraw);
    [self checkDraw:base];
    Cmd m = cmd(OpDraw, uint8_t(type));
    m.a = uint32_t(start);
    m.b = uint32_t(count);
    m.c = uint64_t(instances) << 32 | uint32_t(base);
    [_cb record:m];
}

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
{
    SWITCH_API(ApiDraw);
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
                   afterStages:(MTLRenderStages)after
                  beforeStages:(MTLRenderStages)before
{
    Cmd m = cmd(OpBarrier);
    m.a = uint32_t(scope);
    m.b = uint32_t(after);
    m.c = uint64_t(before);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
    if (!resource)
        return;
    [_cb retain:resource];
    Cmd m = cmd(OpUse);
    m.obj = (__bridge void *)resource;
    m.a = uint32_t(usage);
    m.b = uint32_t(stages);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    [self useResource:resource usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)useHeap:(id<MTLHeap>)heap
{
    (void)heap;  // the facade's heaps hold ordinary resources: nothing to make resident
}

- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    (void)heap;
    (void)stages;
}

- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    (void)stages;
    [self updateFence:fence];
}

- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    (void)stages;
    [self waitForFence:fence];
}

@end

// Where plain Metal reads or writes each buffer row of a copy between a buffer and a texture: rows `bytesPerRow` apart from
// `offset`. A layout off Metal's rules (an offset or bytesPerRow off the texel, or rows that overlap) is placed as Apple's
// GPUs place it (m2v blit_plan.rs placed_rows, measured): each row rounded down to the texel's component (at most 4 bytes),
// except that a copy into a texture of more than one row whose bytesPerRow rounded down to the texel is a multiple of 16,
// from an offset that is, takes Metal's aligned path: its rows at that rounded pitch from offset 0. False: a layout
// Metal's behaviour is not known for, refused as m2v refuses it (rows that overlap out of a texture; on the aligned path,
// a pitch below a row or a nonzero offset).
struct BufferRows {
    uint64_t first, pitch;  // row y at first + y * pitch where `uniform`
    bool uniform;
    uint32_t word;
};

static bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp,
                       bool intoTexture, BufferRows &out)
{
    const uint32_t word = std::min<uint32_t>(bpp, 4);
    if (offset % bpp == 0 && bytesPerRow % bpp == 0 && (rows < 2 || bytesPerRow >= rowBytes)) {  // Metal's rules
        out = {offset, bytesPerRow, true, word};
        return true;
    }
    if (rows > 1 && bytesPerRow < rowBytes && !intoTexture)
        return false;
    const uint64_t pitch = bytesPerRow - bytesPerRow % bpp;
    if (intoTexture && rows > 1 && pitch % 16 == 0 && offset % 16 == 0) {
        if (offset || pitch < rowBytes)
            return false;
        out = {0, pitch, true, word};
        return true;
    }
    out = {offset - offset % word, bytesPerRow, offset % word == 0 && bytesPerRow % word == 0, word};
    return true;
}

// Row y of `r` (offset and bytesPerRow as given to bufferRows)
static uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y)
{
    if (r.uniform)
        return r.first + y * r.pitch;
    const uint64_t at = offset + uint64_t(y) * bytesPerRow;
    return at - at % r.word;
}

// The copy engine's copies of those rows: one copy of every row where they lie a pitch apart, else one per row
static void copyRows(raw::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     bool intoTexture)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, w, h, intoTexture);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * t.bpp, t, x, y + k, w, 1,
                             intoTexture);
}

// Blit options choose a depth/stencil aspect or a PVRTC row layout; a colour texture the raw path holds has neither, so they
// change nothing about its copy (m2v's replay of the copy never carried them)
static bool blitOptionsMoot(id<MTLTexture> t, MTLBlitOption options)
{
    if (!options)
        return true;
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    return x && x->_rawOk && !x->_raw.zeta && !x->_raw.converted();
}

@implementation RMBlitEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBBlitEncoder");
}

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    RMTexture *a = (RMTexture *)s, *b = (RMTexture *)d;
    const bool both = [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMTexture class]];
    // a mipmapped texture's level below the first (its chain's, raw::BlitEncoder::copyFromLevel, 1:1 into a texture of the
    // same format: the 2D engine into rows, the copy engine into a block-linear one)
    const raw::Texture::Chain *ch = both && a->_rawOk ? a->_raw.chain : nullptr;
    const bool level = ch && sl && sl < ch->layout.levels && b->_rawOk && !b->_raw.memoryless &&
                       (b->_raw.blockLinear || !((b->_raw.va | b->_raw.pitch) & 127)) &&
                       b->_raw.format == a->_raw.format && !b->_raw.planar && !b->_raw.ycbcr && !ss && !ds && !dl &&
                       size.depth == 1 && so.x + size.width <= ch->layout.level[sl].width &&
                       so.y + size.height <= ch->layout.level[sl].height && dorg.x + size.width <= b->_raw.width &&
                       dorg.y + size.height <= b->_raw.height;
    const bool ok = level || (both && a->_rawOk && b->_rawOk && !ss && !sl && !ds && !dl && size.depth == 1 &&
                              a->_raw.bpp == b->_raw.bpp && !a->_raw.memoryless && !b->_raw.memoryless &&
                              so.x + size.width <= a->_raw.width && so.y + size.height <= a->_raw.height &&
                              dorg.x + size.width <= b->_raw.width && dorg.y + size.height <= b->_raw.height);
    if (both && (a->_raw.chain || b->_raw.chain))
        _cb->_chains = true;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture copy the raw path does not do (levels, slices, formats)"];
        hit(_cb, PredBlitTexCopy, ss | sl << 8 | ds << 16 | dl << 24,
            uint64_t(size.width) << 32 | uint32_t(size.height));
    } else if (a->_raw.converted() || b->_raw.converted()) {
        // Metal's copies move a planar texture's planes' bytes: m2v's
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, a->_raw.converted(), b->_raw.converted());
    }
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[11] = {ss,
                            sl,
                            so.x,
                            so.y,
                            so.z,
                            size.width,
                            size.height,
                            size.depth,
                            ds,
                            dl,
                            uint64_t(dorg.x) | uint64_t(dorg.y) << 32};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 11);
    _cb->_extra.push_back(dorg.z);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    [self copyFromTexture:s
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(s.width, s.height, 1)
                toTexture:d
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (s.mipmapLevelCount > 1 || s.arrayLength > 1 || s.textureType != MTLTextureType2D) {
        [_cb takeReason:WhyTexture why:@"a whole-texture copy of several levels or slices"];
        hit(_cb, PredBlitWholeTexCopy, s.mipmapLevelCount, s.arrayLength);
    }
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dof
                  size:(NSUInteger)n
{
    RMBuffer *a = (RMBuffer *)s, *b = (RMBuffer *)d;
    if (![(id)a isKindOfClass:[RMBuffer class]] || ![(id)b isKindOfClass:[RMBuffer class]] || so + n > a->_raw.length ||
        dof + n > b->_raw.length) {
        [_cb takeReason:WhyCall why:@"a buffer copy out of range"];
        hit(_cb, PredCallBufCopyRange, so + n, dof + n);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = 0;
    m.c = so;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(dof);
    _cb->_extra.push_back(n);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    RMBuffer *a = (RMBuffer *)s;
    RMTexture *b = (RMTexture *)d;
    if ([(id)b isKindOfClass:[RMTexture class]] && b->_raw.chain)
        _cb->_chains = true;
    BufferRows rows;
    const bool ok =
        [(id)a isKindOfClass:[RMBuffer class]] && [(id)b isKindOfClass:[RMTexture class]] && b->_rawOk && !ds && !dl &&
        size.depth == 1 && !b->_raw.memoryless && b->_raw.bpp &&
        bufferRows(so, row, uint32_t(size.height), uint32_t(size.width * b->_raw.bpp), b->_raw.bpp, true, rows) &&
        bufferRow(rows, so, row, uint32_t(size.height - 1)) + size.width * b->_raw.bpp <= a->_raw.length &&
        dorg.x + size.width <= b->_raw.width && dorg.y + size.height <= b->_raw.height;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a buffer-to-texture copy the raw path does not do"];
        hit(_cb, PredBlitBufToTex, row, uint64_t(ds) | uint64_t(dl) << 8 | uint64_t(size.depth) << 16);
    } else if (b->_raw.converted()) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, 0, 1);
    }
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyBufferToTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        so, row, image, size.width, size.height, size.depth, ds, dl, uint64_t(dorg.x) | uint64_t(dorg.y) << 32, dorg.z};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
                options:(MTLBlitOption)options
{
    if (!blitOptionsMoot(d, options)) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromBuffer:s
               sourceOffset:so
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:d
           destinationSlice:ds
           destinationLevel:dl
          destinationOrigin:dorg];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    RMTexture *a = (RMTexture *)s;
    RMBuffer *b = (RMBuffer *)d;
    if ([(id)a isKindOfClass:[RMTexture class]] && a->_raw.chain)
        _cb->_chains = true;
    BufferRows rows;
    const bool ok =
        [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMBuffer class]] && a->_rawOk && !ss && !sl &&
        size.depth == 1 && !a->_raw.memoryless && a->_raw.bpp &&
        bufferRows(dof, row, uint32_t(size.height), uint32_t(size.width * a->_raw.bpp), a->_raw.bpp, false, rows) &&
        bufferRow(rows, dof, row, uint32_t(size.height - 1)) + size.width * a->_raw.bpp <= b->_raw.length &&
        so.x + size.width <= a->_raw.width && so.y + size.height <= a->_raw.height;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture-to-buffer copy the raw path does not do"];
        hit(_cb, PredBlitTexToBuf, row, uint64_t(ss) | uint64_t(sl) << 8 | uint64_t(size.depth) << 16);
    } else if (a->_raw.converted()) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, 1, 0);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyTextureToBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        ss, sl, uint64_t(so.x) | uint64_t(so.y) << 32, so.z, size.width, size.height, size.depth, dof, row, image};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
                     options:(MTLBlitOption)options
{
    if (!blitOptionsMoot(s, options)) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromTexture:s
                     sourceSlice:ss
                     sourceLevel:sl
                    sourceOrigin:so
                      sourceSize:size
                        toBuffer:d
               destinationOffset:dof
          destinationBytesPerRow:row
        destinationBytesPerImage:image];
}

- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (x)
        declareGpuWrites(x);
    if (x && x->_rawOk && x->_raw.chain)  // a chain's levels, on the 2D engine (raw::BlitEncoder::generate)
        _cb->_chains = true;
    else {
        [_cb takeReason:WhyMipmaps why:@"generateMipmapsForTexture"];
        hit(_cb, PredMipmaps, t.pixelFormat, t.mipmapLevelCount);
    }
    setBinding(_cb, OpMipmaps, 0, 0, t, 0);
}

- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;  // shared memory: nothing to make visible
}

- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l
{
    (void)t;
    (void)s;
    (void)l;
}

- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t
{
    (void)t;  // pitch-linear memory either way
}

- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t
{
    (void)t;
}

@end

@implementation RMComputeEncoder  // everything recorded as invocations (the command buffer is m2v's): see RMEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputeEncoder");
}

@end

// ---- the device's submission machinery ----
namespace {}  // namespace

static void markWritten(RMCommandBuffer *cb);
static void finish(RMCommandBuffer *cb, NSError *error);
static bool heldBehindEvent();

// A depth/stencil attachment the raw path renders (raw::DepthStencilAttachment): one memoryless Depth32Float_Stencil8 texture as
// both aspects, whole (level, slice and depth plane 0, no resolve), cleared or not loaded and not stored (memoryless), a clear
// depth that is a number (metal2vulkan refuses a NaN)
static bool rawDepthStencil(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    RMTexture *t = (RMTexture *)d.texture;
    if (!t || t != (RMTexture *)s.texture || ![(id)t isKindOfClass:[RMTexture class]] || !t->_rawOk || !t->_raw.zeta ||
        !t->_raw.memoryless || std::isnan(d.clearDepth))
        return false;
    for (MTLRenderPassAttachmentDescriptor *a : @[ d, s ])
        if (a.level || a.slice || a.depthPlane || a.resolveTexture || a.storeAction != MTLStoreActionDontCare ||
            (a.loadAction != MTLLoadActionClear && a.loadAction != MTLLoadActionDontCare))
            return false;
    return true;
}

@implementation RMCommandBuffer

- (void)retain:(id)object
{
    if (!object || (!_owners.empty() && _owners.back() == object))
        return;
    _owners.push_back(object);
}

- (void)record:(const Cmd &)cmd
{
    _tape.push_back(cmd);
}

// The encoder recorded last: whether it goes to m2v, into its first command
- (void)closeEncoder
{
    if (_encOpen && _encAt < _tape.size())
        _tape[_encAt].index = _encRefused ? 1 : 0;
    _encOpen = false;
}

- (void)takeReason:(RMReason)reason why:(NSString *)why
{
    if (_active)
        _encRefused = true;  // this encoder goes to m2v
    else
        _whole = true;
    if (_reason != WhyRaw)
        return;
    _reason = reason;
    _why = why;
}

// A refusal of the command buffer's own (an event m2v must see, a call on the command buffer): the whole of it goes to m2v
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why
{
    _whole = true;
    [self takeReason:reason why:why];
}

- (id)rmTwin
{
    return nil;
}

- (MTLCommandBufferStatus)status
{
    std::lock_guard<std::mutex> l(_lock);
    return _status;
}

- (NSError *)error
{
    return _error;
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}

- (id<MTLLogContainer>)logs
{
    return nil;
}

- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}

- (CFTimeInterval)kernelStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)kernelEndTime
{
    return _gpuEnd;
}

- (void)begin:(RMEncoder *)e
{
    if (_active)
        [(RMEncoder *)_active endEncoding];  // Metal: one encoder at a time
    e->_device = _device;
    e->_cb = self;
    _active = e;
    [self closeEncoder];
    _encAt = _tape.size();  // its first command is the next one recorded
    _encOpen = true;
    _encRefused = false;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    RMRenderEncoder *e = [RMRenderEncoder new];
    [self begin:e];
    MTLRenderPassDescriptor *pass = [d copy];
    [self retain:pass];
    for (unsigned c = 0; c < 8; c++) {
        id<MTLTexture> t = pass.colorAttachments[c].texture;
        if (!t)
            continue;
        [self retain:t];
        [self retain:pass.colorAttachments[c].resolveTexture];
        RMTexture *x = (RMTexture *)t;
        if ([(id)x isKindOfClass:[RMTexture class]] && x->_raw.chain)
            _chains = true;
        if ([(id)x isKindOfClass:[RMTexture class]])
            declareGpuWrites(x);
        if (![(id)x isKindOfClass:[RMTexture class]] || !x->_rawOk || !(x->_raw.renderable || x->_raw.shadowTarget)) {
            [self takeReason:WhyPass why:@"a color attachment the raw path does not render to"];
            const bool own = [(id)x isKindOfClass:[RMTexture class]];
            // a = format; b = attachment | kind << 8 | the texture's own predicate << 16 (0: kept, not renderable)
            const uint64_t b = c | (own ? uint64_t(x->_kind) : 0xff) << 8 | (own ? uint64_t(x->_pred) : 0) << 16;
            if (own && x->_rawOk &&
                !predLogged(PredPassAttFormat))  // kept, not a target: which term (raw_gpu.cpp linearTarget)
                logPred(
                    PredPassAttFormat, t.pixelFormat, b,
                    [NSString stringWithFormat:@"(kept, not a target: %ux%u pitch %u va&127 %llu, a pitch target needs "
                                               @"both multiples of 128)",
                                               x->_raw.width, x->_raw.height, x->_raw.pitch,
                                               (unsigned long long)(x->_raw.va & 127)]);
            hit(self, PredPassAttFormat, t.pixelFormat, b);
        } else if (x->_raw.planar || x->_raw.ycbcr) {
            [self takeReason:WhyPass why:@"a planar texture as a render target"];
            hit(self, PredEncPlanarRT, t.pixelFormat, c);
        } else if (pass.colorAttachments[c].slice || pass.colorAttachments[c].depthPlane ||
                   pass.colorAttachments[c].resolveTexture ||
                   (pass.colorAttachments[c].level &&
                    (!x->_raw.chain || pass.colorAttachments[c].level >= x->_raw.chain->layout.levels))) {
            // A slice or depth plane needs an array or 3D texture and a resolve a multisampled one, none of which the raw
            // path keeps (rawShape)
            [self takeReason:WhyPass why:@"a color attachment slice, depth plane or resolve"];
            hit(self, PredPassAttLevel, pass.colorAttachments[c].level | pass.colorAttachments[c].slice << 8,
                pass.colorAttachments[c].resolveTexture != nil);
        } else {
            e->_formats[c] = x->_raw.format;
            e->_count = c + 1;
        }
    }
    if (pass.depthAttachment.texture || pass.stencilAttachment.texture) {
        [self retain:pass.depthAttachment.texture];
        [self retain:pass.stencilAttachment.texture];
        if (rawDepthStencil(pass))
            e->_zeta = raw::kZetaFormat;
        else {
            [self takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
            hit(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                pass.stencilAttachment.texture.pixelFormat);
        }
    }
    // renderTargetWidth / renderTargetHeight: the render area's bounds (raw::PassDesc width, height)
    if (pass.visibilityResultBuffer || (pass.renderTargetArrayLength > 1) || pass.rasterizationRateMap) {
        [self retain:pass.visibilityResultBuffer];
        [self takeReason:WhyPass why:@"a visibility attachment, layers or a rate map"];
        hit(self, PredPassOther, pass.visibilityResultBuffer != nil,
            pass.renderTargetArrayLength | uint64_t(pass.rasterizationRateMap != nil) << 32);
    }
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    [self record:m];
    return (id<MTLRenderCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    RMBlitEncoder *e = [RMBlitEncoder new];
    [self begin:e];
    [self record:cmd(OpBlit)];
    return (id<MTLBlitCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d
{
    (void)d;
    return [self blitCommandEncoder];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    RMComputeEncoder *e = [RMComputeEncoder new];
    [self begin:e];
    Cmd m = cmd(OpCompute);
    m.a = uint32_t(type);
    [self record:m];
    [self takeReason:WhyCompute why:@"a compute encoder"];
    hit(self, PredCompute, type);
    return (id<MTLComputeCommandEncoder>)e;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    return [self computeCommandEncoderWithDispatchType:d.dispatchType];
}

- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal
{
    // the m2v device's events, shared or plain (NBEvent answers a shared event's value, set and notifications): the raw path
    // signals them from the CPU once its work completes, and holds its work until a wait is covered (waitCovered); an event
    // without those (another device's) is m2v's alone
    if (![(id)event respondsToSelector:@selector(signaledValue)] ||
        ![(id)event respondsToSelector:@selector(setSignaledValue:)] ||
        ![(id)event respondsToSelector:@selector(notifyListener:atValue:block:)]) {
        [self takeWholeReason:WhyEvent why:@"an event the raw path cannot read, set or be notified of"];
        hit(self, PredEvent, signal);
    }
    [self retain:event];
    Cmd m = cmd(signal ? OpSignalEvent : OpWaitEvent);
    m.obj = (__bridge void *)event;
    m.c = value;
    [self record:m];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false];
}

- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}

- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        now = _scheduledSent;
        if (!now)
            [_scheduledHandlers addObject:[block copy]];
    }
    if (now)
        dispatch_async(gRM->handlers, ^{
            block((id<MTLCommandBuffer>)self);
        });
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now)
            [_completedHandlers addObject:[block copy]];
    }
    if (now)
        block((id<MTLCommandBuffer>)self);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAtTime:t];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAfterMinimumDuration:d];
    }];
}

- (void)enqueue
{}

- (void)commit
{
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_committed)
            return;
        _committed = true;
        _status = MTLCommandBufferStatusCommitted;
    }
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const raw::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed), gTwinsMade.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(RMEncoder *)_active endEncoding];
    [(RMDevice *)_device rmCommit:self];
#if SWITCH_FRAME_TIMELINE
    _frame.mark(switch_frame::Back);
#endif
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}

- (void)waitUntilScheduled
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed)
        return;
    if (_changed.wait_for(l, kHeldGrace, [&] {
            return _scheduledDone;
        }))
        return;
    // Behind a shared event no one reached yet (its own wait, or an earlier command buffer's that holds it in order): Metal's
    // command buffer is scheduled there, its GPU waiting for the event, so this returns. Core Animation's render thread flushes
    // with waitUntilScheduled every frame, and forces the per-surface event of a client that died or was suspended after 5 s
    // (CA::WindowServer::SharedEvent::is_complete_with_timeout): blocked here it never could (r7n: AegirPoster died, r7o:
    // PosterKit's scenes were suspended; the display froze for good).
    l.unlock();
    if (heldBehindEvent()) {
        gRM->stats.heldReturns++;
        once(@"waitUntilScheduled of a command buffer behind a shared event not reached yet: returns after 500 ms, as "
             @"Metal's "
             @"(its GPU waits for the event)");
        return;
    }
    l.lock();
    if (!_changed.wait_for(l, kWaitLimit, [&] {
            return _scheduledDone;
        }))
        waitTooLong("waitUntilScheduled of command buffer", (__bridge void *)self);
}

- (void)waitUntilCompleted
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed)
        return;
    if (!_changed.wait_for(l, kWaitLimit, [&] {
            return _completedDone;
        }))
        waitTooLong("waitUntilCompleted of command buffer", (__bridge void *)self);
}

// A selector a command buffer does not answer here: recorded for m2v (the command buffer goes there), answered zero.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = NSClassFromString(@"NBCommandBuffer");
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = NSClassFromString(@"NBCommandBuffer");
    return c && [c instancesRespondToSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    [inv retainArguments];
    [self retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    c.stage = 0xff;  // the command buffer's own, not an encoder's
    [self record:c];
    [self takeWholeReason:WhyCall why:@"a command buffer call recorded as an invocation"];
    hit(self, PredCallCBInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.cbinvocation", uintptr_t(inv.selector), "-[MTLCommandBuffer %s] answered zero",
                         sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

@end

@implementation RMQueue

+ (Class)twinClass
{
    return NSClassFromString(@"NBQueue");
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [gRM->fallback newCommandQueue]);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    RMCommandBuffer *b = [RMCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_scheduledHandlers = [NSMutableArray array];
    b->_completedHandlers = [NSMutableArray array];
    b->_status = MTLCommandBufferStatusNotEnqueued;
    b->_tape.reserve(512);
    b->_owners.reserve(256);
    b->_bytes.reserve(16384);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (raw_gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}

- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}

@end

// ---- object mapping for the m2v replay ----
#if SWITCH_FRAME_TIMELINE
static thread_local uint64_t tTwinTicks;
#endif
static inline id twinOf(id object)
{
    if (!object)
        return nil;
    if (![object isKindOfClass:[RMObject class]])
        return object;  // an m2v object already (events, fences, anything the device forwarded)
#if SWITCH_FRAME_TIMELINE
    if (!((RMObject *)object)->_twin) {
        const uint64_t began = mach_absolute_time();
        id twin = [(RMObject *)object rmTwin];
        tTwinTicks += mach_absolute_time() - began;
        gTwinsMade.fetch_add(1, std::memory_order_relaxed);
        return twin;
    }
#endif
    return [(RMObject *)object rmTwin];
}

static MTLRenderPassDescriptor *twinPass(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDescriptor *d = [pass copy];
    for (unsigned c = 0; c < 8; c++) {
        d.colorAttachments[c].texture = twinOf(pass.colorAttachments[c].texture);
        d.colorAttachments[c].resolveTexture = twinOf(pass.colorAttachments[c].resolveTexture);
    }
    d.depthAttachment.texture = twinOf(pass.depthAttachment.texture);
    d.depthAttachment.resolveTexture = twinOf(pass.depthAttachment.resolveTexture);
    d.stencilAttachment.texture = twinOf(pass.stencilAttachment.texture);
    d.stencilAttachment.resolveTexture = twinOf(pass.stencilAttachment.resolveTexture);
    d.visibilityResultBuffer = twinOf(pass.visibilityResultBuffer);
    return d;
}

// An invocation recorded on a facade encoder or command buffer, sent to the m2v one: object arguments become their twins.
static void replayInvocation(NSInvocation *inv, id target)
{
    NSMethodSignature *sig = inv.methodSignature;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = [sig getArgumentTypeAtIndex:i];
        while (*t == 'r' || *t == 'n' || *t == 'N' || *t == 'o' || *t == 'O' || *t == 'R' || *t == 'V')
            t++;
        if (*t == '@') {
            __unsafe_unretained id arg = nil;
            [inv getArgument:&arg atIndex:i];
            id mapped = twinOf(arg);
            if (mapped != arg) {
                __unsafe_unretained id m = mapped;
                [inv setArgument:&m atIndex:i];
            }
        } else if (t[0] == '^' && t[1] == '@') {
            // an array of objects (set...s:withRange:, useResources:count:): mapped in place over a copy the invocation keeps
            id __unsafe_unretained *array = nullptr;
            [inv getArgument:&array atIndex:i];
            NSUInteger count = 0;
            for (NSUInteger k = i + 1; k < sig.numberOfArguments && !count; k++) {
                const char *u = [sig getArgumentTypeAtIndex:k];
                if (u[0] == '{') {  // NSRange
                    NSRange r = {};
                    [inv getArgument:&r atIndex:k];
                    count = r.location + r.length;
                } else if (u[0] == 'Q' || u[0] == 'L' || u[0] == 'I') {
                    [inv getArgument:&count atIndex:k];
                }
            }
            if (array && count) {
                NSMutableData *copy = [NSMutableData dataWithLength:count * sizeof(id)];
                id __unsafe_unretained *out = (id __unsafe_unretained *)copy.mutableBytes;
                NSMutableArray *keep = [NSMutableArray array];
                for (NSUInteger k = 0; k < count; k++) {
                    id m = twinOf(array[k]);
                    if (m)
                        [keep addObject:m];
                    out[k] = m;
                }
                objc_setAssociatedObject(inv, (__bridge const void *)copy, @[ copy, keep ], OBJC_ASSOCIATION_RETAIN);
                id __unsafe_unretained *p = out;
                [inv setArgument:&p atIndex:i];
            }
        }
    }
    [inv invokeWithTarget:target];
}

static uint64_t extra(RMCommandBuffer *cb, uint64_t i)
{
    return cb->_extra[i];
}

static double extraDouble(RMCommandBuffer *cb, uint64_t i)
{
    double d;
    memcpy(&d, &cb->_extra[i], 8);
    return d;
}

static float extraFloat(RMCommandBuffer *cb, uint64_t i)
{
    const uint32_t u = uint32_t(cb->_extra[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// The tape into an m2v command buffer, every object its twin; the CPU lines of the buffers it names cleaned first.
// The tape [from, to) of `cb` as an m2v command buffer, not committed. `part`: a segment of a split command buffer (planSegments),
// whose encoders' waits for fences another segment updated are left out: the segments' order on the GPU covers them.
// A mipmapped texture in a replay (raw::Texture::chain): level 0 is the raw texture's own pages (m2v renders, copies and reads
// level 0 through its in-place twin), every other use is m2v's texture of every level (rmMipTwin), which the replay makes hold
// what the raw path's chain holds before an encoder uses it (mipSync); what m2v writes moves the raw path's states.
static inline bool chained(void *o)
{
    return o && [(__bridge id)o isKindOfClass:[RMTexture class]] && ((__bridge RMTexture *)o)->_raw.chain;
}

static id mipTwinOf(void *o, bool level0)
{
    RMTexture *t = (__bridge RMTexture *)o;
    return chained(o) && !level0 ? [t rmMipTwin] : twinOf((__bridge id)o);
}

// The textures of every level the encoder at `at` uses, current in m2v first (a blit encoder of their own before it): level 0
// copied from the raw texture where it was written since, the levels generated where the raw path generated them since (both
// from level 0, as m2v's own generateMipmaps makes them). A generateMipmaps needs level 0 only.
static void mipSync(RMCommandBuffer *cb, id<MTLCommandBuffer> m, size_t at, size_t to)
{
    std::vector<std::pair<RMTexture *, bool>> need;
    auto add = [&](void *o, bool levels) {
        if (!chained(o))
            return;
        RMTexture *t = (__bridge RMTexture *)o;
        for (auto &n : need)
            if (n.first == t) {
                n.second = n.second || levels;
                return;
            }
        need.push_back({t, levels});
    };
    // A pass into a later level needs m2v's levels current first, or the next sync would write over what it renders.
    if (cb->_tape[at].op == OpRender) {
        MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)cb->_tape[at].obj;
        for (unsigned j = 0; j < 8; j++)
            if (pass.colorAttachments[j].level)
                add((__bridge void *)pass.colorAttachments[j].texture, true);
    }
    for (size_t k = at + 1; k < to && cb->_tape[k].op != OpEnd; k++) {
        const Cmd &c = cb->_tape[k];
        switch (c.op) {
        case OpTexture:
            if (c.obj && [(__bridge id)c.obj isKindOfClass:[RMTexture class]]) {
                RMTexture *t = (__bridge RMTexture *)c.obj;
                add(t->_kind == RMTextureView && t->_parent ? (__bridge void *)t->_parent : c.obj, true);
            }
            break;
        case OpMipmaps:
            add(c.obj, false);
            break;
        case OpCopyTexture:
        case OpCopyTextureToBuffer:
            if (extra(cb, c.x + 1))
                add(c.obj, true);  // a level below the first, read
            break;
        default:
            break;
        }
    }
    bool any = false;
    for (auto &n : need)
        any = any || !n.first->_mipL0 || (n.second && !n.first->_mipLevels);
    if (!any)
        return;
    id<MTLBlitCommandEncoder> b = [m blitCommandEncoder];
    for (auto &n : need) {
        RMTexture *t = n.first;
        id<MTLTexture> all = [t rmMipTwin];
        if (!all)
            continue;
        if (!t->_mipL0) {
            [b copyFromTexture:(id<MTLTexture>)[t rmTwin]
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(t->_raw.width, t->_raw.height, 1)
                        toTexture:all
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            t->_mipL0 = true;
        }
        if (n.second && !t->_mipLevels) {
            // Levels the raw path rendered are not what generating gives: they come from the mirrors mirrorsForM2v filled
            // just before this command buffer. Otherwise the chain's levels are generated ones, and generating is exact.
            bool copied = false;
            if (t->_raw.chain->rawLevels && t->_mirrored) {
                copied = true;
                for (uint32_t l = 1; l < t->_raw.chain->layout.levels; l++) {
                    RMTexture *mirror = [t rmMirror:l];
                    id<MTLTexture> from = mirror ? (id<MTLTexture>)[mirror rmTwin] : nil;
                    if (!from) {
                        copied = false;
                        break;
                    }
                    const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[l];
                    [b copyFromTexture:from
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(lv.width, lv.height, 1)
                                toTexture:all
                         destinationSlice:0
                         destinationLevel:l
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                }
            }
            if (!copied)
                [b generateMipmapsForTexture:all];
            t->_mipLevels = true;
            t->_mirrored = false;
        }
    }
    [b endEncoding];
}

// m2v wrote level `level` of a mipmapped texture: level 0 (its in-place twin: the raw texture's pages) puts the chain's copy and
// m2v's own behind it; another level (m2v's texture of every level) goes to the level's mirror once the encoder ends
// (mirrorWrites). With no mirror the chain's levels are marked behind m2v's and are generated from level 0.
using MirrorWrites = std::vector<std::pair<RMTexture *, uint32_t>>;

static void mipWritten(void *o, uint64_t level, MirrorWrites *toMirrors)
{
    if (!chained(o))
        return;
    RMTexture *t = (__bridge RMTexture *)o;
    if (level) {
        if (toMirrors && level < t->_raw.chain->layout.levels && [t rmMirror:level])
            toMirrors->push_back({t, uint32_t(level)});
        else
            t->_raw.chain->levelsFresh = false;
    } else {
        t->_raw.chain->l0Fresh = false;
        t->_mipL0 = false;
    }
}

// Copies the levels the encoder that just ended wrote into their mirrors, in a blit encoder after it.
static void mirrorWrites(id<MTLCommandBuffer> m, MirrorWrites &writes)
{
    if (writes.empty())
        return;
    id<MTLBlitCommandEncoder> b = [m blitCommandEncoder];
    for (const auto &w : writes) {
        RMTexture *t = w.first, *mirror = [t rmMirror:w.second];
        id<MTLTexture> to = mirror ? (id<MTLTexture>)[mirror rmTwin] : nil, all = [t rmMipTwin];
        if (!to || !all) {
            t->_raw.chain->levelsFresh = false;
            continue;
        }
        const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[w.second];
        [b copyFromTexture:all
                  sourceSlice:0
                  sourceLevel:w.second
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(lv.width, lv.height, 1)
                    toTexture:to
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        t->_raw.chain->mirrorLevels |= uint16_t(1u << w.second);
        t->_mirrored = false;
    }
    [b endEncoding];
    writes.clear();
}

static id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part)
{
    id<MTLCommandQueue> queue = [cb->_queue rmTwin];
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::SlotStart);
#endif
    id<MTLCommandBuffer> m = [queue commandBuffer];
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::SlotEnd);
#endif
    if (!m)
        return nil;
    m.label = cb.label;
    id enc = nil;
    // What the CPU wrote that this work reads, cleaned to memory before m2v's submission (m2v cleans an import only when it
    // makes it): the ranges a draw of a pack state reads, as raw::RenderEncoder::prepare cleans them (its uniform windows, its
    // vertices, its indices), the ranges a copy reads (and writes: cleaned and invalidated, so no dirty line lands over the
    // GPU's bytes); a whole buffer only where the tape does not say (a state not in the pack, a compute or other recorded call,
    // a resource it uses)
    std::set<RMBuffer *> buffers;  // whole

    struct Range {
        const uint8_t *at;
        uint64_t bytes;
        bool flush;
    };

    std::vector<Range> ranges;
    auto range = [&](RMBuffer *b, uint64_t offset, uint64_t bytes, bool flush = false) {
        if (!b || offset >= b->_raw.length || b->_raw.wc)
            return;  // (write-combined: in memory once the DSB below drains it)
        ranges.push_back({b->_raw.cpu + offset, std::min(bytes, b->_raw.length - offset), flush});
    };
    auto use = [&](void *o) -> id {
        id x = (__bridge id)o;
        if ([x isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)x);
        return twinOf(x);
    };
    // a render encoder's bindings, for its draws' ranges
    RMBuffer *bound[3][31] = {};
    uint64_t boundOffset[3][31] = {};
    RMPipeline *pipe = nil;
    auto drawn = [&](uint32_t first, uint32_t count) {  // the ranges a draw of `count` vertices from `first` reads
        if (!pipe || !pipe->_raw) {                     // not the pack's: every buffer bound, whole
            for (auto &stage : bound)
                for (RMBuffer *b : stage)
                    if (b)
                        buffers.insert(b);
            return;
        }
        const raw::PackPipeline &r = *pipe->_raw->rec;
        for (uint32_t i = 0; i < r.entries; i++) {
            const raw::PackEntry &e = r.entry[i];
            if (e.kind != raw::EntryUbo || e.stage > 2 || e.slot > 30)
                continue;
            RMBuffer *b = bound[e.stage][e.slot];
            if (!b)
                continue;
            const uint64_t at = boundOffset[e.stage][e.slot], size = b->_raw.length - std::min(at, b->_raw.length);
            const uint64_t window = (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16;
            uint64_t bytes = std::min<uint64_t>((size + 15) & ~uint64_t(15), 0x10000);
            if (window && window < bytes)
                bytes = window;
            range(b, at, bytes);
        }
        if (r.vertexSlot <= 30 && r.vertexStride)
            if (RMBuffer *b = bound[1][r.vertexSlot])
                range(b, boundOffset[1][r.vertexSlot] + uint64_t(first) * r.vertexStride,
                      uint64_t(count) * r.vertexStride);
    };
    std::set<void *> updated;  // fences this part updated
    MirrorWrites toMirrors;    // later levels of mipmapped textures the open encoder writes (mipWritten)
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj, *twin = twinPass(pass);
            if (cb->_chains) {
                mipSync(cb, m, at, to);
                for (unsigned j = 0; j < 8; j++) {  // a mipmapped attachment: level 0 in place, another level m2v's own
                    MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[j];
                    if (!chained((__bridge void *)a.texture))
                        continue;
                    if (a.level)
                        twin.colorAttachments[j].texture = [(RMTexture *)a.texture rmMipTwin];
                    mipWritten((__bridge void *)a.texture, a.level, &toMirrors);
                }
            }
            enc = [m renderCommandEncoderWithDescriptor:twin];
            memset(bound, 0, sizeof bound);
            pipe = nil;
            break;
        }
        case OpBlit:
            if (cb->_chains)
                mipSync(cb, m, at, to);
            enc = [m blitCommandEncoder];
            break;
        case OpCompute:
            if (cb->_chains)
                mipSync(cb, m, at, to);
            enc = [m computeCommandEncoderWithDispatchType:MTLDispatchType(c.a)];
            break;
        case OpEnd:
            [enc endEncoding];
            enc = nil;
            mirrorWrites(m, toMirrors);
            break;
        case OpPipeline:
            pipe = [(__bridge id)c.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)c.obj : nil;
            [enc setRenderPipelineState:twinOf((__bridge id)c.obj)];
            break;
        case OpBytes:
            if (c.stage <= 2 && c.index <= 30)
                bound[c.stage][c.index] = nil;
            if (c.stage == 1)
                [enc setVertexBytes:cb->_bytes.data() + c.a length:c.b atIndex:c.index];
            else
                [enc setFragmentBytes:cb->_bytes.data() + c.a length:c.b atIndex:c.index];
            break;
        case OpBuffer: {
            id x = (__bridge id)c.obj;
            if (c.stage <= 2 && c.index <= 30) {
                bound[c.stage][c.index] = [x isKindOfClass:[RMBuffer class]] ? (RMBuffer *)x : nil;
                boundOffset[c.stage][c.index] = c.c;
            }
            if (c.stage == 1)
                [enc setVertexBuffer:twinOf(x) offset:c.c atIndex:c.index];
            else
                [enc setFragmentBuffer:twinOf(x) offset:c.c atIndex:c.index];
            break;
        }
        case OpBufferOffset:
            if (c.stage <= 2 && c.index <= 30)
                boundOffset[c.stage][c.index] = c.c;
            if (c.stage == 1)
                [enc setVertexBufferOffset:c.c atIndex:c.index];
            else
                [enc setFragmentBufferOffset:c.c atIndex:c.index];
            break;
        case OpTexture:
            if (c.stage == 1)
                [enc setVertexTexture:mipTwinOf(c.obj, false) atIndex:c.index];
            else
                [enc setFragmentTexture:mipTwinOf(c.obj, false) atIndex:c.index];
            break;
        case OpSampler:
            if (c.stage == 1)
                [enc setVertexSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            else
                [enc setFragmentSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            break;
        case OpScissor:
            [enc setScissorRect:(MTLScissorRect){c.a, c.b, NSUInteger(c.c >> 32), NSUInteger(uint32_t(c.c))}];
            break;
        case OpViewport:
            [enc setViewport:(MTLViewport){extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                                           extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4),
                                           extraDouble(cb, c.x + 5)}];
            break;
        case OpDraw:
            drawn(c.a, c.b);
            [enc drawPrimitives:MTLPrimitiveType(c.stage)
                    vertexStart:c.a
                    vertexCount:c.b
                  instanceCount:NSUInteger(c.c >> 32)
                   baseInstance:NSUInteger(uint32_t(c.c))];
            break;
        case OpCull:
            [enc setCullMode:MTLCullMode(c.a)];
            break;
        case OpWinding:
            [enc setFrontFacingWinding:MTLWinding(c.a)];
            break;
        case OpDepthStencil:
            [enc setDepthStencilState:twinOf((__bridge id)c.obj)];
            break;
        case OpFillMode:
            [enc setTriangleFillMode:MTLTriangleFillMode(c.a)];
            break;
        case OpBlendColor:
            [enc setBlendColorRed:extraFloat(cb, c.x)
                            green:extraFloat(cb, c.x + 1)
                             blue:extraFloat(cb, c.x + 2)
                            alpha:extraFloat(cb, c.x + 3)];
            break;
        case OpStencilRef:
            [enc setStencilFrontReferenceValue:c.a backReferenceValue:c.b];
            break;
        case OpDepthBias:
            [enc setDepthBias:extraFloat(cb, c.x) slopeScale:extraFloat(cb, c.x + 1) clamp:extraFloat(cb, c.x + 2)];
            break;
        case OpBarrier:
            [enc memoryBarrierWithScope:MTLBarrierScope(c.a)
                            afterStages:MTLRenderStages(c.b)
                           beforeStages:MTLRenderStages(c.c)];
            break;
        case OpUse:
            [enc useResource:use(c.obj) usage:MTLResourceUsage(c.a) stages:MTLRenderStages(c.b)];
            break;
        case OpUpdateFence:
            updated.insert(c.obj);
            [enc updateFence:(__bridge id<MTLFence>)c.obj];
            break;
        case OpWaitFence:
            if (!part || updated.count(c.obj))
                [enc waitForFence:(__bridge id<MTLFence>)c.obj];
            break;
        case OpCopyTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 10);
            mipWritten(c.obj2, extra(cb, x + 9), &toMirrors);
            [enc copyFromTexture:mipTwinOf(c.obj, !extra(cb, x + 1))
                      sourceSlice:extra(cb, x)
                      sourceLevel:extra(cb, x + 1)
                     sourceOrigin:MTLOriginMake(extra(cb, x + 2), extra(cb, x + 3), extra(cb, x + 4))
                       sourceSize:MTLSizeMake(extra(cb, x + 5), extra(cb, x + 6), extra(cb, x + 7))
                        toTexture:mipTwinOf(c.obj2, !extra(cb, x + 9))
                 destinationSlice:extra(cb, x + 8)
                 destinationLevel:extra(cb, x + 9)
                destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 11))];
            break;
        }
        case OpCopyBuffer:
            range((__bridge RMBuffer *)c.obj, c.c, extra(cb, c.x + 1));
            range((__bridge RMBuffer *)c.obj2, extra(cb, c.x), extra(cb, c.x + 1), true);
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                     sourceOffset:c.c
                         toBuffer:twinOf((__bridge id)c.obj2)
                destinationOffset:extra(cb, c.x)
                             size:extra(cb, c.x + 1)];
            break;
        case OpCopyBufferToTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 8);
            {  // the rows it reads: bytesPerRow apart, the last one's texels (a whole buffer where the texture is not the raw
                // path's: its texel size unknown here)
                RMBuffer *b = (__bridge RMBuffer *)c.obj;
                RMTexture *t = (__bridge RMTexture *)c.obj2;
                const uint64_t rows = extra(cb, x + 4) * std::max<uint64_t>(extra(cb, x + 5), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x), extra(cb, x + 1) * (rows - 1) + extra(cb, x + 3) * t->_raw.bpp);
                else
                    use(c.obj);
            }
            mipWritten(c.obj2, extra(cb, x + 7), &toMirrors);
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                       sourceOffset:extra(cb, x)
                  sourceBytesPerRow:extra(cb, x + 1)
                sourceBytesPerImage:extra(cb, x + 2)
                         sourceSize:MTLSizeMake(extra(cb, x + 3), extra(cb, x + 4), extra(cb, x + 5))
                          toTexture:mipTwinOf(c.obj2, !extra(cb, x + 7))
                   destinationSlice:extra(cb, x + 6)
                   destinationLevel:extra(cb, x + 7)
                  destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 9))];
            break;
        }
        case OpCopyTextureToBuffer: {
            const uint64_t x = c.x, o = extra(cb, x + 2);
            {  // the rows it writes, cleaned and invalidated (a whole buffer where the texel size is unknown here)
                RMTexture *t = (__bridge RMTexture *)c.obj;
                RMBuffer *b = (__bridge RMBuffer *)c.obj2;
                const uint64_t rows = extra(cb, x + 5) * std::max<uint64_t>(extra(cb, x + 6), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x + 7), extra(cb, x + 8) * (rows - 1) + extra(cb, x + 4) * t->_raw.bpp, true);
                else
                    use(c.obj2);
            }
            [enc copyFromTexture:mipTwinOf(c.obj, !extra(cb, x + 1))
                             sourceSlice:extra(cb, x)
                             sourceLevel:extra(cb, x + 1)
                            sourceOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 3))
                              sourceSize:MTLSizeMake(extra(cb, x + 4), extra(cb, x + 5), extra(cb, x + 6))
                                toBuffer:use(c.obj2)
                       destinationOffset:extra(cb, x + 7)
                  destinationBytesPerRow:extra(cb, x + 8)
                destinationBytesPerImage:extra(cb, x + 9)];
            break;
        }
        case OpMipmaps:
            if (chained(
                    c.obj)) {  // m2v's texture of every level (mipSync made its level 0 current): the chain's are behind it
                RMTexture *t = (__bridge RMTexture *)c.obj;
                [enc generateMipmapsForTexture:[t rmMipTwin]];
                t->_mipLevels = true;
                t->_raw.chain->levelsFresh = false;
                t->_raw.chain->mirrorLevels = 0;  // every level regenerated: no mirror is newer
            } else {
                [enc generateMipmapsForTexture:twinOf((__bridge id)c.obj)];
            }
            break;
        case OpInvocation: {
            NSInvocation *inv = (__bridge NSInvocation *)c.obj;
            for (NSUInteger i = 2; i < inv.methodSignature.numberOfArguments;
                 i++) {  // buffers it names get cleaned too
                const char *t = [inv.methodSignature getArgumentTypeAtIndex:i];
                if (*t != '@')
                    continue;
                __unsafe_unretained id arg = nil;
                [inv getArgument:&arg atIndex:i];
                if ([arg isKindOfClass:[RMBuffer class]])
                    buffers.insert((RMBuffer *)arg);
            }
            replayInvocation(inv, c.stage == 0xff ? m : enc);
            break;
        }
        case OpSignalEvent:
            [m encodeSignalEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpWaitEvent:
            // a value raw work submitted before signals: the BeforeTask acquire orders this command buffer after it on the
            // GPU (the raw path signals its events from the CPU once complete, which m2v would wait for on the CPU)
            if (!(gRM->gpuOrder && signalled(gRM->rawSignals, c.obj) >= c.c))
                [m encodeWaitForEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpDrawIndexed: {
            // its indices, and the vertices up to the largest (a restart reads none), as the raw path's indexed draw cleans
            RMBuffer *ib = [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? (__bridge RMBuffer *)c.obj : nil;
            const uint32_t size = c.b == MTLIndexTypeUInt16 ? 2 : 4;
            if (ib && pipe && pipe->_raw && !extra(cb, c.x + 1) && c.c + uint64_t(c.a) * size <= ib->_raw.length) {
                range(ib, c.c, uint64_t(c.a) * size);
                // the largest index only sizes the vertices to clean: none in write-combined memory (r17), whose indices the
                // CPU would read uncached, one load each (raw::RenderEncoder::drawIndexed's rule)
                const raw::PackPipeline &rec = *pipe->_raw->rec;
                RMBuffer *vb = rec.vertexSlot <= 30 ? bound[1][rec.vertexSlot] : nil;
                uint32_t most = 0;
                if (vb && !vb->_raw.wc && rec.vertexStride)
                    for (uint32_t k = 0; k < c.a; k++) {
                        uint32_t v = 0;
                        memcpy(&v, ib->_raw.cpu + c.c + uint64_t(k) * size, size);
                        if (v != (size == 2 ? 0xffffu : 0xffffffffu))
                            most = std::max(most, v);
                    }
                drawn(0, c.a ? most + 1 : 0);
            } else {
                if (ib)
                    buffers.insert(ib);
                for (auto &stage : bound)
                    for (RMBuffer *b : stage)
                        if (b)
                            buffers.insert(b);
            }
        }
            [enc drawIndexedPrimitives:MTLPrimitiveType(c.stage)
                            indexCount:c.a
                             indexType:MTLIndexType(c.b)
                           indexBuffer:twinOf((__bridge id)c.obj)
                     indexBufferOffset:c.c
                         instanceCount:extra(cb, c.x)
                            baseVertex:NSInteger(extra(cb, c.x + 1))
                          baseInstance:extra(cb, c.x + 2)];
            break;
        case OpVisibility:
            [enc setVisibilityResultMode:MTLVisibilityResultMode(c.a) offset:c.c];
            break;
        }
    }
    if (enc)
        [enc endEncoding];
    // what the CPU wrote that it reads, to memory (above): the ranges, then the whole buffers
    uint64_t cleaned = 0;
    for (const Range &r : ranges) {
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(r.at) & ~uintptr_t(63); a < uintptr_t(r.at) + r.bytes; a += 64) {
            if (r.flush)
                __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
            else
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
        }
#endif
        cleaned += r.bytes;
    }
    for (RMBuffer *b : buffers) {
        if (!b || b->_raw.wc)
            continue;
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(b->_raw.cpu) & ~uintptr_t(63); a < uintptr_t(b->_raw.cpu) + b->_raw.length;
             a += 64)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
        cleaned += b->_raw.length;
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    gRM->stats.replayClean += cleaned;
    return m;
}

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then
// The host imports a texture's memory lies in, named to the submission (raw::CommandBuffer::use): a surface's or a planar
// texture's planes, a buffer texture's buffer's, a view's parent's; and a YCbCr texture's planes, which only its conversion
// reads (its image is pool memory).
static void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (!t)
        return;
    rc->use(t->_buffer ? t->_buffer->_raw.import : t->_raw.import, write);
    for (const raw::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        if (plane)
            rc->use(plane->import, false);
}

static void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write)
{
    if (b)
        rc->use(b->_raw.import, write);
}

static bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why)
{
    raw::Device &d = *gRM->raw;
    raw::CommandBuffer *rc = d.begin();
    raw::RenderEncoder *re = nullptr;
    raw::BlitEncoder *be = nullptr;

    // mipmapped textures' states this encode changes (raw::Texture::Chain, RMTexture's _mipL0/_mipLevels): put back when it is
    // abandoned (its work never runs; the command buffer or segment goes to m2v, whose replay keeps them from what ran)
    struct ChainState {
        RMTexture *t;
        bool l0, levels, mipL0, mipLevels;
        uint16_t rawLevels, mirrorLevels;
    };

    std::vector<ChainState> chainStates;
    auto touch = [&](RMTexture *t) -> raw::Texture::Chain & {
        raw::Texture::Chain &c = *t->_raw.chain;
        for (const ChainState &k : chainStates)
            if (k.t == t)
                return c;
        chainStates.push_back({t, c.l0Fresh, c.levelsFresh, t->_mipL0, t->_mipLevels, c.rawLevels, c.mirrorLevels});
        return c;
    };
    auto untouch = [&]() {
        for (const ChainState &k : chainStates) {
            k.t->_raw.chain->l0Fresh = k.l0;
            k.t->_raw.chain->levelsFresh = k.levels;
            k.t->_mipL0 = k.mipL0;
            k.t->_mipLevels = k.mipLevels;
            k.t->_raw.chain->rawLevels = k.rawLevels;
            k.t->_raw.chain->mirrorLevels = k.mirrorLevels;
        }
    };
    // a chain as sampling or a copy of a level needs it: its levels generated again where m2v generated them last (its own
    // texture of every level's), else its level 0 refreshed where level 0 was written since; then the levels m2v rendered
    // since, from their mirrors (after the generate, which would write over them)
    auto current = [&](RMTexture *t, raw::BlitEncoder *b) {
        raw::Texture::Chain &c = touch(t);
        const uint16_t mirrored = c.mirrorLevels;
        if (!c.levelsFresh)
            b->generate(t->_raw, true);
        else if (!c.l0Fresh)
            b->generate(t->_raw, false);
        for (uint32_t l = 1; l < c.layout.levels; l++)
            if (mirrored & (1u << l))
                if (RMTexture *mirror = [t rmMirror:l]) {
                    b->copyToLevel(t->_raw, l, mirror->_raw);
                    c.rawLevels |= uint16_t(1u << l);
                    d.stats.mirrorsIn++;
                }
        c.mirrorLevels = 0;
    };
    auto fail = [&](RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0) {
        *why = w;
        hit(cb, p, a, b);
        rc->abandon();
        untouch();
        return false;
    };
    // a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
    auto refused = [&]() {
        static const RMPred map[] = {PredEncPassRender, PredEncIndexedRange, PredEncBaseInstance, PredEncColorFormats,
                                     PredEncUboOffset,  PredEncNoTic,        PredEncDepthStencil};
        const raw::Refusal &f = rc->refusal;
        hit(cb, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
        rc->abandon();
        untouch();
        return false;
    };
    // the planar textures its draws sample: their images from their planes first, once (m2v converts at each encoder that uses
    // one; nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's
    // full barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
    // image is made when a draw samples it. A sampler that clamps to zero or to the border reads a 4:4:4 code texture as clamp to
    // edge (m2v's address map), which the raw path does not derive: such a command buffer is m2v's.
    std::vector<std::pair<RMTexture *, uint32_t>> converted;  // planar images converted here, at their surfaces' seeds
    {
        std::vector<RMTexture *> planar;
        std::vector<RMSampler *> samplers;
        auto isPlanar = [](void *o) {
            return o && ((__bridge RMTexture *)o)->_raw.converted();
        };
        RMTexture *bound[16] = {};  // the open render encoder's planar fragment textures
        RMPipeline *pipe = nil;
        for (size_t at = from; at < to; at++) {
            const Cmd &c = cb->_tape[at];
            // Metal's copies of a planar texture move its planes' bytes, not the image: m2v's
            if ((c.op == OpCopyTexture && (isPlanar(c.obj) || isPlanar(c.obj2))) ||
                (c.op == OpCopyBufferToTexture && isPlanar(c.obj2)) ||
                (c.op == OpCopyTextureToBuffer && isPlanar(c.obj)))
                return fail(PredEncPlanarCopy, "a copy to or from a planar texture", c.op);
            if (c.op == OpRender) {
                std::fill(std::begin(bound), std::end(bound), nil);
                pipe = nil;
            } else if (c.op == OpPipeline) {
                pipe = [(__bridge id)c.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)c.obj : nil;
            } else if (c.op == OpTexture && c.stage == 2 && c.index < 16) {
                bound[c.index] = isPlanar(c.obj) ? (__bridge RMTexture *)c.obj : nil;
            } else if ((c.op == OpDraw || c.op == OpDrawIndexed) && pipe && pipe->_raw) {
                const raw::PackPipeline &r = *pipe->_raw->rec;
                for (uint32_t e = 0; e < r.entries; e++) {
                    RMTexture *t =
                        r.entry[e].kind == raw::EntryTexture && r.entry[e].slot < 16 ? bound[r.entry[e].slot] : nil;
                    if (t && std::find(planar.begin(), planar.end(), t) == planar.end())
                        planar.push_back(t);
                }
            } else if (c.op == OpSampler && c.obj) {
                RMSampler *sm = (__bridge RMSampler *)c.obj;
                if (std::find(samplers.begin(), samplers.end(), sm) == samplers.end())
                    samplers.push_back(sm);
            }
        }
        if (!planar.empty()) {
            // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
            for (RMTexture *t : planar)
                for (RMSampler *sm : samplers)
                    if (t->_raw.addressMap && !d.samplerVariant(sm->_raw, t->_raw.addressMap))
                        return fail(PredEncPlanarClamp, "no sampler header for an address map");
            // an image converted already from its surface at the surface's seed now is not converted again: the work that
            // converted it went before on this channel (the decoder and ImageIO write a surface under a write lock, which
            // moves its seed). The new seeds take effect once this command buffer is submitted.
            std::vector<RMTexture *> stale;
            for (RMTexture *t : planar) {
                const uint32_t seed = t->_surface ? IOSurfaceGetSeed(t->_surface) : 0;
                if (!t->_surface || !t->_imageCurrent || t->_imageSeed != seed) {
                    stale.push_back(t);
                    converted.push_back({t, seed});
#if SWITCH_FRAME_TIMELINE
                    traceSurfaceImage(t, "converted", seed);
#endif
                }
            }
            planar.swap(stale);
            // 4:4:4 code textures: two copy-engine remaps each
            bool copies = false;
            for (RMTexture *t : planar)
                copies |= t->_raw.planar;
            if (copies) {
                raw::BlitEncoder *conv = rc->blit();
                for (RMTexture *t : planar)
                    if (t->_raw.planar)
                        conv->convertPlanar(t->_raw);
                conv->end();
            }
            // YCbCr textures' planes whose rows or offset are 16 bytes apart but not 32 (ImageIO's photo thumbnails, a width of
            // 360: rows of 368) are sampled through pool images (raw::Texture::sampled, createSurfaceTexture): filled from the
            // planes by the copy engine first, as refresh() fills a sampled surface's (r7u: without it the conversion read
            // whatever the pool held, magenta and green stripes of older images)
            raw::BlitEncoder *fill = nullptr;
            for (RMTexture *t : planar)
                if (t->_raw.ycbcr)
                    for (const raw::Texture *x : {t->_raw.luma, t->_raw.chroma})
                        if (x && x->sampled) {
                            if (!fill)
                                fill = rc->blit();
                            fill->copyRect(x->va, x->pitch, x->sampled->va, x->sampled->pitch, x->width, x->height,
                                           x->bpp);
                        }
            if (fill)
                fill->end();
            // YCbCr textures: a pass of the pack's conversion state each, into the image the draws sample
            for (RMTexture *t : planar) {
                raw::Texture &x = t->_raw;
                if (!x.ycbcr)
                    continue;
                raw::PassDesc p;
                p.color[0].texture = &x;
                p.color[0].load = raw::ColorAttachment::DontCare;
                p.color[0].store = true;
                p.count = 1;
                raw::RenderEncoder *conv = rc->render(p);
                if (!conv)
                    return fail(PredEncPassRender, "a YCbCr texture's image the raw path cannot render");
                conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
                conv->setBytes(2, 0, x.shape, sizeof x.shape);
                conv->setTexture(0, x.luma);
                conv->setTexture(1, x.chroma ? x.chroma : x.luma);  // (over plane 1: chroma codes 0, none read)
                conv->setViewport(0, 0, x.width, x.height, 0, 1);
                conv->setScissor(0, 0, x.width, x.height);
                if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                    return refused();
                conv->end();
            }
        }
    }
    // textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
    // copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
    // its shadow at each encoder that uses it once written since)
    std::set<RMTexture *> fresh;
    raw::Texture levelTargets[8];  // the open pass's level targets: the encoder reads them until its end()

    struct LevelCopy {
        RMTexture *t;
        uint32_t level;
        RMTexture *mirror;
    };

    std::vector<LevelCopy> afterPass;  // the open pass's levels rendered into their mirrors
    uint64_t levelPasses = 0;
    static std::atomic<bool> loggedLevel{false};
    auto refresh = [&](size_t pass) {
        for (size_t at = pass + 1; at < to && cb->_tape[at].op != OpEnd; at++) {
            const Cmd &c = cb->_tape[at];
            if (c.op != OpTexture || !c.obj)
                continue;
            RMTexture *t = (__bridge RMTexture *)c.obj;
            if (!t->_raw.sampled || !fresh.insert(t).second)
                continue;
            const raw::Texture &x = t->_raw;
            raw::BlitEncoder *b = rc->blit();
            b->copyRect(x.va, x.pitch, x.sampled->va, x.sampled->pitch, x.width, x.height, x.bpp);
            b->end();
        }
    };
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            if (cb->_sampledShadows)
                refresh(at);
            if (cb->_chains) {  // the chains this pass samples, current first (one blit of the 2D engine's work)
                raw::BlitEncoder *b = nullptr;
                auto stale = [](const raw::Texture::Chain *c) {
                    return c && (!c->l0Fresh || !c->levelsFresh || c->mirrorLevels);
                };
                for (size_t k = at + 1; k < to && cb->_tape[k].op != OpEnd; k++) {
                    const Cmd &u = cb->_tape[k];
                    if (u.op != OpTexture || !u.obj)
                        continue;
                    RMTexture *t = (__bridge RMTexture *)u.obj;
                    if (!stale(t->_raw.chain))
                        continue;
                    if (!b)
                        b = rc->blit();
                    current(t, b);
                }
                // a level this pass renders: the chain current first, or a later generate from level 0 or copy from a
                // mirror would write over what the pass leaves there
                for (unsigned j = 0; j < 8; j++) {
                    MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[j];
                    RMTexture *t = (RMTexture *)a.texture;
                    if (!t || !a.level || !stale(t->_raw.chain))
                        continue;
                    if (!b)
                        b = rc->blit();
                    current(t, b);
                }
                if (b)
                    b->end();
            }
            // One memory layout for every color target of a pass (NVK renders a pass's linear targets through tiled
            // shadows once any is tiled): a level beside a pitch target renders into the level's mirror, which holds the
            // whole level from the chain before the pass (texels outside what the pass writes go back unchanged) and is
            // copied into the chain after it.
            bool levels = false, pitch = false;
            for (unsigned j = 0; j < 8; j++) {
                MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[j];
                RMTexture *t = (RMTexture *)a.texture;
                if (!t)
                    continue;
                if (t->_raw.chain && a.level)
                    levels = true;
                else
                    pitch = true;
            }
            const bool viaMirror = levels && pitch;
            raw::BlitEncoder *loads = nullptr;
            raw::PassDesc p;
            for (unsigned j = 0; j < 8; j++) {
                MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[j];
                RMTexture *t = (RMTexture *)a.texture;
                if (!t)
                    continue;
                if (t->_raw.planar)
                    return fail(PredEncPlanarRT, "a planar texture as a render target", j);
                useTexture(rc, t, true);
                if (!fresh.empty())
                    fresh.erase(t);  // this pass writes it
                if (t->_raw.chain && a.level) {
                    // the chain keeps the texture's levels (levelsFresh), m2v's are behind, and they are no longer what
                    // generating from level 0 gives (rawLevels)
                    if (viaMirror) {
                        RMTexture *mirror = [t rmMirror:a.level];
                        if (!mirror)
                            return fail(PredEncPassRender, "a mip level beside a pitch target, with no mirror",
                                        a.level);
                        levelTargets[j] = mirror->_raw;
                        if (!loads)
                            loads = rc->blit();
                        const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[a.level];
                        loads->copyFromLevel(t->_raw, uint32_t(a.level), 0, 0, mirror->_raw, 0, 0, lv.width, lv.height);
                        afterPass.push_back({t, uint32_t(a.level), mirror});
                    } else if (!raw::chainLevelTarget(t->_raw, uint32_t(a.level), levelTargets[j])) {
                        return fail(PredEncPassRender, "a mip level the raw path cannot render to", a.level);
                    } else {
                        levelTargets[j].tic = gRM->raw->levelTic(t->_raw, uint32_t(a.level));
                    }
                    raw::Texture::Chain &c = touch(t);
                    c.rawLevels |= uint16_t(1u << a.level);
                    t->_mipLevels = false;
                    t->_mirrored = false;
                    levelPasses++;
                    if (!loggedLevel.exchange(true))
                        os_log(rawLog(), "raw: renders into mip levels (first: level %lu of %ux%u format %u)",
                               (unsigned long)a.level, t->_raw.width, t->_raw.height, t->_raw.format);
                } else if (t->_raw.chain) {  // its level 0: the chain's copy and m2v's are behind it
                    touch(t).l0Fresh = false;
                    t->_mipL0 = false;
                }
                p.color[j].texture = a.level ? &levelTargets[j] : &t->_raw;
                p.color[j].load = a.loadAction == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                                  : a.loadAction == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                                      : raw::ColorAttachment::DontCare;
                p.color[j].store =
                    a.storeAction == MTLStoreActionStore || a.storeAction == MTLStoreActionStoreAndMultisampleResolve;
                const MTLClearColor k = a.clearColor;
                p.color[j].clear[0] = float(k.red);
                p.color[j].clear[1] = float(k.green);
                p.color[j].clear[2] = float(k.blue);
                p.color[j].clear[3] = float(k.alpha);
                p.count = j + 1;
            }
            if (RMTexture *z = (RMTexture *)pass.depthAttachment.texture) {  // (rawDepthStencil: decided at record)
                p.depthStencil.texture = &z->_raw;
                p.depthStencil.clearDepth = pass.depthAttachment.loadAction == MTLLoadActionClear;
                p.depthStencil.clearStencil = pass.stencilAttachment.loadAction == MTLLoadActionClear;
                p.depthStencil.depth = float(std::clamp(pass.depthAttachment.clearDepth, 0.0, 1.0));
                p.depthStencil.stencil = pass.stencilAttachment.clearStencil;
            }
            p.width = uint32_t(std::min<NSUInteger>(pass.renderTargetWidth, UINT32_MAX));
            p.height = uint32_t(std::min<NSUInteger>(pass.renderTargetHeight, UINT32_MAX));
            if (loads)
                loads->end();
            if (!(re = rc->render(p)))
                return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
            break;
        }
        case OpBlit:
            be = rc->blit();
            break;
        case OpEnd:
            if (re)
                re->end();
            if (be)
                be->end();
            if (re && !afterPass.empty()) {
                raw::BlitEncoder *x = rc->blit();
                for (const LevelCopy &l : afterPass)
                    x->copyToLevel(l.t->_raw, l.level, l.mirror->_raw);
                x->end();
                afterPass.clear();
            }
            re = nullptr;
            be = nullptr;
            break;
        case OpPipeline:
            re->setPipeline(((__bridge RMPipeline *)c.obj)->_raw);
            break;
        case OpBlendColor: {
            const float rgba[4] = {extraFloat(cb, c.x), extraFloat(cb, c.x + 1), extraFloat(cb, c.x + 2),
                                   extraFloat(cb, c.x + 3)};
            re->setBlendColor(rgba);
            break;
        }
        case OpBytes:
            re->setBytes(c.stage, c.index, cb->_bytes.data() + c.a, c.b);
            break;
        case OpDepthStencil:
            re->setDepthStencil(c.obj ? &((__bridge RMDepthStencil *)c.obj)->_raw : nullptr);
            break;
        case OpStencilRef:
            re->setStencilReference(c.a, c.b);
            break;
        case OpBuffer:
            useBuffer(rc, (__bridge RMBuffer *)c.obj, false);
            re->setBuffer(c.stage, c.index, c.obj ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr, c.c);
            break;
        case OpBufferOffset:
            re->setBufferOffset(c.stage, c.index, c.c);
            break;
        case OpTexture:
            if (c.stage == 2) {
                useTexture(rc, (__bridge RMTexture *)c.obj, false);
                re->setTexture(c.index, c.obj ? &((__bridge RMTexture *)c.obj)->_raw : nullptr);
            }
            break;
        case OpSampler:
            if (c.stage == 2)
                re->setSampler(c.index, c.obj ? &((__bridge RMSampler *)c.obj)->_raw : nullptr);
            break;
        case OpScissor:
            re->setScissor(c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c));
            break;
        case OpCull:
            re->setCullMode(uint8_t(c.a));
            break;
        case OpWinding:
            re->setWinding(uint8_t(c.a));
            break;
        case OpFillMode:
            re->setFillMode(uint8_t(c.a));
            break;
        case OpViewport:
            re->setViewport(extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                            extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4), extraDouble(cb, c.x + 5));
            break;
        case OpDraw:
            if (!re->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why))
                return refused();
            break;
        case OpBarrier:
            re->barrier();
            break;
        case OpDrawIndexed:
            useBuffer(rc, (__bridge RMBuffer *)c.obj, false);
            if (!re->drawIndexed(c.stage, c.a, c.b == MTLIndexTypeUInt16 ? 2 : 4, &((__bridge RMBuffer *)c.obj)->_raw,
                                 c.c, why))
                return refused();
            break;
        case OpCopyTexture: {
            RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            if (!fresh.empty())
                fresh.erase(t);
            const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1);
            const uint64_t sx = extra(cb, x + 2), sy = extra(cb, x + 3), w = extra(cb, x + 5), h = extra(cb, x + 6);
            const uint64_t dx = uint32_t(o), dy = o >> 32;
            if (sl) {  // a level below the first of a chain (decided at record): the chain current, then the 2D engine's copy
                current(s, be);
                be->copyFromLevel(s->_raw, uint32_t(sl), uint32_t(sx), uint32_t(sy), t->_raw, uint32_t(dx),
                                  uint32_t(dy), uint32_t(w), uint32_t(h));
            } else {
                be->copyTexels(s->_raw, uint32_t(sx), uint32_t(sy), t->_raw, uint32_t(dx), uint32_t(dy), uint32_t(w),
                               uint32_t(h));
            }
            if (t->_raw.chain) {  // its level 0 written
                touch(t).l0Fresh = false;
                t->_mipL0 = false;
            }
            break;
        }
        case OpMipmaps: {  // a chain's levels from level 0 (decided at record); m2v's texture of every level is behind it
            RMTexture *t = (__bridge RMTexture *)c.obj;
            useTexture(rc, t, true);
            touch(t);
            be->generate(t->_raw, true);
            t->_mipLevels = false;
            t->_mirrored = false;
            break;
        }
        case OpCopyBuffer: {
            RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
            useBuffer(rc, s, false);
            useBuffer(rc, t, true);
            const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
            if (!s->_raw.wc)
                rc->clean(s->_raw.cpu + c.c, n);
            if (!t->_raw.wc)
                rc->clean(t->_raw.cpu + dof, n, true);
            be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
            break;
        }
        case OpCopyBufferToTexture: {
            RMBuffer *s = (__bridge RMBuffer *)c.obj;
            RMTexture *t = (__bridge RMTexture *)c.obj2;
            useBuffer(rc, s, false);
            useTexture(rc, t, true);
            const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1);
            const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4);
            if (!fresh.empty())
                fresh.erase(t);
            if (t->_raw.chain) {
                touch(t).l0Fresh = false;
                t->_mipL0 = false;
            }
            BufferRows rows;
            bufferRows(so, row, uint32_t(h), uint32_t(w * t->_raw.bpp), t->_raw.bpp, true,
                       rows);  // (decided at record)
            const uint64_t first = bufferRow(rows, so, row, 0);
            if (!s->_raw.wc)
                rc->clean(s->_raw.cpu + first, bufferRow(rows, so, row, uint32_t(h - 1)) - first + w * t->_raw.bpp);
            copyRows(be, s->_raw.va, rows, so, row, t->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(w), uint32_t(h),
                     true);
            break;
        }
        case OpCopyTextureToBuffer: {
            RMTexture *s = (__bridge RMTexture *)c.obj;
            RMBuffer *t = (__bridge RMBuffer *)c.obj2;
            useTexture(rc, s, false);
            useBuffer(rc, t, true);
            const uint64_t x = c.x, o = extra(cb, x + 2), w = extra(cb, x + 4), h = extra(cb, x + 5),
                           dof = extra(cb, x + 7), row = extra(cb, x + 8);
            BufferRows rows;
            bufferRows(dof, row, uint32_t(h), uint32_t(w * s->_raw.bpp), s->_raw.bpp, false,
                       rows);  // (decided at record)
            const uint64_t first = bufferRow(rows, dof, row, 0);
            if (!t->_raw.wc)
                rc->clean(t->_raw.cpu + first, bufferRow(rows, dof, row, uint32_t(h - 1)) - first + w * s->_raw.bpp,
                          true);
            copyRows(be, t->_raw.va, rows, dof, row, s->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(w), uint32_t(h),
                     false);
            break;
        }
        default:  // states, fences, events, resources the raw path needs nothing for
            break;
        }
    }
    if (re)
        re->end();
    if (be)
        be->end();
    uint32_t seq = 0;
    if (!rc->commit(seq))
        return fail(PredEncKernel, "the kernel refused the submission");
    for (const auto &c : converted) {  // (submitted: their images hold the surfaces at these seeds)
        c.first->_imageSeed = c.second;
        c.first->_imageCurrent = true;
    }
    cb->_seqno = seq;
    d.stats.levelPasses += levelPasses;
    gRM->stats.draws += rc->draws;
    gRM->stats.passes += rc->passes;
    gRM->stats.copies += rc->blits;
    return true;
}

// The textures and buffers a command buffer wrote (render targets, copy destinations, and for m2v anything a compute encoder
// bound): their CPU lines are invalidated before the CPU next reads them.
// A recorded object can be m2v's own (a facade command buffer's copy that went to m2v keeps it): only the raw path's carry
// the flag
static void markDirty(void *o)
{
    id object = (__bridge id)o;
    if ([object isKindOfClass:[RMBuffer class]])
        ((RMBuffer *)object)->_gpuDirty = true;
    else if ([object isKindOfClass:[RMTexture class]])
        ((RMTexture *)object)->_gpuDirty = true;
}

static void markWritten(RMCommandBuffer *cb)
{
    bool compute = false;
    for (const Cmd &c : cb->_tape) {
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            for (unsigned j = 0; j < 8; j++)
                if (RMTexture *t = (RMTexture *)pass.colorAttachments[j].texture)
                    if ([t isKindOfClass:[RMTexture class]])
                        t->_gpuDirty = true;
            break;
        }
        case OpCompute:
            compute = true;
            break;
        case OpBlit:
            compute = false;
            break;
        case OpEnd:
            compute = false;
            break;
        case OpCopyTexture:
        case OpCopyBufferToTexture:
        case OpMipmaps:
            markDirty(c.op == OpMipmaps ? c.obj : c.obj2);
            break;
        case OpCopyBuffer:
        case OpCopyTextureToBuffer:
            markDirty(c.obj2);
            break;
        case OpBuffer:
        case OpTexture:
            if (compute)
                markDirty(c.obj);
            break;
        case OpInvocation: {  // compute and other recorded calls: every buffer and texture they name
            NSInvocation *inv = (__bridge NSInvocation *)c.obj;
            for (NSUInteger i = 2; i < inv.methodSignature.numberOfArguments; i++) {
                if (*[inv.methodSignature getArgumentTypeAtIndex:i] != '@')
                    continue;
                __unsafe_unretained id arg = nil;
                [inv getArgument:&arg atIndex:i];
                markDirty((__bridge void *)arg);
            }
            break;
        }
        default:
            break;
        }
    }
}

// Scheduled: handlers in order on the device's handler queue (never inside commit), then waitUntilScheduled returns.
// The committed command buffers that wait for a shared event, until scheduled (heldBehindEvent).
static std::mutex gHoldersLock;
static std::set<RMCommandBuffer *> gHolders;

static bool waitsForEvent(RMCommandBuffer *cb)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpWaitEvent)
            return true;
    return false;
}

// Whether a committed command buffer not yet scheduled waits for a shared event value nothing reached yet.
static bool heldBehindEvent()
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    for (RMCommandBuffer *cb : gHolders)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpWaitEvent && [(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] < c.c)
                return true;
    return false;
}

static void unhold(RMCommandBuffer *cb)
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    gHolders.erase(cb);
}

static void scheduled(RMCommandBuffer *cb)
{
    unhold(cb);
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_scheduledSent)
            return;  // once (eventHeld: at its commit; then its submission's)
        cb->_scheduledSent = true;
        if (cb->_status == MTLCommandBufferStatusCommitted)
            cb->_status = MTLCommandBufferStatusScheduled;
        if (cb->_scheduledHandlers.count) {
            handlers = [cb->_scheduledHandlers copy];
            [cb->_scheduledHandlers removeAllObjects];
        }
        cb->_gpuStart = hostSeconds();
        // Core Animation's render thread waits for this every frame (waitUntilScheduled): with nothing to run, no hop through
        // the handler queue
        if (!handlers.count)
            cb->_scheduledDone = true;
    }
    if (!handlers.count) {
        cb->_changed.notify_all();
        return;
    }
    dispatch_async(gRM->handlers, ^{
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_scheduledDone = true;
            // a handler added meanwhile ran at once (addScheduledHandler)
        }
        cb->_changed.notify_all();
    });
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
static void finish(RMCommandBuffer *cb, NSError *error)
{
    gRM->stats.finished.fetch_add(1, std::memory_order_relaxed);
    unhold(cb);
    markWritten(cb);
    if (!cb->_m2v)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpSignalEvent)
                [(id<MTLSharedEvent>)(__bridge id)c.obj setSignaledValue:c.c];
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        cb->_status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
        cb->_error = error;
        cb->_gpuEnd = hostSeconds();
        handlers = [cb->_completedHandlers copy];
        [cb->_completedHandlers removeAllObjects];
        cb->_completedSent = true;
        cb->_scheduledSent = true;  // a completed command buffer is scheduled; its own scheduled() runs no more
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_completedDone = true;
            cb->_scheduledDone = true;
        }
        cb->_changed.notify_all();
        cb->_owners.clear();  // the resources may die now: no submission uses them any more
        cb->_owners.shrink_to_fit();
#if SWITCH_FRAME_TIMELINE
        switch_frame::finished(cb->_frame);
#endif
    };
    dispatch_queue_t q = cb->_queue ? cb->_queue->_completionQueue : nil;
    if (q)
        dispatch_async(q, run);
    else
        run();
}

static void commitBuffer(RMCommandBuffer *cb);
@implementation RMDevice

- (void)rmCommit:(RMCommandBuffer *)cb
{
    commitBuffer(cb);
}

// Alignments Core Animation lays its memory out by (QuartzCore 21H461), answered for what the raw path takes as it is; m2v takes
// any multiple of 16 (its own answers), so the m2v twins of the same memory are unchanged in what they hold:
//  - its buffer pool's suballocations (CA::OGL::MetalBufferPool::get_buffer: max(this, the argument's)): a uniform buffer the
//    raw path binds at its address (a constant buffer's is a multiple of 256 on GM20B; m2v's residual is not in the pack);
//  (not iosurfaceReadOnlyTextureAlignmentBytes: QuartzCore makes it the render server's needed IOSurface row alignment,
//  CADeviceSetMinimumAlignment, and hands it to every client, CA::Context::connect_remote; MetalContext::update_surface then
//  refuses every surface whose rows miss it, "Row bytes (N) not 128 aligned", and the layer draws nothing (r7p5: 3347 refusals
//  in backboardd, Safari images and widgets blank). The m2v device's answer, 16, stands; rows the raw path cannot take stay
//  m2v's: its texture header takes rows of 32, a pitch render target rows of 128.)
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of this,
//    else it copies them into a texture of its own): the texture header's 32;
//  - the rows of its CPU readbacks (CAMetalTextureGetBytesPerRow).
- (NSUInteger)minConstantBufferAlignmentBytes
{
    return 256;
}

- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes
{
    return 32;
}

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    const NSUInteger a = [gRM->fallback minimumLinearTextureAlignmentForPixelFormat:format];
    return a ? std::max<NSUInteger>(a, 32) : a;
}

// (every other selector: the m2v device's own method, which the device class inherits)
- (id<MTLCommandQueue>)newCommandQueue
{
    RMQueue *q = [RMQueue new];
    q->_device = self;
    return (id<MTLCommandQueue>)q;
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
    (void)n;
    return [self newCommandQueue];
}

- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(id)d
{
    (void)d;
    return [self newCommandQueue];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    // a shared buffer: write-combined (raw::Buffer::wc), as every one Core Animation makes is written by the CPU and read by the
    // GPU (its buffer pool, its gamma and weight tables: QuartzCore MetalBufferPool::allocate_buffer, options 0): nothing to clean
    // before a draw reads it, nothing to scan for the vertices an indexed draw reads (r16: 600-1300 KB cleaned a command buffer)
    const bool wc = (o & MTLResourceStorageModeMask) == MTLResourceStorageModeShared;
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw,
                                wc)) {  // the pool could not grow: m2v's buffer (its users go to m2v)
        SWITCH_REFUSE("raw", "buffer.pool", "no raw memory for a %lu-byte buffer: m2v's", (unsigned long)n);
        if (!predLogged(PredCallBufForeign))
            logPred(PredCallBufForeign, 0, n, @"(made: the pool had no room)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithLength:n options:o];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (b && p && n) {
        memcpy(((RMBuffer *)b)->_raw.cpu, p, n);  // the draws that read it clean the lines they read
    }
    return b;
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p
                                   length:(NSUInteger)n
                                  options:(MTLResourceOptions)o
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    SWITCH_API(ApiObject);
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw)) {  // (not page-aligned, or refused): m2v's, as today
        if (!predLogged(PredCallBufForeign))
            logPred(PredCallBufForeign, uintptr_t(p) & 0xfff, n, @"(made: a no-copy buffer the raw path refused)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithBytesNoCopy:p length:n options:o deallocator:deallocator];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    b->_deallocator = [deallocator copy];
    return (id<MTLBuffer>)b;
}

- (id<MTLTexture>)wrapTwin:(id<MTLTexture>)twin descriptor:(MTLTextureDescriptor *)d
{
    if (!twin)
        return nil;
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_twin = twin;
    t->_kind = RMTextureTwinOnly;
    t->_descriptor = [d copy];
    return (id<MTLTexture>)t;
}

// A private texture the CPU never touches is block-linear (TegraGM20BLayout.h), as NVK lays out an optimal image and m2v's
// view of it is (texture_tiled_in_place).
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [self rmNewTexture:d tiled:d.storageMode == MTLStorageModePrivate];
}

- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled
{
    if (gRM->raw->chainsOn() && chainShape(d)) {  // mipmapped: level 0 a pool texture, the levels a chain (raw/fx)
        RMTexture *t = [RMTexture new];
        t->_device = self;
        t->_descriptor = [d copy];
        t->_kind = RMTexturePool;
        t->_rawOk = gRM->raw->createMipTexture(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                               uint32_t(d.mipmapLevelCount), t->_raw);
        if (t->_rawOk) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                os_log(rawLog(), "raw: mip chain on (%u levels of %ux%u, %llu bytes)", unsigned(d.mipmapLevelCount),
                       unsigned(d.width), unsigned(d.height), (unsigned long long)t->_raw.chain->layout.bytes);
            return (id<MTLTexture>)t;
        }
    }
    if (!rawShape(d) && !zetaDescriptor(d)) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w)
            refuseTexture(w, PredTexShape, rawShapeTerm(d), d.pixelFormat, @"twin-only", nil);
        return (id<MTLTexture>)w;
    }
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    if (d.storageMode == MTLStorageModeMemoryless) {
        t->_kind = RMTextureMemoryless;
        t->_rawOk = gRM->raw->createMemoryless(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw);
    } else {
        t->_kind = RMTexturePool;
        // a shared texture (the CPU writes it: replaceRegion) in write-combined memory: its rows need no clean after
        t->_rawOk = gRM->raw->createTexture(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw,
                                            d.storageMode == MTLStorageModeShared, tiled);
    }
    if (!t->_rawOk) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w)
            refuseTexture(w, PredTexPoolAlloc, d.width, d.height, @"twin-only (pool)", nil);
        return (id<MTLTexture>)w;
    }
    return (id<MTLTexture>)t;
}

// RGB8_444_2P (542) over plane 0 of a two-plane 4:4:4 surface of one byte a luma texel and two a chroma pair (CoreVideo's
// '_RB4', the widget and poster snapshots Core Animation samples): Metal reads its codes, red Cr, green Y, blue Cb, alpha 1
// (m2v ycbcr.rs), which the raw path holds in an RGBA8 image it writes from the planes with the copy engine before the work that
// samples it (raw::BlitEncoder::convertPlanar). A 2D texture of the surface's size, one level, no view, no rotation, sampled.
static bool planar444(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.pixelFormat == kRGB8_444_2P && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView)) &&
           IOSurfaceGetWidthOfPlane(s, 0) == d.width && IOSurfaceGetHeightOfPlane(s, 0) == d.height &&
           IOSurfaceGetWidthOfPlane(s, 1) == d.width && IOSurfaceGetHeightOfPlane(s, 1) == d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 1 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 2;
}

// A YCbCr texture's plan (raw_ycbcr.h): Metal reads it converted by the surface's range and matrix, that through the sRGB curve,
// or its codes, which the raw path draws into an image of its own with the pack's conversion state (pack/ycbcr.metal, exact
// against Metal's reads: test/ycbcroracle.mm) before the work that samples it. Null when taken, else the rule that refused it.
static const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    raw::YcbcrFacts f;
    f.pixelFormat = uint32_t(d.pixelFormat);
    f.fourcc = IOSurfaceGetPixelFormat(s);
    f.plane = uint32_t(plane);
    f.planes = uint32_t(IOSurfaceGetPlaneCount(s));
    f.width = uint32_t(d.width);
    f.height = uint32_t(d.height);
    f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2 && k < f.planes; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, k));
    }
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.simple = d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
               !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView));
    char matrix[32] = "";
    if (CFTypeRef value = IOSurfaceCopyValue(s, CFSTR("IOSurfaceYCbCrMatrix"))) {
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, matrix, sizeof matrix, kCFStringEncodingUTF8);
        CFRelease(value);
        f.matrix = matrix;
    }
    return raw::planYcbcr(f, out);
}

// Whether the GPU may write a texture of this descriptor (Unknown: any use)
static bool writesTexture(MTLTextureDescriptor *d)
{
    return d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

// Whether a texture of this descriptor is made to be rendered into or stored to, as the kernel's layout decision takes it
// (TegraGM20BLayout.h): a surface whose first texture says Unknown is left pitch, as the CPU may produce it
static bool producesTexture(MTLTextureDescriptor *d)
{
    return d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite);
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    SWITCH_API(ApiObject);
    if (!s)
        return nil;
#if SWITCH_FRAME_TIMELINE
    gSurfaceTextures.fetch_add(1, std::memory_order_relaxed);
#endif
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTextureSurface;
    t->_surface = (IOSurfaceRef)CFRetain(s);
    t->_plane = plane;
    raw::YcbcrPlan ycbcr;
    if (planar444(d, s, plane)) {  // RGB8_444_2P over a 4:4:4 two-plane surface: an RGBA8 image of its codes
        void *base = IOSurfaceGetBaseAddress(s);
        const uintptr_t at = uintptr_t(base);
        t->_rawOk = gRM->raw->createPlanarTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), uint32_t(d.width), uint32_t(d.height), t->_raw);
    } else if (raw::isYcbcrFormat(uint32_t(d.pixelFormat)) && !ycbcrPlan(d, s, plane, ycbcr)) {
        // a YCbCr texture: an image the pack's conversion state draws
        t->_rawOk =
            gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                         ycbcr, uint32_t(d.width), uint32_t(d.height), t->_raw);
    } else if (rawShape(d) && !plane && IOSurfaceGetPlaneCount(s) <= 1) {
        // a surface without planes, or one described by a single plane (CoreVideo's chunky surfaces): its plane's bytes
        void *base = IOSurfaceGetBaseAddress(s);  // valid while the surface lives (the import retains it)
        const bool one = IOSurfaceGetPlaneCount(s) == 1;
        const uint64_t offset = one ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - uintptr_t(base) : 0;
        const size_t pitch = one ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(pitch),
                                                   uint32_t(d.pixelFormat), t->_raw, producesTexture(d));
    }
    if (t->_rawOk && writesTexture(d))
        declareGpuWrites(t);
    if (!t->_rawOk) {  // a surface the raw path does not take: m2v's texture over it, made now (it may refuse: nil, as today)
        // which term (createSurfaceTexture's, recomputed here: a creation, not the hot path)
        const size_t planes = IOSurfaceGetPlaneCount(s),
                     pitch = planes == 1 ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
        const uintptr_t offset = planes == 1 ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - base : 0;
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        NSString *extra =
            [NSString stringWithFormat:@"surface %ux%u '%c%c%c%c' bytesPerRow %lu planes %lu plane %lu alloc %lu "
                                       @"base&0xfff 0x%lx",
                                       unsigned(IOSurfaceGetWidth(s)), unsigned(IOSurfaceGetHeight(s)),
                                       char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc),
                                       (unsigned long)pitch, (unsigned long)planes, (unsigned long)plane,
                                       (unsigned long)IOSurfaceGetAllocSize(s), (unsigned long)(base & 0xfff)];
        if (planar444(d, s, plane))
            refuseTexture(t, PredTexPlanar, d.width, d.height, @"surface", extra);
        else if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"surface", extra);
        else if (plane || planes > 1)
            refuseTexture(t, PredTexSurfPlane, plane, planes, @"surface", extra);
        else if ((pitch & 15) || (offset & 15) || pitch < packed)  // (its plane's offset: as its pitch)
            refuseTexture(t, PredTexSurfPitch, pitch, offset & 15 ? offset : packed, @"surface", extra);
        else if (uint64_t(pitch) * d.height > IOSurfaceGetAllocSize(s))
            refuseTexture(t, PredTexSurfBounds, uint64_t(pitch) * d.height, IOSurfaceGetAllocSize(s), @"surface",
                          extra);
        else if (base & (size_t(getpagesize()) - 1))
            refuseTexture(t, PredTexSurfBase, base & 0xfff, 0, @"surface", extra);
        else
            refuseTexture(t, PredTexSurfImport, 1, 0, @"surface", extra);
        if (![t rmTwin])
            return nil;
    }
#if SWITCH_FRAME_TIMELINE
    if (t->_raw.planar || t->_raw.ycbcr)
        traceSurfaceImage(t, "imported", IOSurfaceGetSeed(s));
#endif
    return (id<MTLTexture>)t;
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    SWITCH_API(ApiObject);
    raw::SamplerDesc s;
    s.minFilter = uint8_t(d.minFilter);
    s.magFilter = uint8_t(d.magFilter);
    s.mipFilter = uint8_t(d.mipFilter);
    s.sAddress = uint8_t(d.sAddressMode);
    s.tAddress = uint8_t(d.tAddressMode);
    s.rAddress = uint8_t(d.rAddressMode);
    s.lodMin = d.lodMinClamp;
    s.lodMax = d.lodMaxClamp;
    RMSampler *r = [RMSampler new];
    r->_device = self;
    r->_descriptor = [d copy];
    r.label = d.label;
    // what the raw sampler header does not express (anisotropy, compare, border colours, unnormalised coordinates, LOD average):
    // the m2v twin is the sampler, and the raw path refuses the draws that use it
    const bool rawOk = d.maxAnisotropy <= 1 && d.normalizedCoordinates &&
                       d.compareFunction == MTLCompareFunctionNever &&
                       d.sAddressMode != MTLSamplerAddressModeClampToBorderColor &&
                       d.tAddressMode != MTLSamplerAddressModeClampToBorderColor && !d.lodAverage;
    if (!rawOk || !gRM->raw->createSampler(s, r->_raw)) {
        if (!predLogged(PredCallSamplerForeign))
            logPred(
                PredCallSamplerForeign, d.maxAnisotropy, d.compareFunction,
                [NSString
                    stringWithFormat:@"(made: normalized %d address %lu/%lu border %lu lodAverage %d header slot %d)",
                                     (int)d.normalizedCoordinates, (unsigned long)d.sAddressMode,
                                     (unsigned long)d.tAddressMode, (unsigned long)d.borderColor, (int)d.lodAverage,
                                     (int)rawOk]);
        if (![r rmTwin])
            return nil;
        RMTwinObject *o =
            [RMTwinObject new];  // a sampler the raw path cannot use: a draw with it goes to m2v (not an RMSampler)
        o->_device = self;
        o->_twin = r->_twin;
        return (id<MTLSamplerState>)o;
    }
    return (id<MTLSamplerState>)r;
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    id twin = [gRM->fallback newDepthStencilStateWithDescriptor:d];
    if (!twin)
        return nil;
    RMDepthStencil *o = [RMDepthStencil new];
    o->_device = self;
    o->_twin = twin;
    o.label = d.label;
    o->_raw.depthCompare = uint8_t(d.depthCompareFunction);
    o->_raw.depthWrite = d.depthWriteEnabled;
    raw::StencilFace *faces[2] = {&o->_raw.front, &o->_raw.back};
    MTLStencilDescriptor *stencils[2] = {d.frontFaceStencil, d.backFaceStencil};
    for (int i = 0; i < 2; i++)
        if (MTLStencilDescriptor *s = stencils[i]) {
            faces[i]->compare = uint8_t(s.stencilCompareFunction);
            faces[i]->fail = uint8_t(s.stencilFailureOperation);
            faces[i]->depthFail = uint8_t(s.depthFailureOperation);
            faces[i]->pass = uint8_t(s.depthStencilPassOperation);
            faces[i]->readMask = s.readMask;
            faces[i]->writeMask = s.writeMask;
        }
    return (id<MTLDepthStencilState>)o;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithURL:url error:error], url.path);
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithFile:path error:error], path);
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithData:data error:error], nil);
}

- (id<MTLLibrary>)newDefaultLibrary
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibrary], nil);
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibraryWithBundle:bundle error:error],
                                       [bundle.bundlePath stringByAppendingString:@"/default.metallib"]);
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithSource:source options:options error:error],
                                       nil);
}

// A descriptor whose functions are this device's: a copy over their twins, for the m2v device
static MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)
{
    MTLRenderPipelineDescriptor *t = [d copy];
    t.vertexFunction = twinOf(d.vertexFunction);
    t.fragmentFunction = twinOf(d.fragmentFunction);
    return t;
}

- (RMPipeline *)pipelineFor:(MTLRenderPipelineDescriptor *)d twin:(id<MTLRenderPipelineState>)twin
{
    RMPipeline *p = [RMPipeline new];
    p->_device = self;
    p->_twin = twin;
    p.label = d.label;
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    p->_packable = vf && ff && !vf->_framework.empty() && vf->_framework == ff->_framework;
    if (p->_packable) {
        NSString *key = raw_metal_pipeline_key(d);
        const char *k = key.UTF8String;
        p->_raw = gRM->raw->pipeline(raw::Pack::fnv(k, strlen(k)), raw::PackForm::Default,
                                     vf->_framework == "QuartzCore" && [vf.name hasPrefix:@"fixed_vert_"] &&
                                         [vf.name hasSuffix:@"_spc"]);
    }
    if (!p->_raw) {
        const char *label = d.label.UTF8String ?: "-";
        SWITCH_REFUSE_DETAIL("raw", "state.unpacked", raw::Pack::fnv(label, strlen(label)),
                             "state not in the pack (m2v draws it)%s: %s",
                             !p->_packable              ? " (not one framework library's functions: never looked up)"
                             : gRM->raw->stats.heapFull ? " (or the shader heap is full)"
                                                        : "",
                             label);
    }
    return p;
}

// m2v's state over the twins, for a command buffer that has to go to m2v (made asynchronously in the render server:
// AsyncPipelines.h). A state the pack draws gets one made at its first set there, by a fallback: made eagerly on the
// user-initiated workers (above backboardd's main thread), they cost ~9 ms of CPU each, 25-170 ms of a worker, while no
// command buffer fell back (11-06 21:03: 61 twins, 1074 raw command buffers, none to m2v).
static id<MTLRenderPipelineState> pipelineTwin(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool packed,
                                               NSError **error)
{
    if (packed && native_bridge_metal_render_pipeline_lazy)
        return native_bridge_metal_render_pipeline_lazy(gRM->fallback, twinDescriptor(d), o);
    return [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d) options:o reflection:nil error:error];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    RMPipeline *p = [self pipelineFor:d twin:nil];
    p->_twin = pipelineTwin(d, MTLPipelineOptionNone, p->_raw, error);
    if (!p->_twin && !p->_raw)
        return nil;
    if (error && p->_raw)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (reflection) {
        id<MTLRenderPipelineState> twin = [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                                      options:o
                                                                                   reflection:reflection
                                                                                        error:error];
        RMPipeline *p = [self pipelineFor:d twin:twin];
        return !twin && !p->_raw ? nil : (id<MTLRenderPipelineState>)p;
    }
    RMPipeline *p = [self pipelineFor:d twin:nil];
    p->_twin = pipelineTwin(d, o, p->_raw, error);
    return !p->_twin && !p->_raw ? nil : (id<MTLRenderPipelineState>)p;
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                      completionHandler:^(id<MTLRenderPipelineState> twin, NSError *e) {
                                          RMPipeline *p = [me pipelineFor:copy twin:twin];
                                          handler((!twin && !p->_raw) ? nil : (id<MTLRenderPipelineState>)p, e);
                                      }];
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                     options:(MTLPipelineOption)o
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                options:o
                                      completionHandler:^(id<MTLRenderPipelineState> twin,
                                                          MTLRenderPipelineReflection *r, NSError *e) {
                                          RMPipeline *p = [me pipelineFor:copy twin:twin];
                                          handler((!twin && !p->_raw) ? nil : (id<MTLRenderPipelineState>)p, r, e);
                                      }];
}

- (id)wrapCarried:(id)twin
{
    if (!twin)
        return nil;
    RMTwinObject *o = [RMTwinObject new];
    o->_device = self;
    o->_twin = twin;
    return o;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error
{
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithFunction:twinOf(f) error:error]];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                             error:(NSError **)error
{
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithFunction:twinOf(f)
                                                                        options:o
                                                                     reflection:r
                                                                          error:error]];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                             options:(MTLPipelineOption)o
                                                          reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                               error:(NSError **)error
{
    MTLComputePipelineDescriptor *t = [d copy];
    t.computeFunction = twinOf(d.computeFunction);
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithDescriptor:t
                                                                          options:o
                                                                       reflection:r
                                                                            error:error]];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d
                                                               options:(MTLPipelineOption)o
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)r
                                                                 error:(NSError **)error
{
    MTLTileRenderPipelineDescriptor *t = [d copy];
    t.tileFunction = twinOf(d.tileFunction);
    return [self wrapCarried:[gRM->fallback newRenderPipelineStateWithTileDescriptor:t
                                                                             options:o
                                                                          reflection:r
                                                                               error:error]];
}

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    RMHeap *h = [RMHeap new];
    h->_device = self;
    h->_descriptor = [d copy];
    return (id<MTLHeap>)h;
}

- (id)newResourceGroupFromResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    return [NSObject new];  // cache hints are no-ops here (encodeCacheHint...)
}

@end

// ---- commit: order between the two drivers ----
// With the kernel's Order (TegraGM20BABI.h method 14), the two drivers' command buffers are ordered on the GPU: before a raw
// submission that follows m2v work, one Order(AfterTask) makes its first entry a semaphore acquire on what NVK's channels had
// submitted; before an m2v commit that follows raw work, one Order(BeforeTask) makes NVK's channels' next submissions start with
// an acquire on the raw channel's last seqno. Neither thread waits for the GPU. The one CPU handoff left: a raw command buffer is
// submitted once the m2v ones before it are scheduled (NVK's Exec made, so the AfterTask acquire covers them) - a wait for the
// m2v runtime's own submission of work the GPU is already running, not for that work. What the GPU order cannot cover is
// ordered on the CPU, each with a one-shot line naming it: a kernel without Order, an Order the kernel refuses, a shared event
// signalled outside this device's submitted work.

static uint64_t signalled(NSMapTable *map, void *event)
{
    NSNumber *n = [map objectForKey:(__bridge id)event];
    return n ? n.unsignedLongLongValue : 0;
}

// submit lock held: the event signals `cb` makes, as submitted ones of its driver
static void noteSignals(RMCommandBuffer *cb, NSMapTable *map)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpSignalEvent && c.c > signalled(map, c.obj))
            [map setObject:@(c.c) forKey:(__bridge id)c.obj];
}

// Whether the shared event wait `i` of `cb` is covered without a CPU wait: signalled already, earlier in `cb`, or (GPU order)
// by a command buffer submitted before it - raw: the channel's own order; m2v: scheduled, so behind the AfterTask acquire.
static bool waitCovered(RMCommandBuffer *cb, size_t i)
{
    const Cmd &c = cb->_tape[i];
    for (size_t k = 0; k < i; k++)
        if (cb->_tape[k].op == OpSignalEvent && cb->_tape[k].obj == c.obj && cb->_tape[k].c >= c.c)
            return true;
    if ([(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] >= c.c)
        return true;
    return gRM->gpuOrder && (signalled(gRM->rawSignals, c.obj) >= c.c || signalled(gRM->m2vSignals, c.obj) >= c.c);
}

static bool eventsReady(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++)
        if (cb->_tape[i].op == OpWaitEvent && !waitCovered(cb, i))
            return false;
    return true;
}

// How long a command buffer waits for a shared event another process signals (waitEvents).
static constexpr int64_t kEventLimitNs = 10 * NSEC_PER_SEC;

// The CPU-ordered remainder: each wait nothing submitted covers, until the event's value is reached (notified, no polling).
static void waitEvents(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++) {
        if (cb->_tape[i].op != OpWaitEvent || waitCovered(cb, i))
            continue;
        const Cmd &c = cb->_tape[i];
        once(@"CPU-ordered: a shared event signalled outside this device's submitted work (the host or another "
             @"process): "
             @"the command buffer waiting for it is submitted once it is reached");
        gRM->stats.cpuOrdered++;
        dispatch_semaphore_t reached = dispatch_semaphore_create(0);
        id<MTLSharedEvent> event = (__bridge id)c.obj;
        [event notifyListener:gRM->listener
                      atValue:c.c
                        block:^(id<MTLSharedEvent>, uint64_t) {
                            dispatch_semaphore_signal(reached);
                        }];
        // A signaller that never reaches it (a client killed or suspended mid-frame: Core Animation's per-surface events,
        // CA::OGL::shared_event_submit) holds no one past kEventLimit: the command buffer goes on as if the event were reached,
        // as Core Animation's own SharedEvent::force_complete would make it (r7n: AegirPoster died, r7o: PosterKit's scenes
        // were suspended, and this wait held every later command buffer and the render thread for good).
        if (dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW, kEventLimitNs))) {
            gRM->stats.eventsReleased++;
            os_log_error(rawLog(),
                         "raw: shared event %p (%{public}s) not reached %llu after %lld ms (at %llu): its command "
                         "buffer goes on without it",
                         (__bridge void *)event, object_getClassName(event), (unsigned long long)c.c,
                         (long long)(kEventLimitNs / 1000000), (unsigned long long)event.signaledValue);
        }
    }
}

// What a raw command buffer waits for before it is submitted: GPU order, the m2v command buffers before it scheduled; CPU
// order, completed. m2vLock held.
static bool m2vClear()
{
    return gRM->gpuOrder ? gRM->m2vUnscheduled == 0 : gRM->m2vPending == 0;
}

// The kernel refused an Order: from here on the drivers are ordered on the CPU (one line).
static void orderRefused(const char *what, int kr)
{
    once([NSString stringWithFormat:@"CPU-ordered from now on: the kernel refused Order (%s, 0x%x)", what, kr]);
    std::lock_guard<std::mutex> m(gRM->m2vLock);
    gRM->gpuOrder = false;
}

static void countDriver(int driver)
{
    RawState &r = *gRM;
    if (r.lastDriver && r.lastDriver != driver)
        r.stats.switches++;
    r.lastDriver = driver;
}

// Submits `cb` now: the caller holds gRM->submit and everything committed before it is submitted. false: not yet (a raw
// command buffer whose Order was refused just now waits for the m2v work before it to complete; the caller keeps it first).
// Diagnostics: each predicate `cb` hit, once (and once more when it was the only one)
static void countPreds(RMCommandBuffer *cb)
{
    const uint64_t m = cb->_preds;
    if (!m)
        return;
    const bool only = !(m & (m - 1));
    for (uint64_t left = m; left; left &= left - 1) {
        const int p = __builtin_ctzll(left);
        gPredCbs[p].fetch_add(1, std::memory_order_relaxed);
        if (only)
            gPredOnly[p].fetch_add(1, std::memory_order_relaxed);
    }
}

// Before an m2v command buffer that uses the later levels of a mipmapped texture the raw path rendered levels of (so they are
// not what m2v's generateMipmaps gives): every later level out of the chain into its mirror, on the raw channel, ordered
// before the m2v work on the GPU (rawSinceM2v: its submission acquires on this one); mipSync copies them on. In CPU order
// (the kernel refused Order) m2v generates them from level 0 instead.
static void mirrorsForM2v(RMCommandBuffer *cb, size_t from, size_t to)
{
    RawState &r = *gRM;
    if (!cb->_chains || !r.gpuOrder)
        return;
    std::vector<RMTexture *> need;
    auto add = [&](void *o) {
        if (!chained(o))
            return;
        RMTexture *t = (__bridge RMTexture *)o;
        const raw::Texture::Chain &c = *t->_raw.chain;
        if (t->_mipLevels || t->_mirrored || !c.rawLevels || !c.levelsFresh || c.mirrorLevels)
            return;
        if (std::find(need.begin(), need.end(), t) == need.end())
            need.push_back(t);
    };
    for (size_t at = from; at < to; at++) {
        const Cmd &c = cb->_tape[at];
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            for (unsigned j = 0; j < 8; j++)
                if (pass.colorAttachments[j].level)
                    add((__bridge void *)pass.colorAttachments[j].texture);
            break;
        }
        case OpTexture:
            if (c.obj && [(__bridge id)c.obj isKindOfClass:[RMTexture class]]) {
                RMTexture *t = (__bridge RMTexture *)c.obj;
                add(t->_kind == RMTextureView && t->_parent ? (__bridge void *)t->_parent : c.obj);
            }
            break;
        case OpCopyTexture:
        case OpCopyTextureToBuffer:
            if (extra(cb, c.x + 1))
                add(c.obj);
            break;
        default:
            break;
        }
    }
    for (auto it = need.begin(); it != need.end();) {
        bool all = true;
        for (uint32_t l = 1; all && l < (*it)->_raw.chain->layout.levels; l++)
            all = [*it rmMirror:l] != nil;
        it = all ? std::next(it) : need.erase(it);
    }
    if (need.empty())
        return;
    if (r.m2vSinceRaw) {  // m2v work before may still read the mirrors
        uint32_t waits = 0;
        const int kr = r.raw->orderAfterOthers(waits);
        if (kr != KERN_SUCCESS) {
            orderRefused("raw mirrors after m2v", kr);
            return;
        }
        r.m2vSinceRaw = false;
        r.stats.acquires += waits;
    }
    raw::Device &d = *r.raw;
    raw::CommandBuffer *rc = d.begin();
    raw::BlitEncoder *be = rc->blit();
    uint64_t levels = 0;
    for (RMTexture *t : need)
        for (uint32_t l = 1; l < t->_raw.chain->layout.levels; l++) {
            RMTexture *mirror = [t rmMirror:l];
            const raw::fx::MipLevel &lv = t->_raw.chain->layout.level[l];
            be->copyFromLevel(t->_raw, l, 0, 0, mirror->_raw, 0, 0, lv.width, lv.height);
            [cb retain:mirror];  // until the m2v work that reads it completes, which runs after this copy
            levels++;
        }
    be->end();
    uint32_t seq = 0;
    if (!rc->commit(seq)) {
        rc->abandon();
        once(@"the kernel refused the mirror copies of mipmapped levels: m2v generates them from level 0");
        return;
    }
    r.rawSinceM2v = true;
    d.stats.mirrorsOut += levels;
    for (RMTexture *t : need)
        t->_mirrored = true;
}

#if SWITCH_FRAME_TIMELINE
// The m2v replay's parts for the rates line: the wait for an m2v command buffer (its queue's slots), the twins made, and the
// replay's encode end to its commit's return (the Order call, a CPU-order wait, m2v's commit)
static void noteReplay(const switch_frame::Stamps &f, uint64_t returned)
{
    using namespace switch_frame;
    Stats &s = gRM->stats;
    const uint64_t perMs = ticksPerMs();
    auto us = [perMs](uint64_t from, uint64_t to) {
        return to > from ? (to - from) * 1000 / perMs : 0;
    };
    s.replaySlotUs += us(f.get(SlotStart), f.get(SlotEnd));
    s.replayTwinUs += f.get(Twins) * 1000 / perMs;
    s.replayCommitUs += us(f.get(ReplayEnd), returned);
    const uint64_t total = us(f.get(ReplayStart), returned);
    uint64_t seen = s.replayMaxUs.load(std::memory_order_relaxed);
    while (total > seen && !s.replayMaxUs.compare_exchange_weak(seen, total, std::memory_order_relaxed)) {
    }
}
#endif

static bool submitSegments(RMCommandBuffer *cb);

static bool submitLocked(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    foldCpuLevels();
    if (!cb->_segs.empty())
        return submitSegments(cb);
    const char *why = nullptr;
    if (cb->_reason == WhyRaw) {
        if (r.gpuOrder && r.m2vSinceRaw) {  // its first entry: an acquire on what NVK's channels submitted
            uint32_t waits = 0;
            const int kr = r.raw->orderAfterOthers(waits);
            if (kr == KERN_SUCCESS) {
                r.m2vSinceRaw = false;
                r.stats.acquires += waits;
            } else {
                orderRefused("raw after m2v", kr);
                std::lock_guard<std::mutex> m(r.m2vLock);
                if (r.m2vPending)
                    return false;
            }
        }
        const uint64_t began = nowUs();
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::EncodeStart);
        switch_frame::trace(switch_frame::kRawEncode, switch_frame::kStart, cb->_frame.seq);
#endif
        const bool encoded = encodeRaw(cb, 0, cb->_tape.size(), &why);
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::EncodeEnd);
        switch_frame::trace(switch_frame::kRawEncode, switch_frame::kEnd, cb->_frame.seq, encoded);
#endif
        if (encoded) {
            r.stats.encodeUs += nowUs() - began;
            r.stats.raw++;
            r.rawSinceM2v = true;
            countDriver(1);
            noteSignals(cb, r.rawSignals);
            {
                std::lock_guard<std::mutex> l(r.doneLock);
                r.inflight.push_back(cb);
            }
            r.doneChanged.notify_one();
            scheduled(cb);
            return true;
        }
        r.stats.aborted++;
        r.stats.abortUs += nowUs() - began;
        cb->_reason = WhyEncode;
        cb->_why = [NSString stringWithUTF8String:why ?: "?"];
    }
    r.stats.m2v++;
    r.stats.reasons[cb->_reason]++;
    countPreds(cb);
    // one line per reason kind (no string per command buffer: the predicates' own lines carry the values)
    SWITCH_REFUSE_DETAIL("raw", "cb.m2v", cb->_reason, "command buffer on m2v (%s): %s [first predicate %s]",
                         kReasonNames[cb->_reason], cb->_why.UTF8String ?: "-", kPredNames[cb->_pred0]);
    cb->_m2v = true;
    const uint64_t replayBegan = nowUs();
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::ReplayStart);
    switch_frame::trace(switch_frame::kReplay, switch_frame::kStart, cb->_frame.seq);
    tTwinTicks = 0;
#endif
    if (r.fallback)
        mirrorsForM2v(cb, 0, cb->_tape.size());
    id<MTLCommandBuffer> m = r.fallback ? replay(cb, 0, cb->_tape.size(), false) : nil;
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::ReplayEnd);
    cb->_frame.set(switch_frame::Twins, tTwinTicks);
    switch_frame::trace(switch_frame::kReplay, switch_frame::kEnd, cb->_frame.seq, tTwinTicks);
#endif
    if (!m) {
        finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:nil]);
        return true;
    }
    // after the raw work before it: NVK's next submissions start with an acquire on the raw channel (GPU order), or that work
    // completes first (CPU order: a kernel wait on the fence interrupt)
    if (r.gpuOrder && r.rawSinceM2v) {
        uint32_t waits = 0;
        const int kr = r.raw->orderOthersAfter(waits);
        if (kr == KERN_SUCCESS)
            r.stats.acquires += waits;
        else
            orderRefused("m2v after raw", kr);
    }
    if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
        r.stats.cpuOrdered++;
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::OrderWaitStart);
        switch_frame::trace(switch_frame::kOrderWait, switch_frame::kStart, cb->_frame.seq);
#endif
        r.raw->wait(r.raw->submitted());
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::OrderWaitEnd);
        switch_frame::trace(switch_frame::kOrderWait, switch_frame::kEnd, cb->_frame.seq);
#endif
    }
    r.rawSinceM2v = false;
    r.m2vSinceRaw = true;
    countDriver(2);
    noteSignals(cb, r.m2vSignals);
    __block bool counted = false;  // m2vLock: it left m2vUnscheduled (scheduled, or completed without that)
    {
        std::lock_guard<std::mutex> l(r.m2vLock);
        r.m2vPending++;
        r.m2vUnscheduled++;
    }
    [m addScheduledHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted)
                gRM->m2vUnscheduled--;
            counted = true;
        }
        gRM->m2vChanged.notify_all();
        scheduled(cb);
    }];
    [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::Fence);
        switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
        finish(cb, done.error);
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted)
                gRM->m2vUnscheduled--;  // a refused commit never schedules
            counted = true;
            gRM->m2vPending--;
        }
        gRM->m2vChanged.notify_all();
    }];
    [m commit];
    r.stats.replayUs += nowUs() - replayBegan;
#if SWITCH_FRAME_TIMELINE
    noteReplay(cb->_frame, cb->_frame.mark(switch_frame::Returned));
    switch_frame::trace(switch_frame::kReplayCommitted, switch_frame::kPoint, cb->_frame.seq);
#endif
    return true;
}

// A command buffer some of whose encoders go to m2v and some do not, with no refusal of its own and no shared event: its segments
// (each run of encoders of one driver), else none (it goes whole, as its reason says).
// Off since r7p8 (a freeze 20 s after the home screen whose cause the logs did not name; the split was active just before it):
// every command buffer goes whole, as before r12. It comes back once the stall line (rawStall) or m2v's bounded queue slot wait
// names the cause.
static constexpr bool kSplit = false;

static void planSegments(RMCommandBuffer *cb)
{
    cb->_segs.clear();
    cb->_segNext = 0;
    [cb closeEncoder];
    if (!kSplit || cb->_reason == WhyRaw || cb->_whole)
        return;
    bool raw = false, m2v = false;
    for (size_t at = 0; at < cb->_tape.size(); at++) {
        const Cmd &c = cb->_tape[at];
        if (c.op == OpSignalEvent || c.op == OpWaitEvent || (c.op == OpInvocation && c.stage == 0xff)) {
            cb->_segs.clear();
            return;
        }
        if (c.op != OpRender && c.op != OpBlit && c.op != OpCompute) {
            if (cb->_segs.empty())
                return;  // a command before the first encoder
            continue;
        }
        const bool r = !c.index;
        (r ? raw : m2v) = true;
        if (!cb->_segs.empty())
            cb->_segs.back().to = uint32_t(at);
        if (!cb->_segs.empty() && cb->_segs.back().raw == r)
            continue;
        cb->_segs.push_back({uint32_t(at), uint32_t(cb->_tape.size()), r});
    }
    if (!cb->_segs.empty())
        cb->_segs.back().to = uint32_t(cb->_tape.size());
    if (!raw || !m2v)
        cb->_segs.clear();
}

// Whether what `cb` submits next is raw work (it needs the m2v work before it scheduled: m2vClear)
static bool nextRaw(RMCommandBuffer *cb)
{
    return cb->_segs.empty() ? cb->_reason == WhyRaw : cb->_segNext < cb->_segs.size() && cb->_segs[cb->_segNext].raw;
}

// submit lock held: a split command buffer's segments from the next one, in order, each as a command buffer of its driver would
// go (submitLocked), the scheduled and completed handlers with the last. false: the next is raw and the m2v work before it is not
// scheduled yet (it waits on the submit thread, as a raw command buffer after m2v work does).
static bool submitSegments(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    if (cb->_segNext == 0) {
        r.stats.splits++;
        countPreds(cb);
    }
    while (cb->_segNext < cb->_segs.size()) {
        RMSegment &s = cb->_segs[cb->_segNext];
        const bool last = cb->_segNext + 1 == cb->_segs.size();
        if (s.raw) {
            {
                std::lock_guard<std::mutex> m(r.m2vLock);
                if (!m2vClear())
                    return false;
            }
            if (r.gpuOrder && r.m2vSinceRaw) {
                uint32_t waits = 0;
                const int kr = r.raw->orderAfterOthers(waits);
                if (kr == KERN_SUCCESS) {
                    r.m2vSinceRaw = false;
                    r.stats.acquires += waits;
                } else {
                    orderRefused("raw after m2v", kr);
                    std::lock_guard<std::mutex> m(r.m2vLock);
                    if (r.m2vPending)
                        return false;
                }
            }
            const char *why = nullptr;
            const uint64_t began = nowUs();
            if (!encodeRaw(cb, s.from, s.to, &why)) {
                s.raw = false;  // the kernel refused it: this segment on m2v
                continue;
            }
            r.stats.encodeUs += nowUs() - began;
            r.rawSinceM2v = true;
            countDriver(1);
            if (last) {
                {
                    std::lock_guard<std::mutex> l(r.doneLock);
                    r.inflight.push_back(cb);
                }
                r.doneChanged.notify_one();
                scheduled(cb);
            }
        } else {
            r.stats.splitM2v++;
            const uint64_t began = nowUs();
            if (r.fallback)
                mirrorsForM2v(cb, s.from, s.to);
            id<MTLCommandBuffer> m = r.fallback ? replay(cb, s.from, s.to, true) : nil;
            if (!m) {
                cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                                    code:MTLCommandBufferErrorInternal
                                                userInfo:nil];
                if (last)
                    finish(cb, cb->_segError);
                cb->_segNext++;
                continue;
            }
            if (r.gpuOrder && r.rawSinceM2v) {
                uint32_t waits = 0;
                const int kr = r.raw->orderOthersAfter(waits);
                if (kr == KERN_SUCCESS)
                    r.stats.acquires += waits;
                else
                    orderRefused("m2v after raw", kr);
            }
            if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
                r.stats.cpuOrdered++;
                r.raw->wait(r.raw->submitted());
            }
            r.rawSinceM2v = false;
            r.m2vSinceRaw = true;
            countDriver(2);
            __block bool counted = false;
            {
                std::lock_guard<std::mutex> l(r.m2vLock);
                r.m2vPending++;
                r.m2vUnscheduled++;
            }
            [m addScheduledHandler:^(id<MTLCommandBuffer>) {
                {
                    std::lock_guard<std::mutex> l(gRM->m2vLock);
                    if (!counted)
                        gRM->m2vUnscheduled--;
                    counted = true;
                }
                gRM->m2vChanged.notify_all();
                if (last)
                    scheduled(cb);
            }];
            [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (done.error && !cb->_segError)
                    cb->_segError = done.error;
                if (last)
                    finish(cb, cb->_segError);
                {
                    std::lock_guard<std::mutex> l(gRM->m2vLock);
                    if (!counted)
                        gRM->m2vUnscheduled--;
                    counted = true;
                    gRM->m2vPending--;
                }
                gRM->m2vChanged.notify_all();
            }];
            [m commit];
            r.stats.replayUs += nowUs() - began;
        }
        cb->_segNext++;
    }
    return true;
}

// A committed command buffer: submitted now when nothing committed before it waits and what it needs is there (raw: m2vClear,
// the shared events it waits for covered), else handed to the submit thread in order.
static void commitBuffer(RMCommandBuffer *cb)
{
    raw::followers().committing();
    planSegments(cb);
    RawState &r = *gRM;
    if (waitsForEvent(cb)) {
        std::lock_guard<std::mutex> h(gHoldersLock);
        gHolders.insert(cb);
    }
    std::unique_lock<std::mutex> l(r.submit);
    bool now = r.waiting.empty();
    if (now && nextRaw(cb)) {
        std::lock_guard<std::mutex> m(r.m2vLock);
        now = m2vClear();
        if (!now && !r.gpuOrder)
            r.stats.cpuOrdered++;  // it waits for the m2v work before it to complete
    }
    if (now && cb->_reason == WhyRaw)
        now = eventsReady(cb);
    if (now && submitLocked(cb))
        return;
    r.stats.deferred++;
    r.waiting.push_back(cb);
    l.unlock();
    r.waitingChanged.notify_one();
}

// The submit thread: the command buffers that had to wait, in order, each once what it waits for is there.
static void submitLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.submit);
            r.waitingChanged.wait(l, [&] {
                return !r.waiting.empty();
            });
            cb = r.waiting.front();
        }
        if (nextRaw(cb)) {
            {
                std::unique_lock<std::mutex> m(r.m2vLock);
                if (!r.m2vChanged.wait_for(m, kWaitLimit, [&] {
                        return m2vClear();
                    }))
                    waitTooLong("raw command buffer waiting for the m2v work before it, command buffer",
                                (__bridge void *)cb);
            }
            if (cb->_reason == WhyRaw)
                waitEvents(cb);
        }
        std::lock_guard<std::mutex> l(r.submit);
        if (!submitLocked(cb))
            continue;  // CPU order from now on: it waits for the m2v work before it to complete
        r.waiting.pop_front();
    }
}

// The completion thread: sleeps in the kernel on each raw submission's fence (the interrupt), in order.
static void completionLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.doneLock);
            r.doneChanged.wait(l, [&] {
                return !r.inflight.empty();
            });
            cb = r.inflight.front();
        }
        // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
        const bool ok = r.raw->wait(cb->_seqno);
        if (!ok)
            os_log_error(rawLog(), "raw: command buffer %u failed: the kernel killed its channel", cb->_seqno);
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::Fence);
        switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
        {
            std::lock_guard<std::mutex> l(r.doneLock);
            r.inflight.pop_front();
        }
        finish(cb, ok ? nil
                      : [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                            code:MTLCommandBufferErrorInternal
                                        userInfo:nil]);
    }
}

// ---- the rates line ----
static struct {
    uint64_t raw, m2v, encodeUs, draws, passes, copies, deferred, aborted, reasons[WhyCount], ringWaits, lastUs,
        switches, cpuOrdered, acquires, poolStalls, eventsReleased, heldReturns, abortUs, replayUs, replayClean,
        rtUnaligned, rtMixed, predOnly[PredCount], splits, splitM2v, levelPasses, mirrorsOut, mirrorsIn;
} gRates;

bool raw_metal_texture_kept(id<MTLTexture> texture)
{
    return [(id)texture isKindOfClass:[RMTexture class]] && ((RMTexture *)texture)->_rawOk;
}

bool raw_metal_chains_on(void)
{
    return gRM && gRM->raw->chainsOn();
}

const void *raw_metal_texture_image(id<MTLTexture> texture, size_t *pitch)
{
    if (![(id)texture isKindOfClass:[RMTexture class]])
        return nullptr;
    RMTexture *t = (RMTexture *)texture;
    if (!t->_rawOk || !t->_raw.cpu || t->_raw.blockLinear)
        return nullptr;
    if (pitch)
        *pitch = t->_raw.pitch;
    return t->_raw.cpu;
}

size_t raw_metal_rates(char *out, size_t size)
{
    if (!gRM || !size)
        return 0;
    Stats &s = gRM->stats;
    const uint64_t raw = s.raw.load(), m2v = s.m2v.load(), now = nowUs();
    const uint64_t c = raw - gRates.raw, f = m2v - gRates.m2v, sp = s.splits.load() - gRates.splits;
    if (!c && !f && !sp)
        return 0;
    const double secs = gRates.lastUs ? (now - gRates.lastUs) / 1e6 : 5.0;
    char reasons[160] = "";
    size_t at = 0;
    for (int r = 1; r < WhyCount; r++) {
        const uint64_t n = s.reasons[r].load() - gRates.reasons[r];
        if (n && at < sizeof reasons)
            at += snprintf(reasons + at, sizeof reasons - at, "%s%s:%llu", at ? "," : "", kReasonNames[r],
                           (unsigned long long)n);
        gRates.reasons[r] = s.reasons[r].load();
    }
    raw::Device::Stats &k = gRM->raw->stats;
    const double per = c + sp ? double(c + sp) : 1.0;  // the raw path's command buffers, the split ones included
    // diagnostics (r10-rawsweep): per predicate, the command buffers that hit it / those it alone sent to m2v, the ten most hit
    char preds[256] = "";
    {
        uint64_t cbs[PredCount], only[PredCount];
        for (int i = 0; i < PredCount; i++) {
            const uint64_t now = gPredCbs[i].load(std::memory_order_relaxed),
                           o = gPredOnly[i].load(std::memory_order_relaxed);
            cbs[i] = now - gPredRates[i];
            only[i] = o - gRates.predOnly[i];
            gPredRates[i] = now;
            gRates.predOnly[i] = o;
        }
        size_t pat = 0;
        for (int shown = 0; shown < 10; shown++) {
            int best = 0;
            for (int i = 1; i < PredCount; i++)
                if (cbs[i] > cbs[best])
                    best = i;
            if (!best || !cbs[best] || pat >= sizeof preds)
                break;
            pat += snprintf(preds + pat, sizeof preds - pat, "%s%s:%llu/%llu", pat ? "," : "", kPredNames[best],
                            (unsigned long long)cbs[best], (unsigned long long)only[best]);
            cbs[best] = 0;
        }
    }
    // replay= and rclean= per m2v submission: whole command buffers and split ones' m2v segments
    const uint64_t aborts = s.aborted.load() - gRates.aborted, segs = s.splitM2v.load() - gRates.splitM2v,
                   m2vs = f + segs ? f + segs : 1;
    char diag[768];
    snprintf(diag, sizeof diag,
             " sp=%llu/%llu abortenc=%.0fus/a replay=%.0fus/m rclean=%.0fKB/m rt128=%llu(pitch %u va&255 %u) "
             "rtmix=%llu lvl=%llu "
             "mir=%llu/%llu why=%s",
             (unsigned long long)sp, (unsigned long long)segs,
             (s.abortUs.load() - gRates.abortUs) / double(aborts ? aborts : 1),
             (s.replayUs.load() - gRates.replayUs) / double(m2vs),
             (s.replayClean.load() - gRates.replayClean) / 1024.0 / double(m2vs),
             (unsigned long long)(k.rtUnaligned.load() - gRates.rtUnaligned), k.rtUnalignedPitch.load(),
             k.rtUnalignedVa.load(), (unsigned long long)(k.rtMixed.load() - gRates.rtMixed),
             (unsigned long long)(k.levelPasses.load() - gRates.levelPasses),
             (unsigned long long)(k.mirrorsOut.load() - gRates.mirrorsOut),
             (unsigned long long)(k.mirrorsIn.load() - gRates.mirrorsIn), *preds ? preds : "-");
#if SWITCH_FRAME_TIMELINE
    {
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used,
                 " rsplit=%.0f/%.0f/%.0fus/m rmax=%lluus fb=%llu/%llu/%llu/%llu(%s) clr=%llu/%llu"
                 " so=%llu/%llu/%llu/%lluus",
                 (s.replaySlotUs.exchange(0) / double(m2vs)), (s.replayTwinUs.exchange(0) / double(m2vs)),
                 (s.replayCommitUs.exchange(0) / double(m2vs)), (unsigned long long)s.replayMaxUs.exchange(0),
                 (unsigned long long)k.fetchBarriers.exchange(0), (unsigned long long)k.fetchBarriersElided.exchange(0),
                 (unsigned long long)k.fetchBarriersAnyColor.exchange(0),
                 (unsigned long long)k.fetchBarriersInterlocked.exchange(0),
                 raw::Device::fetchBarrierName(gRM->raw->fetchBarrier.load(std::memory_order_relaxed)),
                 (unsigned long long)k.clears.exchange(0), (unsigned long long)k.clearsElided.exchange(0),
                 (unsigned long long)k.selfOverlapSplit.exchange(0), (unsigned long long)k.selfOverlapRuns.exchange(0),
                 (unsigned long long)k.selfOverlapUnbounded.exchange(0),
                 (unsigned long long)(k.selfOverlapNs.exchange(0) / 1000));
    }
#endif
    gRates.abortUs = s.abortUs.load();
    gRates.splits = s.splits.load();
    gRates.splitM2v = s.splitM2v.load();
    gRates.replayUs = s.replayUs.load();
    gRates.replayClean = s.replayClean.load();
    gRates.rtUnaligned = k.rtUnaligned.load();
    gRates.rtMixed = k.rtMixed.load();
    gRates.levelPasses = k.levelPasses.load();
    gRates.mirrorsOut = k.mirrorsOut.load();
    gRates.mirrorsIn = k.mirrorsIn.load();
    const int n = snprintf(
        out, size,
        "raw %.0fs c=%llu(%.1f/s) m2v=%llu%s%s%s sw=%llu cpu=%llu acq=%llu enc=%.0fus/c d=%.1f p=%.1f b=%.2f "
        "defer=%llu abort=%llu rw=%llu pool=%lluMB/%llu grow=%llu imp=%llu/%lluMB clean=%.1fKB/c ce=%llu/%.0fKB "
        "ev=%llu/%llu%s",
        secs, (unsigned long long)c, c / secs, (unsigned long long)f, *reasons ? "(" : "", reasons, *reasons ? ")" : "",
        (unsigned long long)(s.switches.load() - gRates.switches),
        (unsigned long long)(s.cpuOrdered.load() - gRates.cpuOrdered),
        (unsigned long long)(s.acquires.load() - gRates.acquires), (s.encodeUs.load() - gRates.encodeUs) / per,
        (s.draws.load() - gRates.draws) / per, (s.passes.load() - gRates.passes) / per,
        (s.copies.load() - gRates.copies) / per, (unsigned long long)(s.deferred.load() - gRates.deferred),
        (unsigned long long)(s.aborted.load() - gRates.aborted),
        (unsigned long long)(k.ringWaits.load() - gRates.ringWaits), (unsigned long long)(k.poolBytes.load() >> 20),
        (unsigned long long)k.poolChunks.load(), (unsigned long long)(k.poolStalls.load() - gRates.poolStalls),
        (unsigned long long)k.imports.load(), (unsigned long long)(k.importBytes.load() >> 20),
        k.cleanBytes.exchange(0) / 1024.0 / per, (unsigned long long)k.ceOps.exchange(0),
        k.ceBytes.exchange(0) / 1024.0, (unsigned long long)(s.eventsReleased.load() - gRates.eventsReleased),
        (unsigned long long)(s.heldReturns.load() - gRates.heldReturns), diag);
    gRates.raw = raw;
    gRates.m2v = m2v;
    gRates.encodeUs = s.encodeUs.load();
    gRates.draws = s.draws.load();
    gRates.passes = s.passes.load();
    gRates.copies = s.copies.load();
    gRates.deferred = s.deferred.load();
    gRates.aborted = s.aborted.load();
    gRates.switches = s.switches.load();
    gRates.cpuOrdered = s.cpuOrdered.load();
    gRates.acquires = s.acquires.load();
    gRates.poolStalls = k.poolStalls.load();
    gRates.ringWaits = k.ringWaits.load();
    gRates.eventsReleased = s.eventsReleased.load();
    gRates.heldReturns = s.heldReturns.load();
    gRates.lastUs = now;
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The stall line (r7p8: the compositor stopped committing for good with nothing in the log): every 5 s, on a timer of its own,
// what the raw path has pending, read without waiting (a lock another thread holds is reported held, not waited for). Once
// nothing was committed or completed over two ticks while work is pending or one of its locks stays held, one line names it,
// once per process. Nothing else: an idle screen has nothing pending and says nothing.
static void rawStallTick()
{
    static uint64_t lastCommits = ~0ull, lastDone = ~0ull;
    static int still = 0;
    static bool said = false;
    if (said || !gRM)
        return;
    RawState &r = *gRM;
    Stats &s = r.stats;
    const uint64_t commits = s.raw.load() + s.m2v.load() + s.splits.load(), done = s.finished.load();
    const bool moved = commits != lastCommits || done != lastDone;
    lastCommits = commits;
    lastDone = done;
    if (moved) {
        still = 0;
        return;
    }
    long waiting = -1, inflight = -1, m2vPending = -1, m2vUnscheduled = -1;
    unsigned segNext = 0, segs = 0, reason = 0;
    {
        std::unique_lock<std::mutex> a(r.submit, std::try_to_lock);
        if (a.owns_lock()) {
            waiting = long(r.waiting.size());
            if (!r.waiting.empty()) {
                RMCommandBuffer *f = r.waiting.front();
                segNext = unsigned(f->_segNext);
                segs = unsigned(f->_segs.size());
                reason = unsigned(f->_reason);
            }
        }
    }
    {
        std::unique_lock<std::mutex> b(r.doneLock, std::try_to_lock);
        if (b.owns_lock())
            inflight = long(r.inflight.size());
    }
    {
        std::unique_lock<std::mutex> m(r.m2vLock, std::try_to_lock);
        if (m.owns_lock()) {
            m2vPending = long(r.m2vPending);
            m2vUnscheduled = long(r.m2vUnscheduled);
        }
    }
    bool holders = false;
    {
        std::unique_lock<std::mutex> h(gHoldersLock, std::try_to_lock);
        holders = h.owns_lock();
    }
    const bool device = r.raw->lockFree();
    const uint32_t submitted = r.raw->submitted(), completed = r.raw->completed();
    const bool pending = waiting != 0 || inflight != 0 || m2vPending != 0 || m2vUnscheduled != 0 || !holders ||
                         !device || submitted != completed;
    if (!pending) {
        still = 0;
        return;
    }
    if (++still < 2)
        return;
    said = true;
    os_log_error(rawLog(),
                 "raw: stall: nothing committed or completed for 10 s with work pending: waiting %ld (front segment "
                 "%u/%u reason %u), inflight %ld, m2v pending %ld unscheduled %ld (-1: its lock held), locks free: "
                 "holders %d device %d, seqno submitted %u completed %u, commits %llu completed %llu",
                 waiting, segNext, segs, reason, inflight, m2vPending, m2vUnscheduled, int(holders), int(device),
                 submitted, completed, (unsigned long long)commits, (unsigned long long)done);
}
#endif

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The fetch barrier's way, switched from the Mac within one boot (`idevicenotificationproxy -u <udid> post
// com.brunostjohn.switch.debug.fetch-barrier-<ps|psonly|none>`), to measure what the barrier costs: the shipped barrier and
// texture invalidate (ps) against the barrier alone (psonly) and nothing (none). Measurement only: psonly and none let a
// fetching draw read colors still being written
static void watchFetchBarrierWay(raw::Device *dev)
{
    using Way = raw::Device::FetchBarrier;
    for (const Way way : {Way::PixelShader, Way::PixelShaderOnly, Way::None}) {
        char name[96];
        snprintf(name, sizeof name, "com.brunostjohn.switch.debug.fetch-barrier-%s",
                 raw::Device::fetchBarrierName(way));
        int token;
        notify_register_dispatch(name, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            dev->fetchBarrier.store(way, std::memory_order_relaxed);
            os_log(rawLog(), "raw: fetch barrier way %{public}s (debug notification)",
                   raw::Device::fetchBarrierName(way));
        });
    }
}
#endif

// raw_metal_prepare's thread and what it made (taken once, by raw_metal_device_open)
static struct {
    std::mutex lock;
    std::thread thread;
    std::string path, err;
    uint64_t stack = 0;
    raw::Device *dev = nullptr;
    bool started = false;
} gPrepared;

static raw::Device *openDevice(const char *packPath, uint64_t stack, std::string &err)
{
    raw::Device *dev = new raw::Device;
    if (!dev->open(packPath, stack, err)) {
        delete dev;  // its connection closes: the kernel frees what it held
        return nullptr;
    }
    // the 2D engine's mip-chain self-test at open (raw/fx): one line, whichever way it went (each sampling candidate's bad bytes
    // in the CPU's Tegra/Fermi views, the winner), and when the documented convention failed a second: what it got wrong
    {
        char counts[256] = "", name[48] = "none";
        size_t at = 0;
        for (uint32_t i = 0; i < raw::fx::kSamplings && at < sizeof counts; i++)
            at += snprintf(counts + at, sizeof counts - at, "%s%u/%u", i ? " " : "", dev->selfTestTegra[i],
                           dev->selfTestFermi[i]);
        if (dev->selfTestChosen < raw::fx::kSamplings)
            raw::fx::samplingName(raw::fx::sampling(dev->selfTestChosen), name, sizeof name);
        os_log(rawLog(),
               "raw: fx mip self-test: bad bytes tegra/fermi per sampling [%{public}s] -> %{public}s (sampling %u, "
               "%{public}s view) -> chains %{public}s",
               counts, name, dev->selfTestChosen, dev->selfTestFermiView ? "fermi" : "tegra",
               dev->chainsOn() ? "on" : "off");
        const raw::fx::SelfTestReport &r = dev->selfTestFirst;
        if (r.bad) {
            char firsts[160] = "", levels[96] = "";
            size_t f = 0, l = 0;
            for (uint32_t k = 0; k < r.firsts && f < sizeof firsts; k++)
                f += snprintf(firsts + f, sizeof firsts - f, "%sL%u (%u,%u) c%u want %u got %u", k ? "; " : "",
                              r.first[k].level, r.first[k].x, r.first[k].y, r.first[k].channel, r.first[k].want,
                              r.first[k].got);
            for (uint32_t k = 0; k < raw::fx::kMaxLevels && l < sizeof levels; k++)
                if (r.levelBad[k])
                    l += snprintf(levels + l, sizeof levels - l, "%sL%u:%u", l ? " " : "", k, r.levelBad[k]);
            const uint8_t m = std::max({r.maxDiff[0], r.maxDiff[1], r.maxDiff[2], r.maxDiff[3]});
            os_log(
                rawLog(),
                "raw: fx mip self-test, the documented sampling (centre, origin 0, tegra view): %u bytes off by more "
                "than 1 [%{public}s], max |diff| b/g/r/a %u/%u/%u/%u (%{public}s), first: %{public}s",
                r.bad, levels, r.maxDiff[0], r.maxDiff[1], r.maxDiff[2], r.maxDiff[3],
                m <= 2 ? "rounding" : "positional", firsts);
        }
    }
    dev->warm();
    return dev;
}

void raw_metal_prepare(const char *packPath, uint64_t stack)
{
    std::lock_guard<std::mutex> l(gPrepared.lock);
    if (gPrepared.started)
        return;
    gPrepared.started = true;
    gPrepared.path = packPath;
    gPrepared.stack = stack;
    gPrepared.thread = std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.open");
        std::string err;
        raw::Device *dev = openDevice(gPrepared.path.c_str(), gPrepared.stack, err);
        std::lock_guard<std::mutex> m(gPrepared.lock);
        gPrepared.dev = dev;
        gPrepared.err = err;
    });
}

id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error)
{
    static std::mutex once;
    std::lock_guard<std::mutex> l(once);
    if (gRM)
        return gRM->device;  // one per process
    raw::Device *dev = nullptr;
    std::string err;
    bool prepared = false;
    {
        std::unique_lock<std::mutex> m(gPrepared.lock);
        if (gPrepared.started && gPrepared.thread.joinable()) {
            m.unlock();
            gPrepared.thread.join();  // what is left of it: it began with the plugin, before the m2v device's open
            m.lock();
        }
        if (gPrepared.started && gPrepared.path == packPath && gPrepared.stack == stack) {
            prepared = true;
            dev = gPrepared.dev;
            err = gPrepared.err;
            gPrepared.dev = nullptr;
        }
    }
    if (!prepared)
        dev = openDevice(packPath, stack, err);
    if (dev && fallback && !dev->shared()) {
        err = "the kernel connection is not NVK's, so m2v cannot alias the raw path's memory";
        delete dev;
        dev = nullptr;
    }
    if (!dev) {
        if (error)
            *error = [NSString stringWithUTF8String:err.c_str()];
        return nil;
    }
    // The device class: a subclass of the m2v device's own (an _MTLDevice conforming to MTLDevice and MTLDeviceSPI on the iPad),
    // with no instance variables of its own, and the facade's methods over the inherited ones
    Class base = fallback ? object_getClass(fallback) : [NSObject class];
    Class cls = objc_allocateClassPair(base, "RawGM20BMetalDevice", 0);
    if (!cls) {
        if (error)
            *error = @"the device class could not be made";
        delete dev;
        return nil;
    }
    for (Class from : {[RMDevice class]}) {
        unsigned count = 0;
        Method *methods = class_copyMethodList(from, &count);
        for (unsigned i = 0; i < count; i++)
            class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                            method_getTypeEncoding(methods[i]));
        free(methods);
    }
    objc_registerClassPair(cls);
    // Each facade class conforms to its Metal protocol, as the m2v objects it stands in for do (the frontend's adoptMetalProtocols).
    const std::pair<Class, Protocol *> classes[] = {
        {[RMBuffer class], @protocol(MTLBuffer)},
        {[RMTexture class], @protocol(MTLTexture)},
        {[RMLibrary class], @protocol(MTLLibrary)},
        {[RMFunction class], @protocol(MTLFunction)},
        {[RMHeap class], @protocol(MTLHeap)},
        {[RMSampler class], @protocol(MTLSamplerState)},
        {[RMPipeline class], @protocol(MTLRenderPipelineState)},
        {[RMQueue class], @protocol(MTLCommandQueue)},
        {[RMCommandBuffer class], @protocol(MTLCommandBuffer)},
        {[RMEncoder class], @protocol(MTLCommandEncoder)},
        {[RMRenderEncoder class], @protocol(MTLRenderCommandEncoder)},
        {[RMBlitEncoder class], @protocol(MTLBlitCommandEncoder)},
        {[RMComputeEncoder class], @protocol(MTLComputeCommandEncoder)},
    };
    for (const auto &[c, protocol] : classes)
        class_addProtocol(c, protocol);
    RawState *r = new RawState;
    r->raw = dev;
    r->fallback = fallback;
    r->device = [cls new];
    // The base class's limits table (_MTLDevice's own accessors read it; the frontend's fillLimits filled the m2v device's from its
    // answers): the same for this device. The frontend's fillLimits, which Metal's registration runs through initLimits, finds no
    // answer of this class's own to write (they are the m2v class's), so it leaves the copy as it is.
    static const SEL limits = NSSelectorFromString(@"limits");
    if (fallback && [fallback respondsToSelector:limits] && [r->device respondsToSelector:limits]) {
        const void *from = ((const void *(*)(id, SEL))objc_msgSend)(fallback, limits);
        void *to = ((void *(*)(id, SEL))objc_msgSend)(r->device, limits);
        if (from && to && from != to)
            memcpy(to, from, 0x100);  // kLimitFields' extent (NativeBridgeMetalFrontend.mm)
        // and this device's own answers (RMDevice) where the table holds them (kLimitFields' offsets)
        if (to && from != to) {
            static const struct {
                uint16_t offset;
                uint32_t value;
            } own[] = {{0x78, 256}, {0xb0, 32}};

            for (const auto &o : own)
                memcpy(static_cast<uint8_t *>(to) + o.offset, &o.value, 4);
        }
    }
    r->handlers = dispatch_queue_create(
        "com.brunostjohn.switch.raw.handlers",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    r->rawSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->m2vSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:r->handlers];
    r->gpuOrder = dev->canOrder();
    if (!r->gpuOrder && fallback)
        ::once(@"CPU-ordered: the kernel has no Order (TegraGM20BInfoOrder): raw and m2v command buffers wait for each "
               @"other's "
               @"work to complete");
    gRM = r;
    if (fallback)
        rawAliasSurfaces(dev);
    os_log(rawLog(), "raw: shader heap %llu KB, each program placed at its state's first use",
           (unsigned long long)(dev->heapBytes() >> 10));
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    {  // the stall line's timer (rawStallTick): its own, so nothing the render server's other threads hold delays it
        static dispatch_source_t stall;
        stall =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(stall, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC,
                                  NSEC_PER_SEC / 4);
        dispatch_source_set_event_handler(stall, ^{
            rawStallTick();
        });
        dispatch_resume(stall);
    }
    watchFetchBarrierWay(dev);
#endif
    // what both threads hold up is the frame: they follow its committing threads (raw_follow.h)
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.submit");
        raw::followers().join();
        submitLoop();
    }).detach();
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.completion");
        raw::followers().join();
        completionLoop();
    }).detach();
    return (id<MTLDevice>)r->device;
}
