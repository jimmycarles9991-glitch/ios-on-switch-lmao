// SPDX-License-Identifier: MIT
// The raw device's completion of the states and kernels its pack lacks (RawMetal.mm), of any library's functions: each asked
// once per process and per build. A state is looked up in the record cache under the process's shader cache directory when it is
// created (made()), else made directly (RawDirect.h: m2v's translation and NVK's stage compile, no pipeline or capture), its
// record kept, and handed to the raw device; for a state it refuses, m2v draws it. A kernel the same, at the first dispatch of
// its key (raw_compute_key.h: its threadgroup shape and bindings are the dispatch's).
// Its fragment program's split is made after state creation, offered to the state and kept with its record.
#pragma once
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include "raw_compiler.h"
#include "raw_cubic.h"
#include "raw_metallib.h"

struct NmbDirectDispatch;

namespace raw {

class Device;
class Direct;
struct ComputeRecord;
struct ComputeState;
struct Pipeline;

class Completion {
public:
    // `cacheDir`: the process's own shader cache directory (null or empty: records kept in memory only)
    Completion(Device &device, const char *cacheDir, uint64_t stack);
    ~Completion();

    struct Ask {
        MTLRenderPipelineDescriptor *descriptor;  // over m2v's functions, whose metallibs name their AIR
        std::string key;                          // the pack key
        bool caTransform = false;                 // Pipeline::caTransform
        // its functions are not a framework's default library's: no pack names the key, so the record is kept under the
        // key and both functions' AIR (two apps' alike-named functions are not one state)
        bool own = false;
        // its cubic variant's binds (raw_cubic.h), whose name the key ends with; empty: the state itself
        CubicBinds cubic;
    };

    // The state's pipeline handed to `ready` once made (on the compile queue; at once if it was before), or null once the raw
    // path will not draw it (refused, its compile failed past its retries, or no room for it)
    void ask(Ask a, std::function<void(Pipeline *)> ready);
    // The state's pipeline now, on the caller's thread: its record from this process or the disk (a state made in an earlier
    // boot is raw from its first draw), else made, as Metal makes a state at its creation. Null: refused, or its compile could
    // not run (*retry: ask() may make it later)
    Pipeline *makeNow(Ask a, bool *retry);

    // The kernel of compute key `key` (raw_compute_key.h) for `descriptor` (over m2v's functions) at `dispatch`, placed:
    // this process's, the disk's, or made now on the caller's thread, the dispatch that first needs it. Null: refused (named
    // once in the raw log), its compile failed past its retries, or no room for it
    const ComputeState *kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                               const std::string &key);

    Compiler::Stats stats() const;

private:
    struct Kernel {
        std::unique_ptr<ComputeRecord> record;  // placeCompute names its state by the record's address: kept for good
        const ComputeState *state = nullptr;
        bool refused = false;
        uint32_t failures = 0;
    };

    Device &device_;
    RecordCache cache_;
    std::unique_ptr<Compiler> compiler_;
    std::unique_ptr<Direct> direct_;
    std::once_flag directMade_;  // make() runs on the compile queue and on creating threads at once
    uint64_t stack_;
    dispatch_queue_t queue_, splitQueue_;
    std::mutex lock_;
    std::map<uint64_t, Ask> asked_;  // by CacheKey::name, what make() makes
    std::mutex kernelLock_;          // one kernel made at a time: a second dispatch of it waits for the first's
    std::map<uint64_t, Kernel> kernels_;
    std::set<uint64_t> splitAsked_;  // by CacheKey::name, the states whose split splitLater made or is making

    bool keyOf(Ask &a, CacheKey &k);
    Direct &direct();
    Compiler::Outcome make(const CacheKey &k, RuntimeRecord &record, std::string &why);
    void splitLater(const CacheKey &k, const RuntimeRecord &record);
};

}  // namespace raw
