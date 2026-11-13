// SPDX-License-Identifier: MIT
// State segments: a state object's method stream written once, at the object's creation, into GPU memory that is never
// rewritten, and run by a GPFIFO entry of its own wherever the state is bound. A bind costs the CPU two push records (the
// segment's and the pass body's continuation after it) instead of a copy of the stream into the push ring.
//
// The heap is append-only and places a stream once: a segment's address names its words, and only those words, for the
// device's lifetime, so a channel's record of the segment it last ran (HeldSegment) names exactly the registers it holds.
// Its chunks are mapped GPU-volatile, as the push ring is: the GPU may have fetched a chunk's earlier segments (and with
// them the L2 lines of later ones) before the CPU appends.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>
#include "raw_batch.h"

namespace raw {

struct StateSegment {
    uint64_t va = 0;
    uint32_t words = 0;
    // none of its methods is one a pass's epilogue or the next pass's prologue writes (raw_pass_writes.h): the channel
    // still holds it at the next pass's first draw
    bool keptAcrossPasses = false;

    explicit operator bool() const
    {
        return words != 0;
    }
};

// What a segment bind adds to a job: two push records of 16 bytes. A stream no longer than this goes inline.
constexpr uint32_t kSegmentBindWords = 8;

// The segment binds a command buffer takes before it binds inline. Each adds two GPFIFO entries: past
// TegraGM20BMaxJobPushes a command buffer becomes several jobs, each with its own fence.
constexpr uint32_t kSegmentBindsPerCommandBuffer = 128;

struct SegmentChunk {
    uint8_t *cpu = nullptr;
    uint64_t va = 0, bytes = 0;
};

class StateSegmentHeap {
public:
    // Allocates GPU-volatile control memory of at least `bytes` and makes it a member of the device's resource set
    using Grow = std::function<bool(uint64_t bytes, SegmentChunk &chunk)>;
    static constexpr uint64_t kChunkBytes = 0x10000;

    explicit StateSegmentHeap(Grow grow = {}) : grow_(std::move(grow))
    {}

    void setGrow(Grow grow)
    {
        grow_ = std::move(grow);
    }

    // The caller serialises place() (Device::lock_). A stream placed before is that segment again, so the heap grows with
    // the distinct states a process makes, not with the objects it makes of them. An empty segment: the stream is short
    // enough to stay inline, or no memory was left for it (it stays inline).
    StateSegment place(const uint32_t *words, uint32_t count);

    uint64_t bytesPlaced() const
    {
        return placed_;
    }

private:
    Grow grow_;
    std::vector<SegmentChunk> chunks_;
    std::map<std::vector<uint32_t>, StateSegment> placedStreams_;
    uint64_t used_ = 0, placed_ = 0;
};

// Whether a method stream sets none of kPassWrites' methods
bool keptAcrossPasses(const uint32_t *words, uint32_t count);

// A render pass's body in order: stretches the CPU wrote into the push ring, and pushes already resident in GPU memory
// (state segments, prefetched; indirect arguments, never prefetched, which follow an incomplete MME call)
struct PushBody {
    uint32_t *first = nullptr, *second = nullptr;
    uint64_t va = 0;
    uint32_t words = 0;
    bool incomplete = false;
    bool batchEnd = false;
    uint32_t flags = 0;  // a resident push's TegraGM20BPush* flags

    static PushBody resident(uint64_t va, uint32_t words, uint32_t flags)
    {
        PushBody b;
        b.va = va;
        b.words = words;
        b.flags = flags;
        return b;
    }
};

// The open stretch [start, at) closed, the segment after it, and the stretch going on from `at`
void bindSegment(std::vector<PushBody> &bodies, uint32_t *&start, uint32_t *at, const StateSegment &segment);

// A colour matrix macro call (raw_gpu.cpp matrixConstants): the stretch [start, at) that ends in its header, then the
// uniform's `words` at `uniformVa` as the GPU reads them, and where that uniform holds the rows alone, `clampWords` of 0 at
// `clampVa`. The call's data runs on across the pushes: each but the last is incomplete, and the kernel takes an incomplete
// push only before a non-prefetched one.
void matrixCall(std::vector<PushBody> &bodies, uint32_t *start, uint32_t *at, uint64_t uniformVa, uint32_t words,
                uint64_t clampVa, uint32_t clampWords);

// The bodies as the job's pushes, ring stretches at their GPU address (`ringCpu` maps `ringVa`). Returns the ring words.
uint64_t appendBodies(PushBatches &pushes, const std::vector<PushBody> &bodies, const uint8_t *ringCpu,
                      uint64_t ringVa);

// The segment a channel's 3D registers hold from its last bind; binding it again writes nothing. Lost wherever something
// other than a segment bind may have written one of its methods: a channel made again, a state's extras set back, an
// encoder of another engine, a pass boundary for a segment not kept across passes.
class HeldSegment {
public:
    bool holds(const StateSegment &s) const
    {
        return s && held_.va == s.va;
    }

    void bound(const StateSegment &s)
    {
        held_ = s;
    }

    void lose()
    {
        held_ = {};
    }

    void passBoundary()
    {
        if (!held_.keptAcrossPasses)
            lose();
    }

private:
    StateSegment held_;
};

}  // namespace raw
