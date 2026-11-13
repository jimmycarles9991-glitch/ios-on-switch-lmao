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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "GpuMemoryRelease.h"
#include "LimitsTable.h"
#include "raw_submit_gate.h"
#include "raw_invocation.h"
#include "SwitchFrameTimeline.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusalSite.h"
#include "RawAlias.h"
#include "RawComplete.h"
#include "raw_follow.h"
#include "raw_gpu.h"
#include "raw_handoff_queue.h"
#include "raw_progress_gate.h"
#include "TegraGM20BLayout.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusal.h"
#include "RawMetalObjects.h"
#include "raw_retirement.h"

// Buffer-backed texture views share the caller's cached CPU pointer with their parent buffer.
static inline RMBuffer *cpuBufferOfResource(id object)
{
    if ([object isKindOfClass:[RMBuffer class]])
        return (RMBuffer *)object;
    if (![object isKindOfClass:[RMTexture class]])
        return nil;
    RMTexture *texture = (RMTexture *)object;
    while (texture->_kind == RMTextureView && texture->_parent)
        texture = texture->_parent;
    return texture->_buffer;
}

// The m2v frontend's in-place textures over a buffer, of rows or the device's optimal layout (NativeBridgeMetalFrontend.mm):
// weak, so the host benchmark links without them.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options)
    __attribute__((weak_import));
extern "C" void native_bridge_metal_journal_render(id descriptor, NSUInteger options) __attribute__((weak_import));
extern "C" int native_bridge_metal_render_pipeline_ready(id pipeline, void (^observer)(void))
    __attribute__((weak_import));
extern "C" int native_bridge_metal_render_pipeline_urge(id pipeline, void (^observer)(void), dispatch_queue_t queue)
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

os_log_t rawLog();
void once(NSString *text);
// The device's handle tables of argument buffers (RawMetalResourceIds.mm): which texture a heap array holds, and an object
// whose ID was named going
bool rmHeapHolds(uint32_t heapClass, const raw::HeapTextureFacts *facts);
void rmHeapForgetTexture(RMTexture *t);
void rmHeapForgetSampler(RMSampler *s);
// A resource a heap made outside its arena (RawMetalHeap.mm): a member of what a job names of the heap until it goes
void rmHeapHold(RMHeap *heap, const raw::HeapMember &member);
void rmHeapForget(RMHeap *heap, uint64_t va);
// newArgumentEncoderWithBufferIndex: of a function (RawMetalArgumentEncoder.mm): nil where Metal makes no encoder
id<MTLArgumentEncoder> rmFunctionArgumentEncoder(RMFunction *function, NSUInteger index);
double hostSeconds();
uint64_t nowUs();

struct Stats {
#if SWITCH_RAW_DIAGNOSTICS
    std::atomic<uint64_t> raw{0}, m2v{0}, deferred{0}, aborted{0}, encodeUs{0}, draws{0}, passes{0}, copies{0},
        reasons[WhyCount]{}, switches{0}, cpuOrdered{0}, acquires{0}, eventsReleased{0}, heldReturns{0};
    // shared event signals published as their raw work completes (publishRawEventSignals), those the CPU set after the head
    // waited for its own work, and native event waits that went on a submitted command buffer's promise
    std::atomic<uint64_t> signalsPublished{0}, signalsAfterWork{0}, waitsOnPromise{0};
    // event waits the scheduler held its queue for on the CPU (a listener armed): no submitted producer, or a timeout
    std::atomic<uint64_t> waitsHeldOnCpu{0};
    // predicated loops laid down again after their job, where they would have run past their copies
    std::atomic<uint64_t> loopContinuations{0};
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
// subclass of _MTLDevice (raw_metal_device_open_lazy) that conforms to Metal's protocols as the m2v device does; a selector
// only m2v's device class has a method for opens the m2v device at its first use (RawMetalM2V.mm). It has no instance
// variables of its own (the base class's ABI is fixed), so its state lives here.
struct RMNativeEventSignal {
    void *event;
    raw::EventTimeline::Promise promise;
};

struct RMRawRetirement {
    RMCommandBuffer *buffer;
    uint32_t seqno;
    const raw::RetirementWrites *writes;  // the command buffer's (RMCommandBuffer::_retirements)
    std::vector<RMNativeEventSignal> signals;
    std::vector<raw::EventTimeline::Promise> waits;
    uint64_t generation = 0;
    raw::CpuCompletion cpu;
    bool quiet = false;  // its job raised no interrupt: a later range's covers it (raw_completion_source.h)
    // the range laid predicated loops: the stamp of the record that names one which ran past its copies, and the range
    uint32_t loopStamp = 0;
    uint32_t loopFrom = 0, loopTo = 0;
};

// A change of the submit worker's schedule another thread hands it (RawState::schedule)
struct RMScheduleChange {
    enum Kind : uint8_t {
        Enqueue,
        Commit,
        Wake
    } kind = Wake;

    RMCommandBuffer *buffer = nil;  // Enqueue, Commit
    void *queue = nullptr;          // Wake: the queue whose head it wakes (null: none)
    uint32_t dependencies = 0;      // Wake: the progress it reports (RMReadyDependency)
};

struct RawState {
    raw::Device *raw = nullptr;
    std::shared_ptr<raw::SharedEvents> sharedEvents;
    std::atomic<bool> eventWords{true};  // the kernel has TegraGM20BJobABI.h method 40 (kernel events' words)
    RawM2V m2v;                          // how the m2v device is opened (RawMetalM2V.mm)
    id<MTLDevice> fallback;              // the m2v device, once opened: read it through rmM2V or rmM2VIfOpen
    std::atomic<bool> m2vOpened{false};
    uint64_t registryID = 0;
    id device;  // the raw device object
    // The submit worker's own: every command buffer's place in its queue's order (RawMetalSchedule.mm). Other threads
    // change it through `schedule`, the changes the worker applies before it takes the next command buffer.
    raw::HandoffQueue<RMScheduleChange> schedule;
    raw::SubmitQueue<void *, RMCommandBuffer *> waiting;
    std::atomic<uint32_t> waitingCommitted{0};  // waiting.committed() as the worker last saw it (the stall line's)
    raw::Mutex eventValues;
    raw::EventTimeline events;
    raw::Mutex m2vLock;
    raw::CondVar m2vChanged;
    // changed under m2vLock, read with none (m2vClear)
    std::atomic<uint32_t> m2vPending{0};      // m2v command buffers committed and not completed
    std::atomic<uint32_t> m2vUnscheduled{0};  // and not scheduled yet: their kernel submission (NVK's Exec) is not made
    // The two drivers' order (stored under m2vLock): on the GPU when the kernel has Order (TegraGM20BInfoOrder), else on
    // the CPU (the m2v work before a raw command buffer completes first, and the raw work before an m2v one).
    std::atomic<bool> gpuOrder{false};
    uint64_t orderRefusedAt = 0;  // raw::Device::Stats::reopens when the kernel last refused an Order (raw_order.h)
    bool rawSinceM2v =
        false;  // raw work submitted since the last m2v commit: the next m2v one waits for it (BeforeTask)
    bool m2vSinceRaw =
        false;  // m2v work submitted since the last raw submission: the next raw one waits for it (AfterTask)
#if SWITCH_RAW_DIAGNOSTICS
    int lastDriver = 0;
#endif
    MTLSharedEventListener *listener;  // the CPU-ordered remainder's event waits
    // immutable range fences, the submit worker's to the completion thread; later submissions never change an earlier wait
    raw::HandoffQueue<RMRawRetirement> inflight;
    // the submit thread will put a job that interrupts behind the quiet ranges in flight (stored before each hand-off);
    // threads in waitUntilCompleted. A change of either wakes the completion thread (inflight.wake()).
    std::atomic<bool> submitPromised{false};
    std::atomic<uint32_t> completionWaiters{0};
    dispatch_queue_t handlers;           // where scheduled handlers run (in order, never inside commit)
    raw::ProgressGate progress;          // whether a completion wakes the submit worker (raw_progress_gate.h)
    std::atomic<uint32_t> committed{0};  // command buffers committed and not finished (the arenas' purge)
    // ranges handed to the kernel or m2v and not completed (rmRangeHandedOff): gRawRelease's drain and its idle rule's
    // in-flight count, which a command buffer parked on an event the CPU has not signalled holds no part of
    std::atomic<uint32_t> rangesInFlight{0};
    Stats stats;
    std::unique_ptr<raw::Completion> completion;  // the states the pack lacks (raw_metal_complete_states)
};

extern RawState *gRM;

// RawMetalM2V.mm: the m2v device, for something the raw path has no own for (`need`, a string literal), opened now if this is
// the process's first need (nil: none can be had). Debug builds name each need once a process and keep their count
// (switchRawM2V): what made its m2v device open.
id<MTLDevice> rmM2V(const char *need);
// The m2v device if something opened it already, else nil: what holds no need of its own (a twin made already, the
// counters)
id<MTLDevice> rmM2VIfOpen();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
id<MTLDevice> rmM2VFor(const char *need, std::atomic<bool> &said);
// "m2v device never opened", or what opened it and in how long: the give-back line's
std::string rmM2VState();
#define RM_M2V(need)                          \
    ([]() -> id<MTLDevice> {                  \
        static std::atomic<bool> said{false}; \
        return rmM2VFor(need, said);          \
    }())
#else
#define RM_M2V(need) rmM2V(need)
#endif
// The device class's selectors m2v's device class has a method for and the raw path has none: each opens the m2v device at
// its first use, then runs m2v's method (RawMetalM2V.mm)
void installM2VForwarders(Class cls, const RawM2V &m2v);

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
uint32_t memorylessLayers(MTLTextureDescriptor *d);
bool blockShape(MTLTextureDescriptor *d);
bool cpuBlocks(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
               bool write);
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
// A library of the metallib `bytes` (a file's at `path`, or data) read by the facade, with no m2v twin; nil: bytes it does
// not read (m2v's library answers then)
RMLibrary *readLibrary(id device, NSString *path, NSData *bytes);
// raw::FunctionSource for the facade's functions (RawRenderKey.h): the AIR slice its library read, else its m2v twin's
bool rmFunctionSource(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name);
std::string libraryName(RMFunction *f);

// RawMetalStates.mm: a descriptor whose functions are this device's, over their m2v twins
MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d);
// The device SPI m2v's device answers and the raw device answers too (RawMetalDevice.mm, RawMetalDeviceAnswers.mm)
@protocol RMDeviceSPI
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface;
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)descriptor;
@end

// A compute descriptor over m2v's functions (RawMetalComputeState.mm): for m2v's state, or a kernel compiled in process
MTLComputePipelineDescriptor *twinComputeDescriptor(MTLComputePipelineDescriptor *d);

// RawMetalCubic.mm: the cubic binds (raw_cubic.h) of a draw of `pl` with these fragment samplers and textures bound (empty: no
// cubic sampler where it samples), and a packed state's variants, the pack's records of them (under its pack key's hash)
// placed at its creation
raw::CubicBinds cubicBinds(const raw::Pipeline &pl, RMSampler *const samplers[16],
                           RMTexture *const textures[raw::kStageTextures]);
void packedVariants(RMPipeline *p, uint64_t keyHash);

// RawMetalHeap.mm: the purged heaps a committed command buffer's resources lie in, given their pages back before it is encoded
// (it counts as committed already, so no purge runs meanwhile)
void restoreArenas(RMCommandBuffer *cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// DEVELOPMENT: the structure of the plan's operations [from, to) into the device's replay tally (raw_replay_shape.h)
void noteReplayShape(RMCommandBuffer *cb, size_t from, size_t to);
#endif
// A heap's pages back if it was purged (an m2v twin aliases them); true without a heap or an arena
bool heapResident(RMHeap *h);
// The device's ask of `d` in an arena of `blockLinear` (a private heap's): every kind the raw path keeps; false: none
bool heapTextureAsk(MTLTextureDescriptor *d, bool blockLinear, raw::TextureAsk &ask);

// RawMetalTexture.mm
void declareGpuWrites(RMTexture *t);
#if SWITCH_FRAME_TIMELINE
void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed);
#endif

// RawMetalEncode.mm: the imports a texture's or buffer's memory lies in, named to the submission
void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write);
void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write);
// RawMetalTessellation.mm: a patch draw on the raw encoder, and its replay on m2v's
bool encodePatches(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const RMPlanPatches &patches,
                   bool indirect, const char **why);
void replayPatches(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const RMPlanPatches &patches,
                   bool indirect, const std::function<id(void *)> &use);

// RawMetalEncodeBlit.mm: a blit encoder's command of the tape onto the raw path's copy engine; a texture it writes leaves
// `fresh` and `standInFresh` (its pool image and its stand-in are refreshed again before a sample)
void encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *be,
                std::set<RMTexture *> &fresh, std::set<RMTexture *> &standInFresh);
// Levels first + 1 .. last of a chain by a pass of the pack's mip state a level (raw_mip.h); false where the pack has none or a
// pass is refused (*why)
bool mipPasses(raw::CommandBuffer *rc, RMTexture *t, uint32_t first, uint32_t last, const char **why);
// A chain's generateMipmapsForTexture: each step on the 2D engine or by mipPasses (raw/fx/fx_mip_route.h); `be` is the blit
// encoder encoding goes on with; `probe2d` the command's probe format (fx_mip_probe.h), 0 for none
bool encodeMipmaps(raw::CommandBuffer *rc, RMTexture *t, uint32_t probe2d, raw::BlitEncoder *&be, const char **why);
bool fillTexturePass(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc);

// RawMetalTextureCpu.mm: `bytes` from `from` cleaned to memory (or cleaned and invalidated), then a DSB
void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate);
bool transferTexture(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, void *bytes,
                     NSUInteger row, NSUInteger image, bool write);
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);
const tgm_layout *textureCpuImportLayout(RMTexture *texture);
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
              NSUInteger image, bool write);
void cpuVolume(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write);
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write);

// RawMetalDevice.mm
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out);

// RawMetalUnsupported.mm: why the device has no form of a state of `d` (null: it has), and that refusal as a creation answers
// it (nil, its error)
const char *rmUnsupportedRenderState(MTLRenderPipelineDescriptor *d);
const char *rmUnsupportedComputeState(MTLComputePipelineDescriptor *d);
id rmRefuseUnsupported(const char *why, NSError **error);

// RawMetalStates.mm: the state of `d` as the pack and the archives it lists answer, nil where it is Metal's binary archive
// miss (`*error`), its record made at once where neither holds it (nil where its creation fails, `*error`); and whether the
// archives miss the state of `d` (its functions this device's)
RMPipeline *rmPipelineFor(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);
bool rmRenderArchiveMissed(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);
// rmPipelineFor's parts: the state as the pack and the archives answer, whether its record is still to be made and what to
// ask the runtime for, and the state handed to its creator once its record is there or refused (its m2v twin made then)
RMPipeline *rmPipelineShell(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);
bool rmNeedsRecord(RMPipeline *p);
raw::Completion::Ask rmRecordAsk(RMPipeline *p, MTLRenderPipelineDescriptor *d);
id<MTLRenderPipelineState> rmFinishState(RMPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                         MTLAutoreleasedRenderPipelineReflection *reflection, NSError **error);
// The error a state's creation fails with (Completion::makeNow's `failure`), as Metal's compile failures come
NSError *rmCreationError(const std::string &failure);

// RawMetalYcbcrTargets.mm: two-plane YCbCr textures rendered into (raw_ycbcr_store.h). A target's stand-in and store shape
// (Texture::planarStandIn, ycbcrStore), the reason a pass into one goes to m2v (null: none), its planes stored from its pass's
// backing of the stand-in (0; 1 a pass it cannot render, 2 a draw the encoder refused: *why), and a descriptor's first such
// attachment's format (0: none)
void makeYcbcrTarget(raw::Texture &t, uint32_t pixelFormat, const raw::YcbcrStoreShape &store);
const char *ycbcrTargetPassRefusal(const raw::Texture &t, MTLRenderPassColorAttachmentDescriptor *a);
int storeYcbcrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &standIn, const char **why);
uint32_t ycbcrTargetFormat(MTLRenderPipelineDescriptor *d);

@interface RMDevice (YcbcrTargets)
- (id<MTLRenderPipelineState>)ycbcrTargetState:(MTLRenderPipelineDescriptor *)d
                                       options:(MTLPipelineOption)o
                                    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                         error:(NSError **)error;
@end

// RawMetalParallel.mm: the command at `at` is a pass that continues the one before it (a parallel encoder's sub-encoder after
// its first); the pass at `begin` is one of such a group (pass fusion's body checks see one pass of it, not the group); and
// where the passes that continue the one at `begin` end (its own end if none does)
bool rmContinuesPass(RMCommandBuffer *cb, size_t at);
bool rmGroupedPass(RMCommandBuffer *cb, size_t begin);
size_t rmPassGroupEnd(RMCommandBuffer *cb, size_t begin);

// RawMetalReplay.mm
id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part);
// RawMetalM2vGuard.mm: what of a state m2v has no form of (null: none), and whether the range [from, to) holds such work,
// which fails its command buffer (its error set, one refusal line) instead of replaying on m2v
const char *rmM2vMissingState(MTLRenderPipelineDescriptor *d);
const char *rmM2vMissingKernel(MTLComputePipelineDescriptor *d);
bool rmRefuseM2vRange(RMCommandBuffer *cb, size_t from, size_t to);

// RawMetalEncode.mm
bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why, size_t *refusedEncoder);

// RawMetalEvents.mm
extern raw::Mutex gHoldersLock;
extern std::set<RMCommandBuffer *> gHolders;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// A held command buffer's shared event wait as it was armed, for the still-waiting line (rawStallTick); gHoldersLock
struct RMEventWaitNote {
    void *event;
    uint64_t value;
    uint64_t sinceUs, saidUs;
};

extern std::map<RMCommandBuffer *, RMEventWaitNote> gEventWaits;
#endif
bool waitsForEvent(RMCommandBuffer *cb);
void armEventWaits(RMCommandBuffer *cb);
raw::EventReadiness eventReadiness(RMCommandBuffer *cb, raw::EventDeadline::Time &nextWake);
// A value a submitted command buffer's signal reaches once its work completes (raw_event_timeline.h)
bool promisedEvent(void *event, uint64_t value);
bool expiredEventWait(RMCommandBuffer *cb, size_t index);
std::vector<RMNativeEventSignal> publishRawEventSignals(RMCommandBuffer *cb);
void retireEventSignals(const std::vector<RMNativeEventSignal> &signals, bool success);
raw::EventDependencyState validateNativeEventWaits(RMCommandBuffer *cb, uint64_t generation);
bool waitsOnRetiredProducer(RMCommandBuffer *cb);
void releaseEventWaits(RMCommandBuffer *cb);
std::shared_ptr<raw::SharedEventLease> sharedEventLease(id event);
int sharedEventReadiness(RMCommandBuffer *cb, id event, uint64_t value, bool native);
bool sharedWaitsReady(RMCommandBuffer *cb, bool native);
void prepareSharedEvents(RMCommandBuffer *cb, raw::CommandBuffer *commands);
void publishSharedEvents(RMCommandBuffer *cb);
// A kernel event's word for a mid-job release of `value` on the raw channel (submit thread, while a range is encoded): false
// where the signal goes at its job's end
bool sharedEventWord(id event, uint64_t value, uint64_t inlineStamp, uint64_t &address, uint32_t &payload);
// RawMetalInlineSignals.mm: a signal of a kernel event between two native ranges joins them (planSegments); at encode, its
// release into the event's word after the work before it
void inlineSignals(RMCommandBuffer *cb);
// `range`, now m2v's: its inline signals back as event ranges of their own between compatibility ranges (one part: it has none)
std::vector<RMSegment> compatibilityParts(RMCommandBuffer *cb, const RMSegment &range);
void beginInlineSignals(RMCommandBuffer *cb);
bool isInlineSignal(RMCommandBuffer *cb, size_t at);
bool releaseInlineSignal(RMCommandBuffer *cb, size_t at, raw::CommandBuffer *commands);
void enqueueBuffer(RMCommandBuffer *cb);

enum RMReadyDependency : uint32_t {
    RMRawProgress = 1,
    RMCompatibilityProgress = 2
};

void wakeSubmit(void *queue, uint32_t dependencies = 0);
bool m2vClear();
void planSegments(RMCommandBuffer *cb);
bool submitRange(RMCommandBuffer *cb, bool followed);
void cancelPromises(RMCommandBuffer *cb);
void unhold(RMCommandBuffer *cb);

// RawMetalPrivateComp.mm (raw_private_comp.h): submissions and the moves of a texture's memory exclude each other through
// the submit gate; a move inside a raw encode is refused
// submitRange on the submit worker, excluding the moves of compressed private textures (raw_submit_gate.h)
bool submitRangeExclusive(RMCommandBuffer *cb, bool followed);
// The gate a move of a texture's memory takes against the submit worker's submissions, and whether this thread is that
// worker inside one (RawMetalPrivateComp.mm)
raw::SubmitGate &rmSubmitGate();
bool rmInSubmission();

// `move` with no submission running: at once on the submit worker inside its own submission
template <typename Move> auto rmExcludingSubmissions(Move &&move) -> decltype(move())
{
    return rmInSubmission() ? move() : rmSubmitGate().exclude(move);
}

extern thread_local bool tRawEncoding;
uint32_t privateCompressedKind(MTLTextureDescriptor *d);
bool rmTexturePlain(RMTexture *t);
void rmRangePlain(RMCommandBuffer *cb, size_t from, size_t to);
// A pipeline bound (OpPipeline `binding`) in `pass`, which has compressed attachments: the attachments its pipeline binds
// as storage images with no barrier form to take (raw_private_comp.h storageSlotsToPlain) join the command buffer's
// _plainForStorage; a variant still being made marks the command buffer for a look at its draws before the encode
void rmNoteStorageTargets(RMCommandBuffer *cb, const RMPass &pass, const Cmd &binding);
// Before a raw range encodes, which may not ask for a move: those move to plain memory, and their shape is made plain
// from then on
void rmStorageTargetsPlain(RMCommandBuffer *cb, size_t from, size_t to);

// RawMetalCommandBuffer.mm: the kept recordings and setBytes chunks past what steady frames take, given back
void releaseRecordings();
// cb completed: its setBytes chunks kept for the next command buffer's (one of their total where it filled several)
void rmBytesDone(RMCommandBuffer *cb);
// A setBytes chunk of at least `length` bytes: one a completed command buffer gave back, else a new one (false: the kernel
// had no memory for it). One taken that leaves none kept has the next made off the recording thread.
constexpr uint32_t kRMBytesChunk = 64 << 10;  // a Core Animation frame's setBytes (~55 KB of 256-byte copies)
bool rmTakeBytesChunk(uint64_t length, raw::Buffer &chunk);
// RawMetalEncoder.mm: render, blit and compute encoders built on the storage of those before (raw_object_pool.h), from
// before the device's first command buffer
void rmPoolEncoders();
void rmPoolCommandBuffers();
// cb's state changed: its waiters woken, with no lock
void rmChanged(RMCommandBuffer *cb);
// its scheduled or completed handlers (`list`) taken, once its sent flag was exchanged: under its lock only where `added`
// says one was added
NSArray *rmTakeHandlers(RMCommandBuffer *cb, NSMutableArray *__strong &list, std::atomic<bool> &added);

// RawMetalFinish.mm
extern GpuMemoryRelease &gRawRelease;
void scheduled(RMCommandBuffer *cb);
dispatch_queue_t followingRootQueue();
dispatch_queue_t followingSerialQueue(const char *label);
void finish(RMCommandBuffer *cb, NSError *error);
// The command buffer's error, unless it has one: under its lock, which waitUntilScheduled reads it under
void failCommandBuffer(RMCommandBuffer *cb, NSString *reason,
                       MTLCommandBufferError code = MTLCommandBufferErrorInternal);
void rmRangeHandedOff(RMCommandBuffer *cb);
void segmentCompleted(RMCommandBuffer *cb, NSError *error, const raw::RetirementWrites &writes, uint32_t dependencies,
                      const std::vector<RMNativeEventSignal> &signals = {},
                      const std::vector<raw::EventTimeline::Promise> &waits = {},
                      const raw::CpuCompletion *cpu = nullptr);
void retireRawRange(RMCommandBuffer *cb, size_t from, size_t to);
const raw::RetirementWrites *captureWrittenRange(RMCommandBuffer *cb, size_t from, size_t to);
void markWrittenRange(RMCommandBuffer *cb, size_t from, size_t to);
void completionLoop();

// RawMetalSurfaceWrites.mm
SurfaceWriteCount *holdSurfaceWrites(IOSurfaceRef s);  // a texture over `s` made: its surface's count, held
void releaseSurfaceWrites(IOSurfaceRef s, SurfaceWriteCount *count);
void surfaceConverted(SurfaceWriteCount *count, bool made);  // a planar or YCbCr texture over its surface made, or gone
uint64_t surfaceWrites(const SurfaceWriteCount *count);
void noteSurfaceWrite(SurfaceWriteCount *count);
void noteSurfaceWrites(RMCommandBuffer *cb, size_t from, size_t to);  // after [from, to) of `cb` was submitted

// RawMetalDeviceBase.mm: the base class's limits table filled with the device's own answers (Metal's registration asks again:
// initLimits)
void fillDeviceLimits(id device);

// RawMetalDeviceAnswers.mm: the per-kind capability answers (raw_device_answers.h) as methods of the device class `cls`, where
// it has none of its own; and an argument query's answer
void installDeviceAnswers(Class cls);
uint64_t deviceAnswer(SEL selector, uint64_t argument);

// RawMetalSharedTextures.mm: the shared texture methods only some kinds' Metal has (raw_shared_texture.h), on `cls`
void installSharedTextureMethods(Class cls);
// The image of `d` that surface `s` holds as a shared texture of the device's layout (raw_surface_image.h): false for any
// other surface, or a Plain image
bool laidOutSurface(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::SurfaceImage &image);

// RawMetalQueuePriority.mm
void queuePriorityAdd(RMQueue *queue);
void queuePriorityRemove(RMQueue *queue);

// RawMetalSubmit.mm
void orderRefused(const char *what, int kr);
void commitBuffer(RMCommandBuffer *cb);
void submitLoop();
bool admitComputeDispatch(RMCommandBuffer *cb, size_t at, const raw::ComputeKernelSet<raw::ComputeState> *kernels,
                          const char **why);
int computeReadiness(RMCommandBuffer *cb, const RMSegment &range);

// RawMetalFootprint.mm: the idle give-back's line, `relieved` the bytes malloc gave back
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void logGivenBack(size_t relieved);
#endif

// RawMetalRates.mm
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void rawStallTick();
void watchFetchBarrierWay(raw::Device *dev);
void watchKeylineProbe();
// RawMetalGlassProbe.mm
void watchGlassProbe(raw::Device *dev);
void glassProbeCommitted(RMCommandBuffer *cb);
bool rmWholePrograms();
bool rmZeroNewBuffers();
bool rmHalfViewRefused();
void splitCaptureCommitted(raw::CommandBuffer &rc);
#endif
