// SPDX-License-Identifier: MIT
// See RawComplete.h.
#import "RawComplete.h"

#include <algorithm>
#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "RawMetalInternal.h"
#include "RawRenderKey.h"
#include "raw_compute.h"
#include "raw_gpu.h"

namespace raw {
namespace {

constexpr uint64_t kCacheCap = 8u << 20;

// A compute key (raw_compute_key.h) with its function constants moved last: a log line cut short keeps its shape and bindings
std::string constantsLast(const std::string &key)
{
    const size_t constants = key.find(" | "), shape = key.find(" | max=");
    if (constants == std::string::npos || shape == std::string::npos || shape < constants)
        return key;
    return key.substr(0, constants) + key.substr(shape) + " | constants" +
           key.substr(constants + 2, shape - constants - 2);
}

}  // namespace

#if SWITCH_RAW_DIAGNOSTICS
void logHarvestParts(uint64_t name, NSString *label, const std::string &key)
{
    std::string text = std::string("label=") + (label.length ? label.UTF8String : "-");
    std::replace(text.begin(), text.end(), ' ', '_');
    text += " " + key;
    constexpr size_t kPart = 700;
    const size_t parts = (text.size() + kPart - 1) / kPart;
    for (size_t i = 0; i < parts; i++)
        os_log(rawLog(), "raw: harvest %016llx %zu/%zu %{public}s", (unsigned long long)name, i + 1, parts,
               text.substr(i * kPart, kPart).c_str());
}
#endif

Completion::Completion(Device &device, const char *cacheDir, uint64_t stack, const char *seedDir)
    : device_(device), stack_(stack),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.complete",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0))),
      persistenceQueue_(
          dispatch_queue_create("com.brunostjohn.switch.raw.kernel-cache",
                                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0))),
      pending_(dispatch_group_create())
{
    const bool writable = cacheDir && *cacheDir && cache_.attach(cacheDir, stack_, kCacheCap);
    if (seedDir && *seedDir)
        cache_.attachSeed(seedDir, stack_);
    compiler_ = std::make_unique<Compiler>(&cache_, [](const CacheKey &, RuntimeRecord &, std::string &why) {
        why = "a client makes no state";
        return Compiler::Outcome::Failed;
    });
    const raw::Kernel &kernel = device_.kernel();
    remote_ = std::make_unique<Remote>(
        KernelTransport{kernel.connection(), kernel.tunnelled(), kernel.tunnelSelector()}, stack_);
    Remote *remote = remote_.get();
    const RecordCache *cache = &cache_;
    device_.runSlotsFrom([remote, cache](const uint8_t *slot, uint32_t size, const uint8_t *&mapping, uint64_t &bytes) {
        const uint8_t *found = remote->findSlot(slot, size, mapping, bytes);
        return found ? found : cache->findSeedSlot(slot, size, mapping, bytes);
    });
    if (!remote_->serving())
        os_log_error(rawLog(),
                     "raw: %{public}s has no shader compiler this boot: a state its pack, seed and store lack fails "
                     "its creation",
                     getprogname());
    // the directory taken at once, for the lookups at state creation; its scan (another build's entries removed, the bytes
    // kept counted) on the queue, before any store there
    if (writable)
        dispatch_async(persistenceQueue_, ^{
            cache_.tidy();
        });
    else if (cacheDir && *cacheDir)
        NSLog(@"TegraGM20BMetal: raw records kept in memory only: %s takes no entry", cacheDir);
}

Completion::~Completion()
{
    device_.runSlotsFrom(nullptr);
    stopping_.store(true, std::memory_order_release);
    compiler_->drain();
    dispatch_group_wait(pending_, DISPATCH_TIME_FOREVER);
    dispatch_sync(persistenceQueue_, ^{
                  });
    compiler_.reset();
}

// The state's key: the build, both functions' AIR (their entries' HASH tags in the metallibs m2v made them from) and the pack
// key, which a state of no framework's library gets both AIR hashes appended to, and a state linking functions each linked
// one's. False: a function the raw path cannot name the AIR of (made from no metallib's bytes, or not one of them)
bool Completion::keyOf(Ask &a, CacheKey &k)
{
    if (!renderCacheKey(a.facade ?: a.descriptor, a.key, a.own, stack_, k, a.facade ? rmFunctionSource : nullptr))
        return false;
    a.key = k.key;
    return true;
}

struct Completion::Pending {
    Pending(dispatch_group_t group, Ready ready, bool creation)
        : group(group), ready(std::move(ready)), creation(creation)
    {
        dispatch_group_enter(group);
    }

    ~Pending()
    {
        dispatch_group_leave(group);
    }

    dispatch_group_t group;
    Ready ready;
    // a state's creation, which a failure ends (Metal's error) where a variant's ask tries again
    const bool creation;
};

void Completion::ask(Ask asked, std::function<void(Pipeline *)> ready)
{
    asked.qos = uint32_t(qos_class_self());
    request(std::move(asked), std::make_shared<Pending>(
                                  pending_,
                                  [ready = std::move(ready)](Pipeline *made, const std::string &) {
                                      ready(made);
                                  },
                                  false));
}

void Completion::askVariant(Ask asked, Ready ready)
{
    asked.qos = uint32_t(qos_class_self());
    request(std::move(asked), std::make_shared<Pending>(pending_, std::move(ready), false));
}

void Completion::create(Ask asked, Ready ready)
{
    asked.qos = uint32_t(qos_class_self());
    request(std::move(asked), std::make_shared<Pending>(pending_, std::move(ready), true));
}

// Why an ask of `k` that came to nothing failed, "" where it was refused
std::string Completion::failureOf(const CacheKey &k)
{
    if (remote_->declined())
        return kNoCompilerComes;
    RuntimeRecord record;
    std::string why;
    return compiler_->lookup(k, record, why) == RecordCache::Found::Refusal ? std::string()
                                                                            : "the state's compile could not run";
}

void Completion::request(Ask asked, std::shared_ptr<Pending> pending)
{
    dispatch_async(queue_, ^{
        if (stopping_.load(std::memory_order_acquire)) {
            pending->ready(nullptr, std::string());
            return;
        }
        Ask a = asked;
        CacheKey k;
        if (!keyOf(a, k)) {
            pending->ready(nullptr, std::string());
            return;
        }
        const bool caTransform = a.caTransform;
        // the installed seed's, this process's or the disk's answer, kept in the process table
        RuntimeRecord known;
        std::string why;
        if (compiler_->lookup(k, known, why) != RecordCache::Found::None) {
            const RuntimeRecord *kept = nullptr;
            if (compiler_->ask(k, nullptr, &kept) == Compiler::State::Ready && kept)
                place(k, kept, caTransform, pending);
            else
                pending->ready(nullptr, std::string());
            return;
        }
        if (!remote_->serving()) {
            pending->ready(nullptr, kNoCompilerComes);
            return;
        }
        remoteLater(a, k, [this, k, caTransform, pending](const RuntimeRecord *record) {
            if (record)
                place(k, record, caTransform, pending);
            else
                pending->ready(nullptr, failureOf(k));
        });
    });
}

void Completion::place(const CacheKey &key, const RuntimeRecord *record, bool caTransform,
                       std::shared_ptr<Pending> pending)
{
    const CacheKey keptKey = key;
    dispatch_async(queue_, ^{
        if (stopping_.load(std::memory_order_acquire)) {
            pending->ready(nullptr, std::string());
            return;
        }
        std::string why;
        bool full = false;
        Pipeline *made = device_.addRecord(*record, caTransform, why, &full);
        if (!made && full)
            os_log_error(rawLog(), "raw: %{public}s: the state that needs %016llx fails", why.c_str(),
                         (unsigned long long)keptKey.name());
        pending->ready(made, full ? why : std::string());
    });
}

Pipeline *Completion::makeNow(Ask a, std::string &failure)
{
    failure.clear();
    CacheKey k;
    if (!keyOf(a, k))
        return nullptr;
    RuntimeRecord record;
    std::string why;
    Compiler::State state = Compiler::State::Refused;
    RAW_DIAGNOSTIC(noteObtained({"table", 0, uint32_t(qos_class_self()), 0}));
    switch (compiler_->lookup(k, record, why)) {
    case RecordCache::Found::Record:
        state = Compiler::State::Ready;
        break;
    case RecordCache::Found::Refusal:
        state = Compiler::State::Refused;
        break;
    case RecordCache::Found::None:
        if (!remote_->serving()) {
            failure = kNoCompilerComes;
            return nullptr;
        }
        if (!remoteNow(a, k, state, record, failure))
            return nullptr;
        break;
    }
    if (state == Compiler::State::Failed) {
        failure = "the state's compile could not run" + (why.empty() ? std::string() : ": " + why);
        return nullptr;
    }
    if (state != Compiler::State::Ready)
        return nullptr;
    bool heapFull = false;
    Pipeline *made = device_.addRecord(record, a.caTransform, why, &heapFull);
    if (!made && heapFull)
        failure = why;
    return made;
}

void Completion::backgroundKernel(std::function<void()> work)
{
    compiler_->enqueue([work = std::move(work)] {
        @autoreleasepool {
            work();
        }
    });
}

const ComputeState *Completion::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                                       const std::string &key, bool *retry, id facadeFunction, bool *gone)
{
    CacheKey k;
    k.stack = stack_;
    k.key = "kernel | " + key;
    std::shared_ptr<Kernel> job;
    {
        std::lock_guard<raw::Mutex> mapLock(kernelLock_);
        auto &entry = kernels_[k.name()];
        if (!entry)
            entry = std::make_shared<Kernel>();
        job = entry;
    }
    std::lock_guard<raw::Mutex> hold(job->lock);
    using Build = ComputeKernelBuild<ComputeRecord, ComputeState>;
    std::string why;
    bool attempted = false;
    const auto result = job->build.make(
        [&](ComputeRecord &record) {
            attempted = true;
            Compiler::Outcome outcome = Compiler::Outcome::Failed;
            switch (cache_.loadKernel(k, record, why)) {
            case RecordCache::Found::Record:
                outcome = Compiler::Outcome::Made;
                break;
            case RecordCache::Found::Refusal:
                outcome = Compiler::Outcome::Refused;
                break;
            case RecordCache::Found::None: {
                bool taken = false;
                if (!remote_->serving())
                    return Build::Outcome::Gone;
                outcome = remoteKernel(facadeFunction, descriptor, dispatch, k, record, why, taken);
                if (!taken)
                    return Build::Outcome::Gone;
                break;
            }
            }
            return outcome == Compiler::Outcome::Made      ? Build::Outcome::Made
                   : outcome == Compiler::Outcome::Refused ? Build::Outcome::Refused
                                                           : Build::Outcome::Failed;
        },
        [&](const ComputeRecord &record) {
            attempted = true;
            const auto *state = placeCompute(device_, record);
            if (!state)
                why = "no room for its program or samplers";
            return state;
        });
    if (retry)
        *retry = result.retry;
    if (gone)
        *gone = result.gone;
    if (!result.state && attempted)
        once([NSString stringWithFormat:@"a kernel the pack lacks %s at %u,%u,%u threads: %s | %s %s",
                                        result.retry ? "was not made" : "is refused", dispatch.threads[0],
                                        dispatch.threads[1], dispatch.threads[2], why.c_str(),
                                        descriptor.computeFunction.name.UTF8String ?: "-", constantsLast(key).c_str()]);
    return result.state;
}

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
