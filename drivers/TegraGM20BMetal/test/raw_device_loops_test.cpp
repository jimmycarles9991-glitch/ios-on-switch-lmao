// SPDX-License-Identifier: MIT
// raw::ComputeEncoder's predicated loops on raw::Device over the kernel model (raw_device_harness.h), the jobs it submits
// run by the predication model (predication_model.h) over the device's own memory: the copies its GP entries list, the
// QMD each copy launches, and the loop words the completion reads.
#include "raw_device_harness.h"
#include "predication_model.h"
#include "../raw/runtime/raw_compute.h"
#include <algorithm>
#include <functional>
#include <optional>

namespace {

using Answer = raw::Predication::Answer;
using raw::PredicateCompare;

// QMDV01_07 (clb1c0qmd.h): PROGRAM_OFFSET is bits 256-287; the HW_ONLY_* fields (:429-438, bits 1536-1663) are words the
// scheduler writes as it runs a launch
constexpr uint32_t kProgramWord = 8, kHwOnlyFirst = 48, kHwOnlyLast = 51;
constexpr uint32_t kTag = 41, kInnerTag = 42;

class DeviceLoops : public harness::DeviceTest {
protected:
    raw::ComputeRecord records[2];
    const raw::ComputeState *programs[2] = {};
    raw::Buffer words;
    predication_model::Gpu gpu{0};
    uint32_t qmdsBroken = 0;
    std::vector<int> launches;  // which program each launch ran: 0 or 1

    void SetUp() override
    {
        DeviceTest::SetUp();
        for (int i = 0; i < 2; ++i) {
            records[i].kernel.regionWord = -1;
            records[i].code.assign(64 * (i + 1), uint8_t(i + 1));
            records[i].keyString = i ? "loops b" : "loops a";
            programs[i] = raw::placeCompute(dev, records[i]);
            ASSERT_NE(programs[i], nullptr);
        }
        ASSERT_NE(programs[0]->program, programs[1]->program);
        ASSERT_TRUE(dev.createBuffer(4096, words));
        gpu.map = [](uint64_t va) {
            return kernel_model::model.host(va);
        };
    }

    uint32_t &word(uint32_t i)
    {
        return reinterpret_cast<uint32_t *>(words.cpu)[i];
    }

    uint64_t wordVa(uint32_t i) const
    {
        return words.va + 4 * i;
    }

    void dispatch(raw::ComputeEncoder &e, int program)
    {
        const uint32_t one[3] = {1, 1, 1};
        const char *why = nullptr;
        e.setState(programs[program]);
        ASSERT_TRUE(e.dispatch(one, one, &why)) << why;
    }

    // Runs every job the device submitted since `firstJob` in order, each segment read when its GP entry's turn comes.
    // Each launch checks its QMD is the CPU's (no HW_ONLY word an earlier launch of it left), writes those words as the
    // scheduler does, and runs `program`'s effect.
    void run(size_t firstJob, std::function<void(int)> program)
    {
        launches.clear();
        qmdsBroken = 0;
        gpu.onLaunch = [&, program](uint32_t pcas) {
            uint32_t *qmd = reinterpret_cast<uint32_t *>(gpu.at(uint64_t(pcas) << 8));
            for (uint32_t w = kHwOnlyFirst; w <= kHwOnlyLast; ++w) {
                qmdsBroken += qmd[w] != 0;
                qmd[w] = 0xa5a5a5a5u;
            }
            const int which = qmd[kProgramWord] == programs[1]->program ? 1 : 0;
            launches.push_back(which);
            program(which);
        };
        const auto &jobs = kernel_model::model.jobs;
        for (size_t j = firstJob; j < jobs.size(); ++j)
            for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
                std::vector<uint32_t> segment(push.dwords);
                memcpy(segment.data(), gpu.at(push.va), segment.size() * 4);
                gpu.run(segment.data(), segment.size(), true);
            }
    }

    size_t count(int program) const
    {
        return size_t(std::count(launches.begin(), launches.end(), program));
    }

    uint32_t commit(raw::CommandBuffer *cb)
    {
        const uint32_t stamp = cb->stamp;
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return stamp;
    }
};

// while (word 0) { 0 counts it down }; 1
TEST_F(DeviceLoops, AWhileRunsItsDispatchUntilItsWordFailsEachLaunchOnTheCpusQmd)
{
    for (uint32_t n : {0u, 1u, 5u, 300u}) {
        word(0) = n;
        const size_t first = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        {
            raw::ComputeEncoder e(*cb, dev);
            ASSERT_EQ(e.startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
            dispatch(e, 0);
            ASSERT_EQ(e.endWhile(kTag), Answer::Done);
            dispatch(e, 1);
            e.end();
        }
        EXPECT_TRUE(cb->loopsLaid);
        const uint32_t stamp = commit(cb);
        run(first, [&](int program) {
            if (program == 0)
                --word(0);
        });
        EXPECT_EQ(count(0), n);
        EXPECT_EQ(count(1), 1u);
        EXPECT_EQ(launches.back(), 1);
        EXPECT_EQ(word(0), 0u);
        EXPECT_EQ(qmdsBroken, 0u) << "a copy launched a QMD the scheduler had written";
        EXPECT_FALSE(dev.loopContinue(stamp).continues) << n;
    }
}

// Past its copies a loop no block holds leaves its stamp's continued word, from which the submission lays it again
TEST_F(DeviceLoops, AnOutermostLoopPastItsCopiesLeavesContinue)
{
    word(0) = raw::LoopCopies::kOutermostCopies + 7;
    const size_t first = kernel_model::model.jobs.size();
    raw::CommandBuffer *cb = dev.begin();
    {
        raw::ComputeEncoder e(*cb, dev);
        ASSERT_EQ(e.startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(e, 0);
        ASSERT_EQ(e.endWhile(kTag), Answer::Done);
        e.end();
    }
    const uint32_t stamp = commit(cb);
    run(first, [&](int) {
        --word(0);
    });
    EXPECT_EQ(count(0), raw::LoopCopies::kOutermostCopies);
    EXPECT_EQ(word(0), 7u);
    EXPECT_EQ(qmdsBroken, 0u);
    EXPECT_TRUE(dev.loopContinue(stamp).continues);
    EXPECT_EQ(dev.loopContinue(stamp).tag, kTag);
}

// while (word 0) { 0 clears it; while (word 1) { 1 counts it down }; 0 }: the inner loop past its copies records
// itself with its tag and stops the rest of the outer loop's copy, which the submission lays down again
TEST_F(DeviceLoops, AnInnerLoopPastItsCopiesRecordsItselfAndStopsTheRestOfItsOuterLoop)
{
    word(0) = 1;
    word(1) = raw::LoopCopies::kNestedCopies + 1;
    const size_t first = kernel_model::model.jobs.size();
    raw::CommandBuffer *cb = dev.begin();
    {
        raw::ComputeEncoder e(*cb, dev);
        ASSERT_EQ(e.startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(e, 0);
        ASSERT_EQ(e.startWhile(wordVa(1), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(e, 1);
        ASSERT_EQ(e.endWhile(kInnerTag), Answer::Done);
        dispatch(e, 0);
        ASSERT_EQ(e.endWhile(kTag), Answer::Done);
        e.end();
    }
    const uint32_t stamp = commit(cb);
    run(first, [&](int program) {
        if (program == 0)
            word(0) = 0;
        else
            --word(1);
    });
    EXPECT_EQ(count(0), 1u);
    EXPECT_EQ(count(1), raw::LoopCopies::kNestedCopies);
    EXPECT_EQ(word(1), 1u);
    EXPECT_EQ(qmdsBroken, 0u);
    EXPECT_TRUE(dev.loopContinue(stamp).continues);
    EXPECT_EQ(dev.loopContinue(stamp).tag, kInnerTag);
}

// if (word 0 == 1) { do { 1 counts word 1 down } while (word 1) }; 0
TEST_F(DeviceLoops, ADoWhileRunsInsideAnIf)
{
    for (bool taken : {true, false}) {
        word(0) = taken ? 1 : 0;
        word(1) = 3;
        const size_t first = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        {
            raw::ComputeEncoder e(*cb, dev);
            ASSERT_EQ(e.startIf(wordVa(0), PredicateCompare::Equal, 1), Answer::Done);
            ASSERT_EQ(e.startDoWhile(), Answer::Done);
            dispatch(e, 1);
            ASSERT_EQ(e.endDoWhile(wordVa(1), PredicateCompare::NotEqual, 0, kTag), Answer::Done);
            ASSERT_EQ(e.endIf(), Answer::Done);
            dispatch(e, 0);
            e.end();
        }
        const uint32_t stamp = commit(cb);
        run(first, [&](int program) {
            if (program == 1)
                --word(1);
        });
        EXPECT_EQ(count(1), taken ? 3u : 0u) << taken;
        EXPECT_EQ(count(0), 1u) << taken;
        EXPECT_EQ(word(1), taken ? 0u : 3u);
        EXPECT_EQ(qmdsBroken, 0u);
        EXPECT_FALSE(dev.loopContinue(stamp).continues);
    }
}

// while (word 0) { 0 counts it down and sets word 1 to 2; do { 1 counts word 1 down } while (word 1) }
TEST_F(DeviceLoops, ADoWhileRunsInsideAWhile)
{
    word(0) = 2;
    word(1) = 0;
    const size_t first = kernel_model::model.jobs.size();
    raw::CommandBuffer *cb = dev.begin();
    {
        raw::ComputeEncoder e(*cb, dev);
        ASSERT_EQ(e.startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(e, 0);
        ASSERT_EQ(e.startDoWhile(), Answer::Done);
        dispatch(e, 1);
        ASSERT_EQ(e.endDoWhile(wordVa(1), PredicateCompare::NotEqual, 0, kTag), Answer::Done);
        ASSERT_EQ(e.endWhile(kTag), Answer::Done);
        e.end();
    }
    const uint32_t stamp = commit(cb);
    run(first, [&](int program) {
        if (program == 0) {
            --word(0);
            word(1) = 2;
        } else {
            --word(1);
        }
    });
    EXPECT_EQ(launches, (std::vector<int>{0, 1, 1, 0, 1, 1}));
    EXPECT_EQ(word(0), 0u);
    EXPECT_EQ(qmdsBroken, 0u);
    EXPECT_FALSE(dev.loopContinue(stamp).continues);
}

// The copies end with the GPU's writes into the push ring, so a command buffer that laid loops names its member written
TEST_F(DeviceLoops, ACommandBufferThatLaidLoopsWritesThePushRing)
{
    word(0) = 0;
    const size_t first = kernel_model::model.jobs.size();
    raw::CommandBuffer *cb = dev.begin();
    {
        raw::ComputeEncoder e(*cb, dev);
        ASSERT_EQ(e.startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(e, 0);
        ASSERT_EQ(e.endWhile(kTag), Answer::Done);
        e.end();
    }
    commit(cb);
    ASSERT_GT(kernel_model::model.jobs.size(), first);
    const kernel_model::Job *job = nullptr;
    uint64_t body = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j)
        for (const TegraGM20BAbiPush &push : kernel_model::model.jobs[j].pushes)
            if (push.flags & TegraGM20BPushNoPrefetch) {
                body = push.va;
                job = &kernel_model::model.jobs[j];
            }
    ASSERT_NE(job, nullptr) << "no copy listed NO_PREFETCH in " << kernel_model::model.jobs.size() - first << " jobs";
    uint64_t ring = 0;
    for (const auto &[va, bound] : kernel_model::model.binds)
        if (body >= va && body - va < bound.second)
            ring = bound.first;
    ASSERT_NE(ring, 0u);
    std::string seen;
    bool written = false;
    for (const auto &use : job->uses) {
        if (use.handle == ring)
            seen += " access " + std::to_string(use.access);
        written |= use.handle == ring && (use.access & TegraGM20BSurfaceWrite);
    }
    EXPECT_TRUE(written) << "ring " << ring << ":" << (seen.empty() ? " not named" : seen);
}

// while (word 0) { 0 counts it down; a copy of word 6 to word 7; 1 } over two compute encoders of one unit with the copy
// engine between them, as the facade lays a dispatch whose textures need shadows: the copies cover all three
TEST_F(DeviceLoops, ALoopSpansItsUnitsComputeEncodersAndACopyBetweenThem)
{
    for (uint32_t n : {5u, raw::LoopCopies::kOutermostCopies + 3}) {
        word(0) = n;
        word(6) = 77;
        word(7) = 0;
        const size_t first = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        std::optional<raw::ComputeEncoder> e;
        e.emplace(*cb, dev);
        ASSERT_EQ(e->startWhile(wordVa(0), PredicateCompare::NotEqual, 0), Answer::Done);
        dispatch(*e, 0);
        e->end();
        raw::BlitEncoder *copy = cb->blit();
        copy->copyBytes(wordVa(6), wordVa(7), 4);
        copy->end();
        e.emplace(*cb, dev);
        dispatch(*e, 1);
        ASSERT_EQ(e->endWhile(kTag), Answer::Done);
        e->end();
        const uint32_t stamp = commit(cb);
        run(first, [&](int program) {
            if (program == 0)
                --word(0);
        });
        const uint32_t ran = std::min(n, raw::LoopCopies::kOutermostCopies);
        EXPECT_EQ(count(0), ran) << n;
        EXPECT_EQ(count(1), ran) << n;
        EXPECT_EQ(word(7), 77u) << n;
        EXPECT_EQ(qmdsBroken, 0u) << n;
        EXPECT_EQ(dev.loopContinue(stamp).continues, n > raw::LoopCopies::kOutermostCopies) << n;
    }
}

}  // namespace
