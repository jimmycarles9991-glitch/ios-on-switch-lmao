// SPDX-License-Identifier: MIT
// Development builds, the render server only: which state a long stretch of the compositor's GPU time drew. After each draw,
// and after each pass's prologue (its loads and clears), the push writes a four-word 3D report: the payload and PTIMER once
// every write before it has left the end of the pipeline. Unlike the stamps' host releases (raw_stamps.h) it waits for no
// idle, so the pipeline keeps running behind it. A span is the time from one mark to the next (the first from the command
// buffer's start stamp, the last to its end stamp); consecutive spans of one state add up. Each span of kLongSpanNs or more
// is named on completion (Device::reportLongSpans): key, static instructions, Core Animation's state name.
#pragma once
#include "raw_lock.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"
#include "raw_stamps.h"

namespace raw {

enum : uint32_t {
    REPORT_SEMAPHORE_A = NVB197_SET_REPORT_SEMAPHORE_A,
    REPORT_RELEASE_AFTER_ALL = NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, RELEASE, AFTER_ALL_PRECEEDING_WRITES_COMPLETE) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS),
};

constexpr uint32_t kSpanMarkWords = 5;
constexpr uint32_t kSpanMarksPerBuffer = 256;
constexpr uint32_t kSpanSlots = 32;
constexpr uint64_t kLongSpanNs = 5'000'000;

using SpanMark = StampRelease;

enum class SpanKind : uint8_t {
    Draws,
    Prologue,
    Tail
};

struct SpanLabel {
    uint64_t key = 0;
    uint32_t instructions = 0;
    uint16_t kpx = 0;  // the pass's colour target 0 in thousands of samples
    uint8_t samples = 1;
    SpanKind kind = SpanKind::Draws;
    char name[40] = {};

    bool sameState(const SpanLabel &o) const
    {
        return kind == o.kind && key == o.key && kpx == o.kpx && !strncmp(name, o.name, sizeof name);
    }
};

struct StateSpan {
    SpanLabel label;
    uint32_t marks = 0;
    uint64_t ns = 0;
};

inline uint64_t spanSlotOffset(uint32_t stamp)
{
    return uint64_t((stamp - 1) % kSpanSlots) * kSpanMarksPerBuffer * sizeof(SpanMark);
}

inline void emitSpanMark(Push &p, uint64_t va, uint32_t stamp)
{
    p.inc(REPORT_SEMAPHORE_A, {uint32_t(va >> 32) & 0xff, uint32_t(va), stamp, REPORT_RELEASE_AFTER_ALL});
}

// The spans of one command buffer from its start stamp, its marks in order and its end stamp, consecutive spans of one state
// added up. A mark the GPU did not write (a pass refused after its prologue's mark was taken) or that runs backwards is
// passed over: its time goes to the span after it.
inline std::vector<StateSpan> spansOf(const volatile SpanMark *marks, const SpanLabel *labels, uint32_t count,
                                      uint32_t stamp, uint64_t startPtimer, uint64_t endPtimer)
{
    std::vector<StateSpan> out;
    uint64_t prev = startPtimer;
    const auto add = [&](const SpanLabel &label, uint64_t at) {
        const uint64_t ns = ptimerNanoseconds(at - prev);
        prev = at;
        if (!out.empty() && out.back().label.sameState(label)) {
            out.back().ns += ns;
            out.back().marks++;
        } else {
            out.push_back({label, 1, ns});
        }
    };
    for (uint32_t i = 0; i < std::min(count, kSpanMarksPerBuffer); i++) {
        const uint64_t at = marks[i].ptimer;
        if (marks[i].payload == stamp && at >= prev)
            add(labels[i], at);
    }
    SpanLabel tail;
    tail.kind = SpanKind::Tail;
    if (endPtimer >= prev)
        add(tail, endPtimer);
    return out;
}

// The render server's marks: begin() takes a command buffer's slot (by its stamp), each mark a label in it and a report into
// the marks memory, take() reads a completed one's while its stamp still holds the slot. Encoding is one command buffer at a
// time (the device's lock); completion reads on another thread, so a slot changes hands under lock_.
class SpanMarks {
public:
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr uint64_t kBytes = uint64_t(kSpanSlots) * kSpanMarksPerBuffer * sizeof(SpanMark);

    bool on() const
    {
        return slots_ != nullptr;
    }

    void open(uint64_t va, uint8_t *cpu)
    {
        memset(cpu, 0, kBytes);
        va_ = va;
        cpu_ = cpu;
        slots_.reset(new std::array<Slot, kSpanSlots>);
    }

    void begin(uint32_t stamp)
    {
        if (!slots_)
            return;
        std::lock_guard<raw::Mutex> l(lock_);
        at_ = &(*slots_)[(stamp - 1) % kSpanSlots];
        at_->stamp = stamp;
        at_->marks = 0;
    }

    // A mark whose report comes later (a pass's prologue, which goes ahead of the draws marked meanwhile); kNone when full
    uint32_t reserve()
    {
        if (!at_ || at_->marks >= kSpanMarksPerBuffer)
            return kNone;
        at_->labels[at_->marks] = SpanLabel{};
        return at_->marks++;
    }

    // kSpanMarkWords of room
    void markAt(Push &p, uint32_t index, const SpanLabel &label)
    {
        if (!at_ || index >= at_->marks)
            return;
        at_->labels[index] = label;
        emitSpanMark(p, va_ + spanSlotOffset(at_->stamp) + index * sizeof(SpanMark), at_->stamp);
    }

    void mark(Push &p, const SpanLabel &label)
    {
        markAt(p, reserve(), label);
    }

    std::vector<StateSpan> take(uint32_t stamp, uint64_t startPtimer, uint64_t endPtimer)
    {
        if (!slots_ || !stamp)
            return {};
        std::lock_guard<raw::Mutex> l(lock_);
        const Slot &s = (*slots_)[(stamp - 1) % kSpanSlots];
        if (s.stamp != stamp)
            return {};
        const auto *marks = reinterpret_cast<const volatile SpanMark *>(cpu_ + spanSlotOffset(stamp));
        return spansOf(marks, s.labels.data(), s.marks, stamp, startPtimer, endPtimer);
    }

private:
    struct Slot {
        uint32_t stamp = 0, marks = 0;
        std::array<SpanLabel, kSpanMarksPerBuffer> labels;
    };

    std::unique_ptr<std::array<Slot, kSpanSlots>> slots_;
    Slot *at_ = nullptr;
    raw::Mutex lock_;
    uint64_t va_ = 0;
    uint8_t *cpu_ = nullptr;
};

// A draw's label: its state's key, its fragment program's static instructions and Core Animation's name for it
template <class Pipeline> SpanLabel drawSpanLabel(const Pipeline &pl, uint64_t key, uint32_t kpx, uint8_t samples)
{
    SpanLabel l;
    l.key = key;
    l.instructions = pl.fragmentInstructions;
    l.kpx = uint16_t(std::min<uint32_t>(kpx, 0xffff));
    l.samples = samples;
    if (const char *name = pl.caName ? pl.caName : pl.spanName)
        strlcpy(l.name, name, sizeof l.name);
    return l;
}

inline const char *spanKindName(SpanKind kind)
{
    return kind == SpanKind::Prologue ? "pass prologue" : kind == SpanKind::Tail ? "after the last draw" : "draws";
}

// "<us> us <name> k<key> <instructions> instr x<marks> <kpx>k[m]": one span for the log and the kernel's note
inline int formatSpan(char *out, size_t size, const StateSpan &s)
{
    const SpanLabel &l = s.label;
    return snprintf(out, size, "%llu us %s k%016llx %u instr x%u %uk%s", (unsigned long long)(s.ns / 1000),
                    l.name[0] ? l.name : spanKindName(l.kind), (unsigned long long)l.key, l.instructions, s.marks,
                    l.kpx, l.samples > 1 ? "m" : "");
}

}  // namespace raw
