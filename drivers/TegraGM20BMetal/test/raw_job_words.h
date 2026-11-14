// SPDX-License-Identifier: MIT
// The jobs of command buffers as text that does not depend on where their setBytes landed. A command buffer takes its
// setBytes chunks from a pool a utility queue grows ahead and completions refill after waking waitUntilCompleted
// (RawMetalCommandBuffer.mm), so which chunk, and whether one coalesced chunk or two, varies from run to run; every
// chunk alive is in the device's resource set, so in every job's uses. Each address in a chunk reads as its offset in
// the command buffer's chunks laid end to end, and any other address in the driver's coherent control memory (its upload
// and push rings, whose place moves with completions) as "ctl"; a job's uses leave out that memory and the BOs made since
// the test's mark (spares and backings made or not as the threads meet).
#pragma once
#include "raw_kernel_model.h"
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

@interface NSObject (SwitchRawJobWords)
- (NSArray<NSNumber *> *)switchRawBytesChunks;        // RawMetalCommandBuffer.mm (debug builds)
- (NSArray<NSNumber *> *)switchRawBytesChunkLengths;  // the same chunks' lengths
@end

namespace job_words {

struct Chunk {
    uint64_t va = 0, bytes = 0, at = 0;  // at: its first byte's offset in the run
};

// A command buffer's jobs, from `firstJob` to the next run's, and its setBytes chunks in the order it filled them
struct Run {
    size_t firstJob = 0;
    std::vector<Chunk> chunks;
};

// A test's own resource, which a pool suballocates where the threads leave room: its addresses read by name
struct Named {
    const char *name;
    uint64_t va = 0, bytes = 0;
};

// The model's next BO handle: taken before the first command buffer is recorded
inline uint64_t handleMark()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.nextHandle;
}

// `cb` recorded and not yet committed: its jobs are the model's next ones
inline Run run(id<MTLCommandBuffer> cb)
{
    Run r;
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        r.firstJob = kernel_model::model.jobs.size();
    }
    NSArray<NSNumber *> *vas = [(id)cb switchRawBytesChunks], *lengths = [(id)cb switchRawBytesChunkLengths];
    uint64_t at = 0;
    for (NSUInteger i = 0; i < vas.count && i < lengths.count; i++) {
        r.chunks.push_back({vas[i].unsignedLongLongValue, lengths[i].unsignedLongLongValue, at});
        at += lengths[i].unsignedLongLongValue;
    }
    return r;
}

// Whether `va` lies in a coherent BO the model bound (the driver's control memory)
inline bool controlAt(uint64_t va)
{
    const auto &binds = kernel_model::model.binds;
    auto bound = binds.upper_bound(va);
    if (!va || bound == binds.begin())
        return false;
    --bound;
    if (va - bound->first >= bound->second.second)
        return false;
    const auto bo = kernel_model::model.bos.find(bound->second.first);
    return bo != kernel_model::model.bos.end() && !bo->second.import && !bo->second.cached;
}

inline const Chunk *chunkAt(const Run &r, uint64_t va)
{
    for (const Chunk &c : r.chunks)
        if (va >= c.va && va - c.va < c.bytes)
            return &c;
    return nullptr;
}

// The model's jobs of `runs` (in order, the last one's to the model's last job): one line a job, its words in hex with each
// (high, low) address pair in a chunk as "bytes+<offset in the run>" and each (low, high) one, an address as data words
// hold it, as "bytes.le+<offset>", one in a `named` resource as "<name>+<offset>", then its uses of BOs neither coherent
// nor made since `mark` (handleMark)
inline std::string words(const std::vector<Run> &runs, uint64_t mark, const std::vector<Named> &named = {})
{
    const auto nameOf = [&](uint64_t va) -> const Named * {
        for (const Named &n : named)
            if (va >= n.va && va - n.va < n.bytes)
                return &n;
        return nullptr;
    };
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    const auto &jobs = kernel_model::model.jobs;
    std::string out;
    char text[48];
    for (size_t r = 0; r < runs.size(); r++) {
        const Run &run = runs[r];
        const size_t end = r + 1 < runs.size() ? runs[r + 1].firstJob : jobs.size();
        for (size_t j = run.firstJob; j < end && j < jobs.size(); j++) {
            const auto &w = jobs[j].words;
            snprintf(text, sizeof text, "cb %zu job %zu %zu:", r, j - run.firstJob, w.size());
            out += text;
            for (size_t i = 0; i < w.size(); i++) {
                const uint64_t high = i + 1 < w.size() ? uint64_t(w[i]) << 32 | w[i + 1] : 0,
                               low = i + 1 < w.size() ? uint64_t(w[i + 1]) << 32 | w[i] : 0;
                const Chunk *c = w[i] ? chunkAt(run, high) : nullptr;
                const Chunk *data = !c && w[i + 1 < w.size() ? i + 1 : i] ? chunkAt(run, low) : nullptr;
                const Named *n = c || data || !w[i] ? nullptr : nameOf(high);
                const Named *nl = c || data || n || !w[i + 1 < w.size() ? i + 1 : i] ? nullptr : nameOf(low);
                if (n || nl) {
                    snprintf(text, sizeof text, " %s%s+%llx", n ? n->name : nl->name, n ? "" : ".le",
                             (unsigned long long)(n ? high - n->va : low - nl->va));
                    out += text;
                    i++;
                    continue;
                }
                if (c || data) {
                    snprintf(text, sizeof text, c ? " bytes+%llx" : " bytes.le+%llx",
                             (unsigned long long)(c ? c->at + high - c->va : data->at + low - data->va));
                    out += text;
                    i++;
                    continue;
                }
                if ((w[i] && controlAt(high)) || (w[i + 1 < w.size() ? i + 1 : i] && controlAt(low))) {
                    out += " ctl";
                    i++;
                    continue;
                }
                snprintf(text, sizeof text, " %08x", w[i]);
                out += text;
            }
            out += " | uses";
            for (const TegraGM20BAbiSurfaceUse &u : jobs[j].uses) {
                const auto bo = kernel_model::model.bos.find(u.handle);
                if (u.handle >= mark ||
                    (bo != kernel_model::model.bos.end() && !bo->second.import && !bo->second.cached))
                    continue;
                snprintf(text, sizeof text, " %llx/%x", (unsigned long long)u.handle, u.access);
                out += text;
            }
            out += "\n";
        }
    }
    return out;
}

inline uint64_t fnv(const std::string &text)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace job_words
