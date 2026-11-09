// SPDX-License-Identifier: MIT
// See RawComplete.h.
#import "RawComplete.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "RawDirect.h"
#include "RawMetalInternal.h"
#include "raw_gpu.h"

namespace raw {
namespace {

constexpr uint64_t kCacheCap = 8u << 20;

}  // namespace

Completion::Completion(Device &device, const char *cacheDir, uint64_t stack)
    : device_(device), stack_(stack),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.complete",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)))
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
                ready(device_.addRecord(*record, caTransform, why));
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
    return state == Compiler::State::Ready ? device_.addRecord(record, a.caTransform, why) : nullptr;
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
    std::call_once(directMade_, [this] {
        direct_ = std::make_unique<Direct>(device_.pack());
    });
    const Compiler::Outcome outcome = direct_->make(a.descriptor, a.key, record, why);
    if (outcome != Compiler::Outcome::Made)
        once([NSString stringWithFormat:@"a state the pack lacks %s: %s | %.600s",
                                        outcome == Compiler::Outcome::Refused ? "is refused" : "was not made",
                                        why.c_str(), a.key.c_str()]);
    return outcome;
}

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
