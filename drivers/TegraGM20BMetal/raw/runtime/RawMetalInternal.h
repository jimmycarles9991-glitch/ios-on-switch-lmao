// SPDX-License-Identifier: MIT
#pragma once
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
#include <unordered_map>
#include <vector>
#include "GpuMemoryRelease.h"
#include "LimitsTable.h"
#include "raw_invocation.h"
#include "SwitchFrameTimeline.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusalSite.h"
#include "RawAlias.h"
#include "RawComplete.h"
#include "raw_follow.h"
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusal.h"
#include "RawMetalObjects.h"

// The m2v frontend's in-place textures over a buffer, of rows or the device's optimal layout (NativeBridgeMetalFrontend.mm):
// weak, so the host benchmark links without them.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options)
    __attribute__((weak_import));
// Where NVK keeps a texture's texels (a stored depth/stencil texture the raw path renders and copies in place)
struct switch_nvk_image_memory;
extern "C" int native_bridge_metal_texture_memory(id texture, struct switch_nvk_image_memory *out)
    __attribute__((weak_import));
// Why the bridge's last call on this thread failed (bridge/NativeMetalObjects.h)
extern "C" const char *nmb_last_error(void) __attribute__((weak_import));

// A wait the plugin bounds (r9-hang: r7n's and r7o's freezes were a userland wait with no end in backboardd's render thread):
// past kWaitLimit, one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze, the userland twin of the kernel's missing-interrupt panic. Nothing waits that long when it works: within 10 s the kernel
// kills the channel a GPU wait is stuck on (or panics).
static constexpr auto kWaitLimit = std::chrono::seconds(20);
[[noreturn]] void waitTooLong(const char *what, const void *object);

// How long waitUntilScheduled waits before it asks whether a shared event holds the command buffer (waitUntilScheduled).
static constexpr auto kHeldGrace = std::chrono::milliseconds(500);

os_log_t rawLog();
void once(NSString *text);
double hostSeconds();
uint64_t nowUs();

struct Stats {
    std::atomic<uint64_t> raw{0}, m2v{0}, deferred{0}, aborted{0}, encodeUs{0}, draws{0}, passes{0}, copies{0},
        reasons[WhyCount]{}, switches{0}, cpuOrdered{0}, acquires{0}, eventsReleased{0}, heldReturns{0};
    // diagnostics (r10-rawsweep): the raw encode thrown away by aborted command buffers, the m2v replay's time on the committing
    // thread (replay + commit), and the bytes replay cleans
    std::atomic<uint64_t> abortUs{0}, replayUs{0}, replayClean{0};
    // command buffers submitted in segments (planSegments), and their segments on m2v; of those with both drivers' encoders, the
    // ones split around their shared events, and the ones kept whole for an event wait or signal between encoders
    std::atomic<uint64_t> splits{0}, splitM2v{0}, eventSplits{0}, eventWhole{0};
    // command buffers completed (finish): with the commits, what the stall line (rawStall) watches
    std::atomic<uint64_t> finished{0};
    // waitUntilScheduled / waitUntilCompleted calls that blocked, and the time they blocked: CPU waits on the caller's thread
    // (Core Animation's render thread flushes with waitUntilScheduled every frame)
    std::atomic<uint64_t> scheduledWaits{0}, scheduledWaitUs{0}, completedWaits{0}, completedWaitUs{0};
#if SWITCH_FRAME_TIMELINE
    // the m2v replay's parts (noteReplay)
    std::atomic<uint64_t> replaySlotUs{0}, replayTwinUs{0}, replayCommitUs{0}, replayMaxUs{0};
    // the draws it replayed, and the twins it made: textures, buffers, anything else
    std::atomic<uint64_t> replayDraws{0}, twinTextures{0}, twinBuffers{0}, twinOthers{0}, twinTextureUs{0};
    // command buffers completed before their submitting thread scheduled them (finish() ran their scheduled handlers)
    std::atomic<uint64_t> scheduledByFinish{0};
    std::atomic<uint64_t> twinForwards{
        0};  // selectors m2v's object answers that a facade object has no method for (answerMissing)
    std::atomic<uint64_t> zeroAnswers{0};  // selectors answered zero, as m2v's objects answer them (answerZero)
#endif
};

// A wait that blocked, counted with its time into two Stats counters when it ends (debug builds; nothing in release).
class BlockedWait {
public:
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    BlockedWait(std::atomic<uint64_t> &waits, std::atomic<uint64_t> &waitedUs)
        : waits_(&waits), waitedUs_(&waitedUs), began_(nowUs())
    {}

    ~BlockedWait()
    {
        waits_->fetch_add(1, std::memory_order_relaxed);
        waitedUs_->fetch_add(nowUs() - began_, std::memory_order_relaxed);
    }

    BlockedWait(const BlockedWait &) = delete;
    BlockedWait &operator=(const BlockedWait &) = delete;
    BlockedWait(BlockedWait &&) = delete;
    BlockedWait &operator=(BlockedWait &&) = delete;

private:
    std::atomic<uint64_t> *waits_;
    std::atomic<uint64_t> *waitedUs_;
    uint64_t began_;
#else
    BlockedWait(std::atomic<uint64_t> &, std::atomic<uint64_t> &)
    {}
#endif
};

// The process's one raw device (the render server's): its state. The device object Core Animation holds is an instance of a
// subclass of the m2v device's own class (raw_metal_device_open), so it is an _MTLDevice that conforms to Metal's protocols as
// the m2v device does and answers every selector the facade does not override with the m2v device's own method; it has no
// instance variables of its own (the base class's ABI is fixed), so its state lives here.
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
    std::atomic<uint32_t> committed{0};      // command buffers committed and not finished (gRawRelease's drain)
    Stats stats;
    std::unique_ptr<raw::Completion> completion;  // the states the pack lacks (raw_metal_complete_states)
};

extern RawState *gRM;

// RawMetalPredicates.mm
const char *metalTypes(SEL selector);
// m2v's object of `twinClass` has a method for `selector`: else it answers zero (the frontend's forwardZero), and so does the
// facade's object, with no twin made and nothing sent to m2v (answerZero)
bool twinAnswers(Class twinClass, SEL selector);
void answerZero(id object, NSInvocation *invocation);
// A selector m2v's object answers that raw has no method for: answered zero, a fault in debug builds and the rates line's tw=../f
void answerMissing(id object, NSInvocation *invocation);
bool rawShape(MTLTextureDescriptor *d);
bool chainShape(MTLTextureDescriptor *d);
bool lineShape(MTLTextureDescriptor *d);
bool volumeShape(MTLTextureDescriptor *d);
bool volumeRendered(MTLTextureDescriptor *d);
uint32_t arrayType(MTLTextureDescriptor *d);
bool zetaDescriptor(MTLTextureDescriptor *d);
bool multisampleShape(MTLTextureDescriptor *d);
uint32_t rawShapeTerm(MTLTextureDescriptor *d);
bool identitySwizzle(MTLTextureSwizzleChannels s);
// `t` reading its channels through `s` (a descriptor's swizzle, or a swizzled view's): its raw header, its descriptor
id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s);
// A texture the raw path does not keep: its predicate, and the predicate's first line with the texture's details
void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra);
void keepExtendedRange(MTLTextureDescriptor *d, raw::Texture &t, bool overBuffer);

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

// RawMetalBlit.mm
bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp, bool intoTexture,
                BufferRows &out);
uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y);

// RawMetalLibrary.mm
RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path);

// RawMetalStates.mm: a descriptor whose functions are this device's, over their m2v twins
MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d);

// RawMetalCubic.mm: the cubic binds (raw_cubic.h) of a draw of `pl` with these fragment samplers and textures bound (empty: no
// cubic sampler where it samples), and `p`'s variant for them: null where it is refused, or not made yet (*making: asked)
raw::CubicBinds cubicBinds(const raw::Pipeline &pl, RMSampler *const samplers[16], RMTexture *const textures[16]);
raw::Pipeline *cubicVariant(RMPipeline *p, const raw::CubicBinds &b, bool *making);

// RawMetalHeap.mm: the purged heaps a committed command buffer's resources lie in, given their pages back before it is encoded
// (it counts as committed already, so no purge runs meanwhile)
void restoreArenas(RMCommandBuffer *cb);
// A heap's pages back if it was purged (an m2v twin aliases them); true without a heap or an arena
bool heapResident(RMHeap *h);

// RawMetalTexture.mm
void declareGpuWrites(RMTexture *t);
#if SWITCH_FRAME_TIMELINE
void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed);
#endif

// RawMetalEncode.mm: the imports a texture's or buffer's memory lies in, named to the submission
void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write);
void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write);

// RawMetalEncodeBlit.mm: a blit encoder's command of the tape onto the raw path's copy engine; a texture it writes leaves
// `fresh` and `standInFresh` (its pool image and its stand-in are refreshed again before a sample)
void encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *be,
                std::set<RMTexture *> &fresh, std::set<RMTexture *> &standInFresh);
// A chain's generateMipmapsForTexture: of a format the 2D engine does not filter as Metal does (fmt2d 0): a pass of the pack's
// mip state a level (raw_mip.h); false where the pack has none or a pass is refused (*why)
bool mipPasses(raw::CommandBuffer *rc, RMTexture *t, const char **why);

// RawMetalTextureCpu.mm: `bytes` from `from` cleaned to memory (or cleaned and invalidated), then a DSB
void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate);
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write);
void cpuVolume(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write);
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write);

// RawMetalDevice.mm
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out);

// RawMetalReplay.mm
id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part);

// RawMetalEncode.mm
bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why);

// RawMetalEvents.mm
extern std::mutex gHoldersLock;
extern std::set<RMCommandBuffer *> gHolders;
bool waitsForEvent(RMCommandBuffer *cb);
bool heldBehindEvent();
void unhold(RMCommandBuffer *cb);
uint64_t signalled(NSMapTable *map, void *event);
void noteSignals(RMCommandBuffer *cb, NSMapTable *map);
bool eventsReady(RMCommandBuffer *cb);
void waitEvents(RMCommandBuffer *cb);

// RawMetalFinish.mm
void forEachWritten(RMCommandBuffer *cb, size_t from, size_t to, void (*written)(void *));
extern GpuMemoryRelease &gRawRelease;
void scheduled(RMCommandBuffer *cb);
void finish(RMCommandBuffer *cb, NSError *error);
void completionLoop();

// RawMetalSurfaceWrites.mm
void surfaceConverted(IOSurfaceRef s, bool made);  // a planar or YCbCr texture over `s` made, or gone
uint64_t surfaceWrites(IOSurfaceRef s);
void noteSurfaceWrite(IOSurfaceRef s);
void noteSurfaceWrites(RMCommandBuffer *cb, size_t from, size_t to);  // after [from, to) of `cb` was submitted

// RawMetalDeviceAnswers.mm: the per-kind capability answers (raw_device_answers.h) as methods of the device class `cls`, where
// it has none of its own; and an argument query's answer
void installDeviceAnswers(Class cls);
uint64_t deviceAnswer(SEL selector, uint64_t argument);

// RawMetalQueuePriority.mm
void queuePriorityAdd(RMQueue *queue);
void queuePriorityRemove(RMQueue *queue);

// RawMetalSubmit.mm
void orderRefused(const char *what, int kr);
void commitBuffer(RMCommandBuffer *cb);
void submitLoop();

// RawMetalRates.mm
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void rawStallTick();
void watchFetchBarrierWay(raw::Device *dev);
#endif
