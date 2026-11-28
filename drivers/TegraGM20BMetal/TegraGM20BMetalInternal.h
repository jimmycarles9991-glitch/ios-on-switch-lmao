// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetal.h"
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
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "GpuMemoryRelease.h"
#include "LimitsTable.h"
#include "kernel_shadow.h"
#include "SwitchFrameTimeline.h"
#include "../../kits/SwitchRefusal/SwitchRefusalSite.h"
#include "TegraGM20BMetalComplete.h"
#include "follow.h"
#include "gpu.h"
#include "handoff_queue.h"
#include "progress_gate.h"
#include "texture_shape.h"
#include "TegraGM20BLayout.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "TegraGM20BMetalObjects.h"
#include "retirement.h"

extern Class gSwitchMetalBufferClass;
extern Class gSwitchMetalTextureClass;
extern Class gSwitchMetalNoCopyTextureClass;
extern Class gSwitchMetalSamplerClass;
extern Class gSwitchMetalPipelineClass;
extern Class gSwitchMetalDepthStencilClass;
extern Class gSwitchMetalHeapClass;
extern Class gSwitchMetalComputeStateClass;

static inline void *gm20bAsClassPtr(__unsafe_unretained id object, Class cls)
{
    return object && object_getClass(object) == cls ? (__bridge void *)object : nullptr;
}

static inline void *gm20bAsTexturePtr(__unsafe_unretained id object)
{
    if (!object)
        return nullptr;
    Class cls = object_getClass(object);
    return (cls == gSwitchMetalTextureClass || cls == gSwitchMetalNoCopyTextureClass) ? (__bridge void *)object
                                                                                      : nullptr;
}

#define gm20bAsBuffer(object)       ((__bridge SwitchMetalBuffer *)gm20bAsClassPtr((object), gSwitchMetalBufferClass))
#define gm20bAsTexture(object)      ((__bridge SwitchMetalTexture *)gm20bAsTexturePtr((object)))
#define gm20bAsSampler(object)      ((__bridge SwitchMetalSampler *)gm20bAsClassPtr((object), gSwitchMetalSamplerClass))
#define gm20bAsPipeline(object)     ((__bridge SwitchMetalPipeline *)gm20bAsClassPtr((object), gSwitchMetalPipelineClass))
#define gm20bAsDepthStencil(object) ((__bridge SwitchMetalDepthStencil *)gm20bAsClassPtr((object), gSwitchMetalDepthStencilClass))
#define gm20bAsHeap(object)         ((__bridge SwitchMetalHeap *)gm20bAsClassPtr((object), gSwitchMetalHeapClass))

// Buffer-backed texture views share the caller's cached CPU pointer with their parent buffer.
static inline SwitchMetalBuffer *cpuBufferOfResource(id object)
{
    if (SwitchMetalBuffer *buffer = gm20bAsBuffer(object))
        return buffer;
    SwitchMetalTexture *texture = gm20bAsTexture(object);
    if (!texture)
        return nil;
    while (texture->_kind == SwitchMetalTextureView && texture->_parent)
        texture = texture->_parent;
    return texture->_buffer;
}

// A wait the plugin bounds (r9-hang: r7n's and r7o's freezes were a userland wait with no end in backboardd's render thread):
// past kWaitLimit, one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze, the userland twin of the kernel's missing-interrupt panic. Nothing waits that long when it works: within 10 s the kernel
// kills the channel a GPU wait is stuck on (or panics).
static constexpr auto kWaitLimit = std::chrono::seconds(20);
[[noreturn]] void waitTooLong(const char *what, const void *object);

os_log_t gm20bLog();
void once(NSString *text);
// The device's handle tables of argument buffers (TegraGM20BMetalResourceIds.mm): an object whose ID was named going
void gm20bHeapForgetTexture(SwitchMetalTexture *t);
void gm20bHeapForgetSampler(SwitchMetalSampler *s);
// A resource a heap made outside its arena (TegraGM20BMetalHeap.mm): a member of what a job names of the heap until it goes
void gm20bHeapHold(SwitchMetalHeap *heap, const gm20b::HeapMember &member);
void gm20bHeapForget(SwitchMetalHeap *heap, uint64_t va);
// newArgumentEncoderWithBufferIndex: of a function (TegraGM20BMetalArgumentEncoder.mm): nil where Metal makes no encoder
id<MTLArgumentEncoder> gm20bFunctionArgumentEncoder(SwitchMetalFunction *function, NSUInteger index);
double hostSeconds();
uint64_t nowUs();

struct Stats {
#if SWITCH_METAL_DIAGNOSTICS
    std::atomic<uint64_t> raw{0}, deferred{0}, encodeUs{0}, draws{0}, passes{0}, copies{0}, eventsReleased{0},
        heldReturns{0};
    // shared event signals published as their raw work completes (publishRawEventSignals), those the CPU set after the head
    // waited for its own work, and native event waits that went on a submitted command buffer's promise
    std::atomic<uint64_t> signalsPublished{0}, signalsAfterWork{0}, waitsOnPromise{0};
    // event waits the scheduler held its queue for on the CPU (a listener armed): no submitted producer, or a timeout
    std::atomic<uint64_t> waitsHeldOnCpu{0};
    // predicated loops laid down again after their job, where they would have run past their copies
    std::atomic<uint64_t> loopContinuations{0};
    // command buffers completed (finish): with the commits, what the stall line (gm20bStall) watches
    std::atomic<uint64_t> finished{0};
    // waitUntilScheduled / waitUntilCompleted calls that blocked, and the time they blocked: CPU waits on the caller's thread
    // (Core Animation's render thread flushes with waitUntilScheduled every frame)
    std::atomic<uint64_t> scheduledWaits{0}, scheduledWaitUs{0}, completedWaits{0}, completedWaitUs{0};
    // encoders the worker encoded before their command buffer's commit (TegraGM20BMetalPipeline.mm), of the encoders of the raw
    // command buffers it sealed; encodes ahead given back, by the first reason (SwitchMetalPipelineVeto's bit)
    std::atomic<uint64_t> pipelinedEncoders{0}, sealedEncoders{0}, pipelineAbandons[VetoKinds]{};
    // command buffers handed to the worker as encoders ended, and those hand-offs that found it asleep (woken by them)
    std::atomic<uint64_t> handoffs{0}, handoffWakes{0};
#if SWITCH_FRAME_TIMELINE
    // command buffers completed before their submitting thread scheduled them (finish() ran their scheduled handlers)
    std::atomic<uint64_t> scheduledByFinish{0};
    std::atomic<uint64_t> zeroAnswers{0};  // selectors answered zero (answerZero)
#endif
#endif
};

// A wait that blocked, counted with its time into two Stats counters when it ends (stats_gpu builds; nothing otherwise).
class BlockedWait {
public:
#if SWITCH_METAL_STATS
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
// subclass of _MTLDevice (metal_device_open) that conforms to Metal's protocols. It has no instance variables of its own
// (the base class's ABI is fixed), so its state lives here.
struct SwitchMetalNativeEventSignal {
    void *event;
    gm20b::EventTimeline::Promise promise;
};

struct SwitchMetalRawRetirement {
    SwitchMetalCommandBuffer *buffer;
    uint32_t seqno;
    const gm20b::RetirementWrites *writes;  // the command buffer's (SwitchMetalCommandBuffer::_retirements)
    std::vector<SwitchMetalNativeEventSignal> signals;
    std::vector<gm20b::EventTimeline::Promise> waits;
    uint64_t generation = 0;
    gm20b::CpuCompletion cpu;
    bool quiet = false;  // its job raised no interrupt: a later range's covers it (completion_source.h)
    // the range laid predicated loops: the stamp of the record that names one which ran past its copies, and the range
    uint32_t loopStamp = 0;
    uint32_t loopFrom = 0, loopTo = 0;
    uint64_t frameDue = 0;  // left quiet for its frame's loud end, due by then (completion_source.h completionLook)
};

// A move of a texture's memory another thread hands the submit worker (gm20bExcludingSubmissions): `work`, run between the
// worker's submissions, then `done` signalled to the giver, which waits for it
struct SwitchMetalMove {
    const std::function<void()> *work;
    dispatch_semaphore_t done;
    bool yieldAll = true;
};

// A change of the submit worker's schedule another thread hands it (GM20BState::schedule)
struct SwitchMetalScheduleChange {
    enum Kind : uint8_t {
        Enqueue,
        Commit,
        Wake,
        Encoded,  // more of its tape is final (gm20bPublishEncoder)
        Move
    } kind = Wake;

    SwitchMetalCommandBuffer *buffer = nil;  // Enqueue, Commit, Encoded
    void *queue = nullptr;                   // Wake: the queue whose head it wakes (null: none)
    uint32_t dependencies = 0;               // Wake: the progress it reports (SwitchMetalReadyDependency)
    SwitchMetalMove *move = nullptr;         // Move
};

struct GM20BState {
    gm20b::Device *raw = nullptr;
    std::shared_ptr<gm20b::SharedEvents> sharedEvents;
    std::atomic<bool> eventWords{true};  // the kernel has TegraGM20BJobABI.h method 40 (kernel events' words)
    uint64_t registryID = 0;
    id device;  // the device object
    // The submit worker's own: every command buffer's place in its queue's order (TegraGM20BMetalSchedule.mm). Other threads
    // change it through `schedule`, the changes the worker applies before it takes the next command buffer.
    gm20b::HandoffQueue<SwitchMetalScheduleChange> schedule;
    gm20b::SubmitQueue<void *, SwitchMetalCommandBuffer *> waiting;
    std::atomic<uint32_t> waitingCommitted{0};  // waiting.committed() as the worker last saw it (the stall line's)
    gm20b::UnfairMutex eventValues;
    gm20b::EventTimeline events;
    MTLSharedEventListener *listener;  // the CPU-ordered remainder's event waits
    // immutable range fences, the submit worker's to the completion thread; later submissions never change an earlier wait
    gm20b::HandoffQueue<SwitchMetalRawRetirement> inflight;
    // the submit thread will put a job that interrupts behind the quiet ranges in flight (stored before each hand-off);
    // threads in waitUntilCompleted. A change of either wakes the completion thread (inflight.wake()).
    std::atomic<bool> submitPromised{false};
    std::atomic<uint32_t> completionWaiters{0};
    dispatch_queue_t handlers;           // where scheduled handlers run (in order, never inside commit)
    gm20b::ProgressGate progress;        // whether a completion wakes the submit worker (progress_gate.h)
    std::atomic<uint32_t> committed{0};  // command buffers committed and not finished (the arenas' purge)
    // the submit worker is about to wait or waits for a change: a command buffer's hand-offs after its first wake it only
    // then (gm20bPublishEncoder; with its _published, a store then a load on each side, both sequentially consistent)
    std::atomic<bool> submitAsleep{false};
    std::atomic<uint32_t> submitThread{0};  // mach thread port name of submitLoop, for OwnedWord priority inheritance
    Stats stats;
    std::unique_ptr<gm20b::Completion> completion;  // the states the pack lacks (metal_complete_states)
};

extern GM20BState *gRM;

// TegraGM20BMetalPredicates.mm: the signature a Metal protocol declares for `selector` (null: none), and a selector the object has
// no method for answered zero, with an error line
const char *metalTypes(SEL selector);
void answerZero(id object, NSInvocation *invocation);
gm20b::shapes::TextureShape textureShape(MTLTextureDescriptor *d);
bool gm20bShape(MTLTextureDescriptor *d);
bool chainShape(MTLTextureDescriptor *d);
bool lineShape(MTLTextureDescriptor *d);
bool volumeShape(MTLTextureDescriptor *d);
bool volumeRendered(MTLTextureDescriptor *d);
uint32_t arrayType(MTLTextureDescriptor *d);
bool zetaDescriptor(MTLTextureDescriptor *d);
bool multisampleShape(MTLTextureDescriptor *d);
uint32_t memorylessLayers(MTLTextureDescriptor *d);
bool blockShape(MTLTextureDescriptor *d);
bool cpuBlocks(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
               NSUInteger image, bool write);
uint32_t gm20bShapeTerm(MTLTextureDescriptor *d);
bool identitySwizzle(MTLTextureSwizzleChannels s);
// `t` reading its channels through `s` (a descriptor's swizzle, or a swizzled view's): its raw header, its descriptor
id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s);
// A visibility result mode Metal's validation stops (MTLRenderCommandEncoder.h setVisibilityResultMode:offset:): `why`,
// in the predicate's first line
void visibilityError(SwitchMetalCommandBuffer *cb, uint64_t mode, uint64_t offset, const char *why);
void eventError(SwitchMetalCommandBuffer *cb, bool signal, const char *why);
bool keepExtendedRange(MTLTextureDescriptor *d, gm20b::Texture &t, bool overBuffer);
// The header type (gm20b::fx::kTex*) of a texture or view of `type`; -1: one the driver makes none of
int viewHeaderType(MTLTextureType type);

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

// TegraGM20BMetalBlitRules.mm
bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp, bool intoTexture,
                BufferRows &out);
uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y);

// TegraGM20BMetalLibrary.mm
// A library of the metallib `bytes` (a file's at `path`, or data) read by the facade; nil: bytes it does not read
SwitchMetalLibrary *readLibrary(id device, NSString *path, NSData *bytes);
// gm20b::FunctionSource for the facade's functions (TegraGM20BMetalRenderKey.h): the AIR slice its library read
bool gm20bFunctionSource(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name);
std::string libraryName(SwitchMetalFunction *f);

// The device SPI the device answers (TegraGM20BMetalDevice.mm, TegraGM20BMetalDeviceAnswers.mm)
@protocol SwitchMetalDeviceSPI
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface;
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)descriptor;
@end

// A packed state's run kept for its variants, each placed when a draw first selects its name (TegraGM20BMetalCubic.mm)
void packedVariants(SwitchMetalPipeline *p, const gm20b::PackRun &run);
// A state this process made: the variants its creation already names (its test classes), made and waited for before
// the creation returns, as the pack builder makes a packed state's
void gm20bMadeVariants(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d);

// TegraGM20BMetalHeap.mm: the purged heaps a committed command buffer's resources lie in, given their pages back before it is encoded
// (it counts as committed already, so no purge runs meanwhile)
void restoreArenas(SwitchMetalCommandBuffer *cb);
// The device's ask of `d` in an arena of `blockLinear` (a private heap's): every kind the driver keeps; false: none
bool heapTextureAsk(MTLTextureDescriptor *d, bool blockLinear, gm20b::TextureAsk &ask);
// TegraGM20BMetalPlainLayout.mm: the texture layout object of a texture no block-linear image of caller memory is made of, nil for
// one it does not describe
id gm20bPlainLayout(id device, MTLTextureDescriptor *d);

// TegraGM20BMetalTexture.mm
void declareGpuWrites(SwitchMetalTexture *t);
#if SWITCH_FRAME_TIMELINE
void traceSurfaceImage(SwitchMetalTexture *t, const char *what, uint32_t seed);
#endif

// TegraGM20BMetalEncode.mm: the imports a texture's or buffer's memory lies in, named to the submission
void useTexture(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, bool write);
void useBuffer(gm20b::CommandBuffer *rc, SwitchMetalBuffer *b, bool write);
// TegraGM20BMetalDeviceBase.mm: residency sets on queues and command buffers, snapshotted at commit and named at seal
void gm20bQueueAddResidencySet(SwitchMetalQueue *queue, id set);
void gm20bQueueRemoveResidencySet(SwitchMetalQueue *queue, id set);
void gm20bUseResidencySet(SwitchMetalCommandBuffer *cb, id set);
void gm20bSnapshotResidencySets(SwitchMetalCommandBuffer *cb);
void gm20bNameResidencySets(SwitchMetalCommandBuffer *cb, gm20b::CommandBuffer *rc);
// TegraGM20BMetalTessellation.mm: a patch draw on the raw encoder
bool encodePatches(gm20b::RenderEncoder *re, gm20b::CommandBuffer *rc, const SwitchMetalPlanDraw &draw,
                   const SwitchMetalPlanPatches &patches, bool indirect, const char **why);

// TegraGM20BMetalEncodeBlit.mm: a blit encoder's copy or fill of the tape onto the driver's copy engine (a fill's rectangle on the
// 2D engine where rule clear.twod holds); a texture it writes leaves `fresh` and `standInFresh` (its pool image and its
// stand-in are refreshed again before a sample). A copy Apple leaves clamped is drawn back clamped (TegraGM20BMetalCopyClamp.h):
// false where that had no memory, header or pack state (*why); `be` the blit encoder encoding goes on with
class SwitchMetalStandInFresh;
bool encodeBlit(SwitchMetalCommandBuffer *cb, const Cmd &c, gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be,
                std::set<SwitchMetalTexture *> &fresh, SwitchMetalStandInFresh &standInFresh, const char **why);
// Levels first + 1 .. last of a chain by a pass of the pack's mip state a level (mip.h); false where the pack has none or a
// pass is refused (*why)
bool mipPasses(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, uint32_t first, uint32_t last, const char **why);
// A chain's generateMipmapsForTexture: each step on the 2D engine or by mipPasses (fx/fx_mip_route.h); `be` is the blit
// encoder encoding goes on with; `probe2d` the command's probe format (fx_mip_probe.h), 0 for none
bool encodeMipmaps(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, uint32_t probe2d, gm20b::BlitEncoder *&be,
                   const char **why);

// TegraGM20BMetalTextureCpu.mm: `bytes` from `from` cleaned to memory (or cleaned and invalidated), then a DSB
void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate);
bool transferTexture(SwitchMetalTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, void *bytes,
                     NSUInteger row, NSUInteger image, bool write);
// A depth/stencil texture's CPU copy transferTexture did not make: one error line, and nothing else moves it (its zeta
// memory is the copy engine's alone); false for any other texture
bool refuseZetaCpu(SwitchMetalTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, bool write);
void cpuTiled(SwitchMetalTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);
const tgm_layout *textureCpuImportLayout(SwitchMetalTexture *texture);
void cpuLevel(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
              NSUInteger image, bool write);
void cpuVolume(SwitchMetalTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write);
void cpuLayer(SwitchMetalTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write);
bool cpuSamples(SwitchMetalTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);
bool cpuXrWords(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
                NSUInteger image, bool write);

// TegraGM20BMetalDevice.mm
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, gm20b::YcbcrPlan &out);

// TegraGM20BMetalUnsupported.mm: why the device has no form of a state of `d` (null: it has), and that refusal as a creation answers
// it (nil, its error)
const char *gm20bUnsupportedRenderState(MTLRenderPipelineDescriptor *d);
id gm20bRefuseUnsupported(const char *why, NSError **error);

// TegraGM20BMetalStates.mm: the state of `d` as the pack and the archives it lists answer, nil where it is Metal's binary archive
// miss (`*error`), its record made at once where neither holds it (nil where its creation fails, `*error`); and whether the
// archives miss the state of `d` (its functions this device's)
SwitchMetalPipeline *gm20bPipelineFor(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                      NSError **error);
bool gm20bRenderArchiveMissed(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);
// gm20bPipelineFor's parts: the state as the pack and the archives answer, whether its record is still to be made and what to
// ask the runtime for, and the state handed to its creator once its record is there, or nil with its error
SwitchMetalPipeline *gm20bPipelineShell(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                        NSError **error);
bool gm20bMakeRecord(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, NSError **error);
bool gm20bNeedsRecord(SwitchMetalPipeline *p);
gm20b::Completion::Ask gm20bRecordAsk(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d);
id<MTLRenderPipelineState> gm20bFinishState(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                            MTLAutoreleasedRenderPipelineReflection *reflection, NSError **error);
// The error a state's creation fails with (Completion::makeNow's `failure`), as Metal's compile failures come
NSError *gm20bCreationError(const std::string &failure);

// TegraGM20BMetalYcbcrTargets.mm: two-plane YCbCr textures rendered into (ycbcr_store.h). A target's stand-in and store shape
// (Texture::planarStandIn, ycbcrStore), the reason a pass into one fails (null: none), its planes stored from its pass's
// backing of the stand-in (0; 1 a pass it cannot render, 2 a draw the encoder refused: *why; `loaded`: the reach of a pass that
// loaded it, whose store writes the tiles it reaches alone), and a descriptor's first attachment a stand-in renders (YCbCr or
// format_standin.h's formats; 0: none), whose state standInTargetState makes. A loaded target's pass at colour `j` renders into `loaded`, a stand-in of the command buffer's that a pass of the load state
// filled first (0, or as storeYcbcrPlanes')
void makeYcbcrTarget(gm20b::Texture &t, uint32_t pixelFormat, const gm20b::YcbcrStoreShape &store);
const char *ycbcrTargetPassRefusal(const gm20b::Texture &t, MTLRenderPassColorAttachmentDescriptor *a);
int storeYcbcrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, const gm20b::Texture &standIn, const char **why,
                     const gm20b::PassReach *loaded = nullptr);
int loadYcbcrStandIn(gm20b::CommandBuffer *rc, const gm20b::Texture &t, uint32_t j, gm20b::Texture *&loaded,
                     const char **why);
bool reachOfLoadedTargets(const SwitchMetalPass &pass, const gm20b::PassDesc &p, gm20b::DrawReplay &draws,
                          gm20b::CommandBuffer &rc, gm20b::PassReach &out, const char **why);
uint32_t standInTargetFormat(MTLRenderPipelineDescriptor *d);
// TegraGM20BMetalEncodeBlit.mm: whether the pack holds the mip state of a format the GM20B renders none of (format_standin.h)
bool gm20bFormatMips(uint32_t format);
// A stand-in's attachment of a format with no alpha: its factors of the destination alpha the constants they come to
// (standin_blend.h)
void gm20bAlphaOneBlend(MTLRenderPipelineColorAttachmentDescriptor *a);

@interface SwitchMetalDevice (YcbcrTargets)
- (id<MTLRenderPipelineState>)standInTargetState:(MTLRenderPipelineDescriptor *)d
                                         options:(MTLPipelineOption)o
                                      reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                           error:(NSError **)error;
@end

// TegraGM20BMetalParallel.mm: the command at `at` is a pass that continues the one before it (a parallel encoder's sub-encoder after
// its first); the pass at `begin` is one of such a group (pass fusion's body checks see one pass of it, not the group)
bool gm20bContinuesPass(SwitchMetalCommandBuffer *cb, size_t at);
bool gm20bGroupedPass(SwitchMetalCommandBuffer *cb, size_t begin);
// TegraGM20BMetalParallel.mm: `cb` is the recorder of a sub-encoder whose parallel encoder ended, the draw being recorded
// skipped and counted; false: it is not
bool gm20bDrawAfterParallelEnd(SwitchMetalCommandBuffer *cb);

// TegraGM20BMetalDrawErrors.mm: a draw refused as `f` (draw_refusal.h) that goes nowhere else: its line logged, the
// command buffer failed (with Metal's error where Metal fails it too); false: a refusal of another kind
bool drawFailed(SwitchMetalCommandBuffer *cb, const gm20b::Refusal &f, const char *why);
// A draw into a plane of rotation `turn` of the state of `keyHash` (its key text `key`, null where unknown) whose
// record holds no turned fragment program (PipelineTurnRefused): its line logged, the command buffer failed
void gm20bTurnRefused(SwitchMetalCommandBuffer *cb, uint64_t keyHash, uint32_t turn, const char *key);

// TegraGM20BMetalEncode.mm: a range onto the driver. `stopped`: nothing of it was submitted, the command buffer's error says why
// (*why where none does); `producerPending`: an event producer it waits on is pending on a channel made again since
enum class SwitchMetalEncoded {
    submitted,
    stopped,
    producerPending
};
SwitchMetalEncoded encodeRaw(SwitchMetalCommandBuffer *cb, size_t from, size_t to, const char **why);

// TegraGM20BMetalEvents.mm
extern gm20b::UnfairMutex gHoldersLock;
extern std::set<SwitchMetalCommandBuffer *> gHolders;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// A held command buffer's shared event wait as it was armed, for the still-waiting line (gm20bStallTick); gHoldersLock
struct SwitchMetalEventWaitNote {
    void *event;
    uint64_t value;
    uint64_t sinceUs, saidUs;
};

extern std::map<SwitchMetalCommandBuffer *, SwitchMetalEventWaitNote> gEventWaits;
#endif
bool waitsForEvent(SwitchMetalCommandBuffer *cb);
void armEventWaits(SwitchMetalCommandBuffer *cb);
gm20b::EventReadiness eventReadiness(SwitchMetalCommandBuffer *cb, gm20b::EventDeadline::Time &nextWake);
// A value a submitted command buffer's signal reaches once its work completes (event_timeline.h)
bool promisedEvent(void *event, uint64_t value);
std::vector<SwitchMetalNativeEventSignal> publishRawEventSignals(SwitchMetalCommandBuffer *cb);
void retireEventSignals(const std::vector<SwitchMetalNativeEventSignal> &signals, bool success);
gm20b::EventDependencyState validateNativeEventWaits(SwitchMetalCommandBuffer *cb, uint64_t generation);
void releaseEventWaits(SwitchMetalCommandBuffer *cb);
std::shared_ptr<gm20b::SharedEventLease> sharedEventLease(id event);
int sharedEventReadiness(SwitchMetalCommandBuffer *cb, id event, uint64_t value, bool native);
bool sharedWaitsReady(SwitchMetalCommandBuffer *cb, bool native);
void prepareSharedEvents(SwitchMetalCommandBuffer *cb, gm20b::CommandBuffer *commands);
void publishSharedEvents(SwitchMetalCommandBuffer *cb);
// A kernel event's word for a mid-job release of `value` on the raw channel (submit thread, while a range is encoded): false
// where the signal goes at its job's end
bool sharedEventWord(id event, uint64_t value, uint64_t inlineStamp, uint64_t &address, uint32_t &payload);
// TegraGM20BMetalInlineSignals.mm: a signal of a kernel event between two native ranges joins them (planSegments); at encode, its
// release into the event's word after the work before it
void inlineSignals(SwitchMetalCommandBuffer *cb);
void beginInlineSignals(SwitchMetalCommandBuffer *cb);
bool isInlineSignal(SwitchMetalCommandBuffer *cb, size_t at);
bool releaseInlineSignal(SwitchMetalCommandBuffer *cb, size_t at, gm20b::CommandBuffer *commands);
void enqueueBuffer(SwitchMetalCommandBuffer *cb);

enum SwitchMetalReadyDependency : uint32_t {
    SwitchMetalRawProgress = 1
};

void wakeSubmit(void *queue, uint32_t dependencies = 0);
void planSegments(SwitchMetalCommandBuffer *cb);
bool submitRange(SwitchMetalCommandBuffer *cb, bool followed);
void cancelPromises(SwitchMetalCommandBuffer *cb);
void unhold(SwitchMetalCommandBuffer *cb);
void heldTooLong(SwitchMetalCommandBuffer *cb, uint64_t value, uint64_t reached);

// TegraGM20BMetalPrivateComp.mm (private_comp.h): the moves of a texture's memory are the submit worker's, between its
// submissions; a move inside a raw encode is refused
// submitRange on the submit worker, marked as its submission
bool submitRangeExclusive(SwitchMetalCommandBuffer *cb, bool followed);
// Whether this thread is the submit worker inside a submission
bool gm20bInSubmission();
// TegraGM20BMetalSchedule.mm: whether this thread is the submit worker, and `work` run by it between its submissions with no
// encode ahead open (a Move change from any other thread, which waits for it)
bool gm20bOnSubmitWorker();
void gm20bRunOnSubmitWorker(const std::function<void()> &work);
void gm20bReleaseAhead(bool yieldOpen);

// `move` with no submission running and no encode ahead holding the device's command buffer: at once inside the submit
// worker's own submission, else on the worker in its order
template <typename Move> auto gm20bExcludingSubmissions(Move &&move) -> decltype(move())
{
    if (gm20bInSubmission())
        return move();
    std::optional<decltype(move())> result;
    gm20bRunOnSubmitWorker([&] {
        result = move();
    });
    return *result;
}

void gm20bMarkSubmission(bool submitting);

// `run` on the submit worker as a submission of its own: an encode ahead's step
template <typename Run> auto gm20bSubmission(Run &&run) -> decltype(run())
{
    gm20bMarkSubmission(true);

    struct Unmark {
        ~Unmark()
        {
            gm20bMarkSubmission(false);
        }
    } unmark;

    return run();
}

uint32_t privateCompressedKind(MTLTextureDescriptor *d);
// A texture gm20bNewTexture made plain for want of compressed memory (its _movableBytes) registered as wanting it, once its
// descriptor's swizzle is in its header: Device::swizzle writes that header in place, and from the registration on the
// submit worker may move the texture, header words and all
void gm20bWantCompressed(id<MTLTexture> texture);
// The submit worker's, inside a raw range's submission before its encode, where memory made since its last look serves a
// texture made plain for want of it: each such texture moved there (gm20b::Device::compressedFromPlain), used by this range
// or not
void gm20bMoveWanting();

// TegraGM20BMetalCompiledLibraries.mm: drop cached front-end function specializations on memory pressure
void gm20bTrimCompiledLibraries();
// TegraGM20BMetalCommandBuffer.mm: the kept recordings and setBytes chunks past what steady frames take, given back
void releaseRecordings();
void gm20bTakeRecording(SwitchMetalCommandBuffer *cb);
void gm20bRecordingDone(SwitchMetalCommandBuffer *cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// gpu.path.encode_cost's: command buffers that found no kept recording, and recordings given back but not kept (over
// kKeptBytes alone, over kKeptTotal with those kept, or every slot full), since the process began
struct SwitchMetalRecordingCounts {
    uint64_t misses, dropsLarge, dropsTotal, dropsSlots;
};

SwitchMetalRecordingCounts gm20bRecordingCounts();
#endif
// cb completed: its setBytes chunks kept for the next command buffer's (one of their total where it filled several)
void gm20bBytesDone(SwitchMetalCommandBuffer *cb);
// A setBytes chunk of at least `length` bytes: one a completed command buffer gave back, else a new one (false: the kernel
// had no memory for it). One taken that leaves none kept has the next made off the recording thread.
constexpr uint32_t kRMBytesChunk = 64 << 10;  // a Core Animation frame's setBytes (~55 KB of 256-byte copies)
bool gm20bTakeBytesChunk(uint64_t length, gm20b::Buffer &chunk);
// TegraGM20BMetalEncoder.mm: render, blit and compute encoders built on the storage of those before (object_pool.h), from
// before the device's first command buffer
void gm20bPoolEncoders();
void gm20bPoolCommandBuffers();
// cb's state changed: its waiters woken, with no lock
void gm20bChanged(SwitchMetalCommandBuffer *cb);
void gm20bSetCompileWaits(SwitchMetalCommandBuffer *cb, std::vector<std::function<bool()>> waits);
void gm20bClearCompileWaits(SwitchMetalCommandBuffer *cb);
void gm20bNoteCompileWaitReady(SwitchMetalCommandBuffer *cb);
// its scheduled or completed handlers (`list`) taken, once its sent flag was exchanged: under its lock only where `added`
// says one was added
NSArray *gm20bTakeHandlers(SwitchMetalCommandBuffer *cb, NSMutableArray *__strong &list, std::atomic<bool> &added);

// TegraGM20BMetalFinish.mm
extern GpuMemoryRelease &gRawRelease;
void scheduled(SwitchMetalCommandBuffer *cb);
dispatch_queue_t followingRootQueue();
dispatch_queue_t followingSerialQueue(const char *label);
void finish(SwitchMetalCommandBuffer *cb, NSError *error);
// The command buffer's error, unless it has one: under its lock, which waitUntilScheduled reads it under. `p`, the
// predicate the failure names, goes with the error it sets (SwitchMetalCommandBuffer::_failedBy).
void failCommandBuffer(SwitchMetalCommandBuffer *cb, NSString *reason,
                       MTLCommandBufferError code = MTLCommandBufferErrorInternal, SwitchMetalPred p = PredNone);
void gm20bRangeHandedOff(SwitchMetalCommandBuffer *cb);
void segmentCompleted(SwitchMetalCommandBuffer *cb, NSError *error, const gm20b::RetirementWrites &writes,
                      uint32_t dependencies, const std::vector<SwitchMetalNativeEventSignal> &signals = {},
                      const std::vector<gm20b::EventTimeline::Promise> &waits = {},
                      const gm20b::CpuCompletion *cpu = nullptr);
void retireRawRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to);
const gm20b::RetirementWrites *captureWrittenRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to);
void markWrittenRange(SwitchMetalCommandBuffer *cb, size_t from, size_t to);
void completionLoop();

// TegraGM20BMetalSurfaceWrites.mm
SurfaceWriteCount *holdSurfaceWrites(IOSurfaceRef s);  // a texture over `s` made: its surface's count, held
void releaseSurfaceWrites(IOSurfaceRef s, SurfaceWriteCount *count);
void surfaceConverted(SurfaceWriteCount *count, bool made);  // a planar or YCbCr texture over its surface made, or gone
uint64_t surfaceWrites(const SurfaceWriteCount *count);
void noteSurfaceWrite(SurfaceWriteCount *count);
void noteSurfaceWrites(SwitchMetalCommandBuffer *cb, size_t from, size_t to);  // after [from, to) of `cb` was submitted

// TegraGM20BMetalDeviceBase.mm: the base class's limits table filled with the device's own answers (Metal's registration asks again:
// initLimits)
void fillDeviceLimits(id device);

// TegraGM20BMetalDeviceAnswers.mm: the per-kind capability answers (device_answers.h) as methods of the device class `cls`, where
// it has none of its own; and an argument query's answer
void installDeviceAnswers(Class cls);
uint64_t deviceAnswer(SEL selector, uint64_t argument);

// TegraGM20BMetalSharedTextures.mm: the shared texture methods only some kinds' Metal has (shared_texture.h), on `cls`
void installSharedTextureMethods(Class cls);
// The image of `d` that surface `s` holds as a shared texture of the device's layout (surface_image.h): false for any
// other surface, or a Plain image
bool laidOutSurface(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, gm20b::SurfaceImage &image);

// TegraGM20BMetalQueuePriority.mm
void queuePriorityAdd(SwitchMetalQueue *queue);
void queuePriorityRemove(SwitchMetalQueue *queue);

// TegraGM20BMetalSubmit.mm
void commitBuffer(SwitchMetalCommandBuffer *cb);
void submitLoop();
bool admitComputeDispatch(SwitchMetalCommandBuffer *cb, size_t at,
                          const gm20b::ComputeKernelSet<gm20b::ComputeState> *kernels, const char **why);
// 1 ready, 0 still being made, -1 refused (`why`), -2 failed to compile
int computeReadiness(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range, const char **why,
                     std::vector<std::function<bool()>> &compileWaits);
// TegraGM20BMetalComputeAdmission.mm: an admitted dispatch's textures and buffers, and the encoder's resources it may reach
void kernelUses(SwitchMetalCommandBuffer *cb, size_t at, std::vector<gm20b::KernelUse> &uses);

// TegraGM20BMetalFootprint.mm: the idle give-back's line, `relieved` the bytes malloc gave back, `passes` and `kept` what the
// passes' copies and the kept surface imports held until then
#if SWITCH_METAL_STATS
struct SwitchMetalPassTrim;
void logGivenBack(size_t relieved, const SwitchMetalPassTrim &passes, const gm20b::Device::Trimmed &kept,
                  const gm20b::Device::PurgeStats &purge);
// The memory line, on the stall line's tick (its queue owns the line's state): what the device holds by kind beside the
// process's footprint and its limit, whenever either moved since the last
void gm20bMemoryTick();
#endif

// TegraGM20BMetalRates.mm
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void gm20bStallTick();
void watchFetchBarrierWay(gm20b::Device *dev);
void watchDebugSwitches(gm20b::Device *dev);
void splitCaptureCommitted(gm20b::CommandBuffer &rc);
// TegraGM20BMetalDrawTimes.mm
void watchFrameWeights(gm20b::Device *dev);
#endif
