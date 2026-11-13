// SPDX-License-Identifier: MIT
// The compute encoder's loop words for one command buffer laid down over predication_model.h's memory, as
// raw_compute.cpp lays them: launches named 1, 2, ... in encode order, each a QMD the hardware writes a word of.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_kit.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_loop_copies.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_loop_ranges.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_predication.h"
#include "drivers/TegraGM20BMetal/test/predication_model.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"

namespace loop_program {

using predication_model::Gpu;
using predication_model::kBase;
using raw::PredicateCompare;
using Answer = raw::Predication::Answer;

constexpr uint64_t kWords = kBase, kQmds = kBase + 0x2000, kRecord = kBase + 0x3000, kSlots = kBase + 0x4000;
constexpr uint64_t kPush = kBase + 0x10000;
constexpr uint32_t kStamp = 77, kQmdWords = 64, kHardwareWord = 48;

inline uint64_t word(uint32_t i)
{
    return kWords + 4 * i;
}

struct LoopProgram {
    Gpu gpu{1 << 20};
    raw::Predication predication;
    raw::LoopCopies loops;
    raw::PushBatches entries;
    raw::BatchWork batches;
    uint32_t *chunk = reinterpret_cast<uint32_t *>(gpu.at(kPush));
    raw::Push p{chunk, chunk + 4096};
    uint32_t *start = p.at;
    uint64_t nextSlot = kSlots;
    uint32_t launches = 0;
    bool qmdsIntact = true;

    raw::LoopStream stream()
    {
        return {p, start, entries, gpu.memory.data(), kBase};
    }

    raw::PredicateSlot slot()
    {
        const raw::PredicateSlot s{gpu.at(nextSlot), nextSlot};
        nextSlot += raw::Predication::kSlotBytes;
        return s;
    }

    // A launch of a QMD the hardware writes a word of, which each copy of a loop puts back
    uint32_t dispatch(uint64_t work = 1)
    {
        const uint32_t id = ++launches;
        const uint64_t qmd = kQmds + 0x100 * id;
        uint32_t words[kQmdWords] = {id};
        memcpy(gpu.at(qmd), words, sizeof words);
        if (loops.open())
            raw::kit(p, switch_gm20b_compute_inline_words, qmd, words, kQmdWords);
        predication.guard(p);
        p.m(NVB1C0_SEND_PCAS_A, id, predication_model::kCompute);
        p.m(NVB1C0_SEND_SIGNALING_PCAS_B, 0, predication_model::kCompute);
        predication.release(p);
        loops.addWork(work);
        return id;
    }

    // The encoder's room() running out: the open segment closes and the next one starts in another chunk
    void nextChunk()
    {
        loops.closeSegment(stream());
        chunk += 4096;
        p = raw::Push{chunk, chunk + 4096};
        loops.openSegment(stream());
    }

    void startIf(uint32_t w, PredicateCompare c, uint32_t reference)
    {
        ASSERT_EQ(predication.startIf(p, slot(), word(w), raw::predicateTest(c, reference)), Answer::Done);
    }

    void startElse()
    {
        ASSERT_EQ(predication.startElse(p, slot()), Answer::Done);
    }

    void endIf()
    {
        ASSERT_EQ(predication.endIf(), Answer::Done);
    }

    // As the compute encoder does, the unit's halt operands at its first loop
    void halting()
    {
        if (!predication.halting())
            predication.halt(slot());
    }

    Answer startWhile(uint32_t w, PredicateCompare c, uint32_t reference)
    {
        halting();
        const raw::PredicateTest test = raw::predicateTest(c, reference);
        return loops.start(stream(), predication, slot(), false, word(w), &test);
    }

    Answer startDoWhile()
    {
        halting();
        return loops.start(stream(), predication, slot(), true, 0, nullptr);
    }

    // `tag` names the loop in the record it leaves where it would run past its copies
    Answer endWhile(uint32_t tag = 0)
    {
        return loops.end(stream(), predication, batches, raw::BatchWork::kLimit, kRecord, kStamp, tag, 0, nullptr);
    }

    Answer endDoWhile(uint32_t w, PredicateCompare c, uint32_t reference, uint32_t tag = 0)
    {
        const raw::PredicateTest test = raw::predicateTest(c, reference);
        return loops.end(stream(), predication, batches, raw::BatchWork::kLimit, kRecord, kStamp, tag, word(w), &test);
    }

    // The command buffer's GP entries run; each launch's program does `effect`
    std::vector<uint32_t> run(bool seesHeads, const std::function<void(uint32_t, Gpu &)> &effect = {})
    {
        loops.closeSegment(stream());
        gpu.seesHeads = seesHeads;
        gpu.onLaunch = [&](uint32_t id) {
            const uint64_t qmd = kQmds + 0x100 * id;
            qmdsIntact &= gpu.word(qmd) == id && gpu.word(qmd + 4 * kHardwareWord) == 0;
            gpu.put(qmd + 4 * kHardwareWord, 0xdead);
            if (effect)
                effect(id, gpu);
        };
        gpu.runEntries(entries);
        return gpu.launched;
    }

    // A loop would run past its copies: the record names the command buffer and the loop (raw_loop_ranges.h)
    raw::LoopContinue record()
    {
        return raw::loopContinue(kStamp, gpu.word(kRecord), gpu.word(kRecord + 4));
    }

    bool overflowed()
    {
        return record().continues;
    }
};

inline std::vector<uint32_t> repeat(std::vector<uint32_t> head, uint32_t id, uint32_t times, std::vector<uint32_t> tail)
{
    head.insert(head.end(), times, id);
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

}  // namespace loop_program
