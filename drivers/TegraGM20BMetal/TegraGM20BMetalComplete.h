// SPDX-License-Identifier: MIT
// The device's completion of the states and kernels its pack lacks (TegraGM20BMetal.mm), of any library's functions: each asked
// once per process and per build. A state is looked up in the installed seed and this process's table when it is created,
// then in the system shader compiler's store, and otherwise asked of that compiler (remote.h), which compiles each state
// once for every process and boot; its record is handed to the device, and for a state it refuses m2v draws it. A kernel
// the same, at the first dispatch of its key (compute_key.h: its threadgroup shape and bindings are the dispatch's).
// No state is made in a client: one no pack, seed, store or compiler answers fails its creation by name, as Metal fails a
// state it cannot compile, debug and release builds alike (a compile's memory belongs to the compiler's process).
#pragma once
#include "lock.h"
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include "compiler.h"
#include "compute_lookup.h"
#include "metallib.h"
#include "obtained.h"
#include "remote.h"
#include "render_variant.h"
#include "store_key.h"

struct NmbDirectDispatch;

namespace gm20b {

class Device;
struct ComputeRecord;
struct ComputeState;
struct MadeLibrary;
struct PackComputeFacts;
struct Pipeline;

class Completion {
public:
    // `cacheDir`: the process's own shader cache directory (null or empty: records kept in memory only).
    // `resolveCacheDir`: when non-null, resolves the writable cache directory and attaches/tidies it on persistenceQueue_.
    Completion(Device &device, const char *cacheDir, uint64_t stack, const char *seedDir = nullptr,
               std::function<std::string()> resolveCacheDir = nullptr);
    ~Completion();

    struct Ask {
        // over m2v's functions, or with resolveDescriptor the facade's, whose m2v twins are made only for a state
        // compiled in this process (m2v's translation)
        MTLRenderPipelineDescriptor *descriptor;
        std::string key;           // the pack key
        bool caTransform = false;  // Pipeline::caTransform
        // its functions are not a framework's default library's: no pack names the key, so the record is kept under the
        // key and both functions' AIR (two apps' alike-named functions are not one state)
        bool own = false;
        RenderVariant variant;  // whose name the key ends with; none: the state itself
        bool resolveDescriptor = false;
        // the descriptor over this facade's own functions, which a compile request names (nil: none can)
        MTLRenderPipelineDescriptor *facade = nil;
        uint32_t qos = 0;  // the asking thread's class, which the compiler runs it at (askVariant: 0 for its own)
        bool waited = true;
        std::function<void(std::function<bool()>)> bindWait;
    };

    // The state's pipeline handed to `ready` once made, or null for a permanent refusal. Temporary allocation
    // failures keep the request pending and retry from retained compiler output.
    void ask(Ask a, std::function<void(Pipeline *)> ready);
    // A state's creation that may complete later (Metal's completion-handler forms): `ready` gets its pipeline; or null
    // and, where its creation fails as makeNow's would, why; nothing is tried again later
    using Ready = std::function<void(Pipeline *, const std::string &failure)>;
    void create(Ask a, Ready ready);
    // A variant's ask: as ask(), and where no compiler made it (rather than refused it), why
    void askVariant(Ask a, Ready ready);
    // The state's pipeline now, on the caller's thread: its record from this process or the disk (a state made in an earlier
    // boot is raw from its first draw), else made while the thread waits, as Metal makes a state at its creation, so its
    // first draw is raw. Null: the creation fails with `failure` as Metal's does: the compiler refused the state, no request
    // names it, no compiler comes this boot, its compile could not run, or the shader heap is full.
    Pipeline *makeNow(Ask a, std::string &failure);

    // The kernel of compute key `key` (compute_key.h) for `descriptor` (over m2v's functions) at `dispatch`, placed:
    // this process's, the disk's, or made now on the caller's thread, the dispatch that first needs it. Null: refused (named
    // once in the raw log), or not made this time (*retry: the dispatch fails and the next one asks afresh).
    // *gone: no compiler made it and none will (the dispatch fails, as a kernel Metal could not compile)
    const ComputeState *kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                               const std::string &key, bool *retry = nullptr, id facadeFunction = nil,
                               bool *gone = nullptr);

    // The answers of a compute state the pack lacks (pack.h PackComputeFacts): `descriptor`'s over the facade's
    // `facadeFunction`, of compute_key.h computeStateKey `stateKey`, the shader compiler's from its store or made while
    // this thread sleeps, as a state's creation waits for its record. False with `failure` as makeNow's: no compiler
    // comes, or it made none; no process makes them itself.
    bool facts(id facadeFunction, MTLComputePipelineDescriptor *descriptor, const std::string &stateKey,
               PackComputeFacts &out, std::string &failure);
    // The library the system's Metal front end makes of code generation request `bytes` of `type` (codegen_request.h),
    // the shader compiler's from its store or made while this thread sleeps. False with `failure`: the front end's error
    // log (`refused`), or why no compiler made it; no process makes one itself.
    bool library(uint32_t type, const std::string &bytes, MadeLibrary &out, std::string &failure, bool &refused);

    void backgroundKernel(std::function<void()> work);
    // Whether every form asked in the background (askForms: barrier twins, full forms) was attached or dropped within
    // `ns`: what a test waits for before it tears down the compiler and store it serves the device with
    bool settleTwins(uint64_t ns);

    // Background forms (Barrier, Full, BarrierFull) that previously failed to reach a compiler, retried on a state's
    // later creation even when its pipeline was already placed in Device::completed_
    bool hasFormsToRetry() const
    {
        return formsRetrying_.load(std::memory_order_acquire) != 0;
    }

    void retryForms(Ask a, Pipeline *made);
    void trim();

    Compiler::Stats stats() const;

    std::string compilerStore() const
    {
        return remote_->storeIdentity();
    }

    const uint8_t *compilerStoreMapping(uint64_t &bytes) const
    {
        return remote_->storeMapping(bytes);
    }

    // A state's reflection blob (reflection_blob.h) from the system shader compiler's store, asked of the compiler where
    // the store lacks it while the calling thread waits: a render state's of `a`, or the compute state's of `function` (this
    // facade's) under `descriptor`'s limits and its computeStateKey `stateKey`. False with `why`: the compiler gave none
    bool reflection(Ask a, std::string &blob, std::string &why);
    bool reflection(id function, MTLComputePipelineDescriptor *descriptor, const std::string &stateKey,
                    std::string &blob, std::string &why);
    // The attributes blob (function_attributes.h) of `function` (this facade's), keyed by its functionAttributesKey
    bool attributes(id function, const std::string &functionKey, std::string &blob, std::string &why);
    // The buffer encoders blob (buffer_encoders.h) of `function` (this facade's), keyed by its bufferEncodersKey
    bool bufferEncoders(id function, const std::string &functionKey, std::string &blob, std::string &why);

    // A failure that is no answer about the state: no compiler serves to ask (an ask's `failure`, makeNow's)
    static constexpr const char kNoCompilerComes[] =
        "no shader compiler comes this boot: it declined, or its launcher stopped starting it";

#if SWITCH_METAL_DIAGNOSTICS
    // What the calling thread's last makeNow got its record by
    static Obtained lastObtained();
    static void noteObtained(const Obtained &o);
#endif

private:
    struct Pending;
    void request(Ask asked, std::shared_ptr<Pending> pending);
    std::string failureOf(const CacheKey &k, bool creation);
    // `qos`: the asker's class, which the record is placed at (answer_class.h)
    void place(const Ask &a, const CacheKey &key, std::shared_ptr<const RuntimeRecord> record, uint32_t qos,
               std::shared_ptr<Pending> pending);

    // TegraGM20BMetalCompleteForms.mm: what a pack would give `made`, placed of `record`, beside it: a storage form's
    // barrier twin (RequestForm::Barrier, as Device::pipeline gives a pack state its own), and the split and program
    // variants of its full form (Full, and BarrierFull for a twin once placed) where the placed record's facts say that
    // form adds them. Each from the store, else asked of the compiler at the background class as the state's creation
    // asks (remote_ask.h), so no creation waits for it; attached once made (draws load what they read of it with
    // acquire). Asked once per pipeline and form, again where no ask of it reached a compiler.
    struct FormAsk {
        Pipeline *target = nullptr;
        RequestForm form = RequestForm::Primary;
    };

    enum class FormState : uint8_t {
        Asked,
        Retry
    };

    void askForms(const Ask &a, const CacheKey &k, Pipeline *made, const RuntimeRecord &record);
    void enqueueForms(const Ask &a, const CacheKey &k, std::array<FormAsk, 2> forms, uint32_t count);
    void formOf(const Ask &a, const CacheKey &k, Pipeline *made, RequestForm form, bool again);
    void attachForm(const Ask &a, const CacheKey &k, Pipeline *made, const RuntimeRecord &record, RequestForm form);
    bool markForm(const Pipeline *made, RequestForm form);
    bool claimRetryForm(const Pipeline *made, RequestForm form);
    void unmarkForm(const Pipeline *made, RequestForm form);
    // The pack key the compiler makes `r`'s record under (TegraGM20BMetalCompileRequest.mm), which is this process's
    // own; a barrier twin's and its full form's their own beside it, so Device::addRecord files the twin apart from its
    // state, and a full form's its state's, so Device::attachFullForm finds the state
    static std::string packKeyOf(const CompileRequest &r);

    struct Kernel {
        gm20b::UnfairMutex lock;
        ComputeKernelBuild<ComputeRecord, ComputeState> build;
    };

    Device &device_;
    RecordCache cache_;
    std::unique_ptr<Compiler> compiler_;
    uint64_t stack_;
    dispatch_queue_t queue_, persistenceQueue_;
    dispatch_group_t pending_;
    dispatch_group_t forms_;  // the forms asked in the background (askForms), which ~Completion waits for too
    std::atomic<bool> stopping_{false};
    gm20b::UnfairMutex kernelLock_;  // map membership only; each kernel owns its single-flight lock
    // the pipelines and forms asked: creation threads, the answer queue and the forms' own background work ask at once,
    // never at a draw
    gm20b::UnfairMutex formLock_;
    std::map<std::pair<const Pipeline *, RequestForm>, FormState> formsAsked_;
    std::atomic<uint32_t> formsRetrying_{0};
    std::map<uint64_t, std::shared_ptr<Kernel>> kernels_;

    std::shared_ptr<Remote> remote_;

    bool keyOf(Ask &a, CacheKey &k);

    // TegraGM20BMetalCompleteRemote.mm: the system shader compiler's side
    // `every`: each library's bytes, a system one's too, as a data library
    bool renderRequest(const Ask &a, const CacheKey &k, bool bytes, CompileRequest &r, StoreKey &store,
                       bool every = false, RequestKind kind = RequestKind::Render,
                       RequestForm form = RequestForm::Primary);
    // A reflection entry from the store, else asked of the compiler with `request`'s (`every`: as renderRequest's)
    bool storedReflection(const std::function<bool(bool bytes, bool every, CompileRequest &, StoreKey &)> &request,
                          std::string &blob, std::string &why);
    // storedReflection of one function of this facade as a request's one stage tagged `tag`
    bool functionReflection(const char *tag, id function, const CacheKey &k, std::string &blob, std::string &why);
    // Whether one of `store`'s candidate names (store_key.h kStoreCandidates) holds an entry `decoded` takes as its content's
    bool storedEntry(const StoreKey &store, const std::function<bool(std::string_view bytes)> &decoded);
    // The render record the store holds under `store`, keyed by this process's pack key of it (store_key.h rekeyRecord)
    RecordCache::Found fromStore(const StoreKey &store, const std::string &packKey, RuntimeRecord &record,
                                 std::string &why);
    // Why a state's creation fails where no compiler makes it (makeNow's `failure`)
    static constexpr const char kInexpressible[] = "a function no compile request can name: another device's";
    static constexpr const char kNotTaken[] = "the shader compiler did not take the state";
    static constexpr const char kNotStored[] = "the shader compiler's store does not keep the state";
    // The state made by the compiler while this thread sleeps; false: the creation fails with `failure`
    bool remoteNow(const Ask &a, const CacheKey &k, Compiler::State &state, RuntimeRecord &record,
                   std::string &failure);
    // The state asked of the compiler with `done` called once it answers, null where it made none
    bool remoteLater(const Ask &a, const CacheKey &k, std::function<void(std::shared_ptr<const RuntimeRecord>)> done,
                     bool again = false);
    // A kernel's record from the store or made by the compiler while this thread sleeps; Failed with `taken` false: none
    // made (the kernel is refused: no process compiles one itself while a compiler serves)
    Compiler::Outcome remoteKernel(id facadeFunction, MTLComputePipelineDescriptor *descriptor,
                                   const NmbDirectDispatch &dispatch, const CacheKey &k, ComputeRecord &record,
                                   std::string &why, bool &taken);
#if SWITCH_METAL_STATS
    void logRemote(const Ask &a, const CacheKey &k, RecordCache::Found found, const char *how, const Obtained &o);
#endif
#if SWITCH_METAL_DIAGNOSTICS
    void logInProcessCa(const Ask &a, const CacheKey &k, RecordCache::Found found);
    gm20b::UnfairMutex inProcessCaLock_;  // debug-only miss path: serializes once-per-label deduplication across threads
    std::set<std::string> inProcessCaSeen_;
#endif
};

#if SWITCH_METAL_STATS
// A state's label and whole key in parts no log line cuts (a message stops near 1 KiB), logged whatever became of the
// state, beside its "a state the pack lacks" line, which keeps 400 characters of the key: tools/gpu/logharvest.py joins
// them into precompile corpus lines
void logHarvestParts(uint64_t name, NSString *label, const std::string &key);
#endif

}  // namespace gm20b
