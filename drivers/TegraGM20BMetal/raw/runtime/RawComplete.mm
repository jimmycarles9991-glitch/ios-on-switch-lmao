// SPDX-License-Identifier: MIT
// See RawComplete.h.
#import "RawComplete.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "RawDirect.h"
#include "RawMetalInternal.h"
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

Completion::Completion(Device &device, const char *cacheDir, uint64_t stack)
    : device_(device), stack_(stack),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.complete",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0))),
      splitQueue_(
          dispatch_queue_create("com.brunostjohn.switch.raw.split",
                                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)))
{
    compiler_ = std::make_unique<Compiler>(&cache_, [this](const CacheKey &k, RuntimeRecord &record, std::string &why) {
        return make(k, record, why);
    });
    // the directory taken at once, for the lookups at state creation; its scan (another build's entries removed, the bytes
    // kept counted) on the queue, before any store there
    if (cacheDir && *cacheDir && cache_.attach(cacheDir, stack_, kCacheCap))
        dispatch_async(queue_, ^{
            cache_.tidy();
        });
    else if (cacheDir && *cacheDir)
        NSLog(@"TegraGM20BMetal: raw records kept in memory only: %s takes no entry", cacheDir);
}

Completion::~Completion()
{
    compiler_->drain();
    dispatch_sync(splitQueue_, ^{
                  });
    compiler_.reset();
}

// The state's key: the build, both functions' AIR (their entries' HASH tags in the metallibs m2v made them from) and the pack
// key, which a state of no framework's library gets both AIR hashes appended to, and a state linking functions each linked
// one's. False: a function the raw path cannot name the AIR of (made from no metallib's bytes, or not one of them)
bool Completion::keyOf(Ask &a, CacheKey &k)
{
    auto air = [](id<MTLFunction> function, uint8_t (&out)[32]) {
        const void *bytes = nullptr;
        size_t length = 0;
        std::map<std::string, AirHash> hashes;
        id library = native_bridge_metal_function_metallib(function, &bytes, &length);
        if (!library || !metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
            return false;
        const auto f = hashes.find(native_bridge_metal_function_library_name(function).UTF8String ?: "");
        if (f == hashes.end())
            return false;
        memcpy(out, f->second.data(), sizeof out);
        return true;
    };
    if (!air(a.descriptor.vertexFunction, k.vertexAir) || !air(a.descriptor.fragmentFunction, k.fragmentAir))
        return false;
    // Linked functions: IconRendering's effects each link their own function under one name (custom_fn), so the key takes
    // each one's AIR besides the names and constants the pack key gives them
    std::string linked;
    const std::pair<char, MTLLinkedFunctions *> stages[] = {{'v', a.descriptor.vertexLinkedFunctions},
                                                            {'f', a.descriptor.fragmentLinkedFunctions}};
    for (const auto &[stage, l] : stages)
        for (NSArray *functions : {l.functions, l.privateFunctions})
            for (id<MTLFunction> f in functions) {
                uint8_t hash[32];
                uint64_t h = 0;
                if (!air(f, hash))
                    return false;
                memcpy(&h, hash, sizeof h);
                char word[24];
                snprintf(word, sizeof word, " %c%016llx", stage, (unsigned long long)h);
                linked += word;
            }
    if (!linked.empty())
        a.key += " | linked" + linked;
    if (a.own) {
        char tail[48];
        uint64_t v, f;
        memcpy(&v, k.vertexAir, sizeof v);
        memcpy(&f, k.fragmentAir, sizeof f);
        snprintf(tail, sizeof tail, " | air %016llx %016llx", (unsigned long long)v, (unsigned long long)f);
        a.key += tail;
    }
    k.stack = stack_;
    k.key = a.key;
    return true;
}

void Completion::ask(Ask asked, std::function<void(Pipeline *)> ready)
{
    dispatch_async(queue_, ^{
        Ask a = asked;
        CacheKey k;
        if (!keyOf(a, k)) {
            ready(nullptr);
            return;
        }
        {
            std::lock_guard<std::mutex> l(lock_);
            asked_[k.name()] = a;
        }
        const bool caTransform = a.caTransform;
        auto done = [this, k, asked, ready, caTransform](const RuntimeRecord *record, const std::string &) {
            std::string why;
            if (record) {
                Pipeline *made = device_.addRecord(*record, caTransform, why);
                if (made)
                    splitLater(k, *record);
                ready(made);
                return;
            }
            // a compile that could not run (NVK or the frontend not up to it yet) is asked again, a bounded number of times
            uint64_t us = 0;
            if (compiler_->retryAfter(k, us))
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(us) * 1000), queue_, ^{
                    ask(asked, ready);
                });
            else
                ready(nullptr);
        };
        const RuntimeRecord *made = nullptr;
        const Compiler::State state = compiler_->ask(k, done, &made);
        if (state == Compiler::State::Ready && made)
            done(made, std::string());
        else if (state == Compiler::State::Refused || state == Compiler::State::Failed)
            done(nullptr, std::string());
    });
}

Pipeline *Completion::makeNow(Ask a, bool *retry)
{
    *retry = false;
    CacheKey k;
    if (!keyOf(a, k))
        return nullptr;
    {
        std::lock_guard<std::mutex> l(lock_);
        asked_[k.name()] = a;
    }
    RuntimeRecord record;
    std::string why;
    const Compiler::State state = compiler_->makeNow(k, record, why);
    *retry = state == Compiler::State::Failed || state == Compiler::State::Pending;
    Pipeline *made = state == Compiler::State::Ready ? device_.addRecord(record, a.caTransform, why) : nullptr;
    if (made)
        splitLater(k, record);
    return made;
}

Direct &Completion::direct()
{
    std::call_once(directMade_, [this] {
        direct_ = std::make_unique<Direct>(device_.pack());
    });
    return *direct_;
}

Compiler::Outcome Completion::make(const CacheKey &k, RuntimeRecord &record, std::string &why)
{
    Ask a;
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto it = asked_.find(k.name());
        if (it == asked_.end()) {
            why = "asked for no state";
            return Compiler::Outcome::Failed;
        }
        a = it->second;
    }
    ProbeTextures textures;
    textures.cubic = a.cubic;
    const Compiler::Outcome outcome = direct().make(a.descriptor, a.key, record, why, textures);
    if (outcome != Compiler::Outcome::Made)
        once([NSString stringWithFormat:@"a state the pack lacks %s: %s | %.600s",
                                        outcome == Compiler::Outcome::Refused ? "is refused" : "was not made",
                                        why.c_str(), a.key.c_str()]);
    return outcome;
}

const ComputeState *Completion::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                                       const std::string &key)
{
    CacheKey k;
    k.stack = stack_;
    k.key = "kernel | " + key;
    std::lock_guard<std::mutex> hold(kernelLock_);
    Kernel &made = kernels_[k.name()];
    if (made.state || made.refused || made.failures >= Compiler::kRetries)
        return made.state;
    auto record = std::make_unique<ComputeRecord>();
    std::string why;
    Compiler::Outcome outcome = Compiler::Outcome::Failed;
    switch (cache_.loadKernel(k, *record, why)) {
    case RecordCache::Found::Record:
        outcome = Compiler::Outcome::Made;
        break;
    case RecordCache::Found::Refusal:
        outcome = Compiler::Outcome::Refused;
        break;
    case RecordCache::Found::None: {
        const uint64_t began = nowUs();
        outcome = direct().kernel(descriptor, dispatch, key, *record, why);
        if (outcome != Compiler::Outcome::Failed)
            cache_.storeKernel(k, outcome == Compiler::Outcome::Made ? record.get() : nullptr, why);
        if (outcome == Compiler::Outcome::Made)
            once([NSString
                stringWithFormat:@"a kernel the pack lacks, made at its first dispatch of %u,%u,%u threads in "
                                 @"%llu us: %s %s",
                                 dispatch.threads[0], dispatch.threads[1], dispatch.threads[2],
                                 (unsigned long long)(nowUs() - began),
                                 descriptor.computeFunction.name.UTF8String ?: "-", constantsLast(key).c_str()]);
        break;
    }
    }
    if (outcome == Compiler::Outcome::Made && (made.state = placeCompute(device_, *record)))
        made.record = std::move(record);
    else if (outcome == Compiler::Outcome::Made)
        why = "no room for its program or samplers";
    made.refused = outcome == Compiler::Outcome::Refused;
    made.failures += !made.state && !made.refused;
    if (!made.state)
        once([NSString stringWithFormat:@"a kernel the pack lacks %s at %u,%u,%u threads: %s | %s %s",
                                        made.refused ? "is refused" : "was not made", dispatch.threads[0],
                                        dispatch.threads[1], dispatch.threads[2], why.c_str(),
                                        descriptor.computeFunction.name.UTF8String ?: "-", constantsLast(key).c_str()]);
    return made.state;
}

// The state made again with its fragment program's split, which only the same programs take (the record's layout is the
// whole program's). Kept on disk unless the compile could not run, so a later boot offers it at the state's creation.
void Completion::splitLater(const CacheKey &k, const RuntimeRecord &record)
{
    Ask a;
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto it = asked_.find(k.name());
        if (record.splitTried || it == asked_.end() || !splitAsked_.insert(k.name()).second)
            return;
        a = it->second;
    }
    auto kept = std::make_shared<RuntimeRecord>(record);
    const CacheKey key = k;  // a block holds a reference parameter as the reference
    dispatch_async(splitQueue_, ^{
        RuntimeRecord made;
        std::string why;
        ProbeTextures textures;
        textures.cubic = a.cubic;
        const Compiler::Outcome outcome = direct().make(a.descriptor, a.key, made, why, textures, nullptr, true);
        if (outcome == Compiler::Outcome::Failed)
            return;
        const bool same = outcome == Compiler::Outcome::Made && made.programs[0].id == kept->programs[0].id &&
                          made.programs[1].id == kept->programs[1].id;
        if (outcome == Compiler::Outcome::Made && !same)
            once([NSString
                stringWithFormat:@"a state's split compile made other programs than its own: %.600s", a.key.c_str()]);
        kept->splitTried = true;
        if (same && made.split) {
            kept->split = true;
            kept->splitRecord = made.splitRecord;
            kept->splitPrograms[0] = std::move(made.splitPrograms[0]);
            kept->splitPrograms[1] = std::move(made.splitPrograms[1]);
            device_.addSplit(*kept);
        }
        cache_.store(key, kept.get(), std::string());
    });
}

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
