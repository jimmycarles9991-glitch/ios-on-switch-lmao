// SPDX-License-Identifier: MIT
// Zero-on-free of Shared buffers on gm20b::Device (scrubs.cpp), off the copy engine's methods the device submitted: a
// destroyed Shared buffer's span is filled with zeros by the next scrub job, a create before then does not get the span
// back, and one after the job completed does, with no CPU write to it. Metal clears every buffer newBufferWithLength: makes;
// review MR4: the CPU memset every reused span at create.
#include "device_harness.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <cstring>
#include <vector>

namespace {

struct Fill {
    uint64_t va = 0;
    uint32_t constA = 0;
};

class DeviceScrub : public harness::DeviceTest {
protected:
    static std::vector<Fill> fills(size_t before)
    {
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        std::vector<switch_gm20b_method> m(words.size() * 2 + 1);
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        std::vector<Fill> out;
        uint64_t va = 0;
        uint32_t constA = 0;
        for (long i = 0; i < n; i++) {
            if (m[i].subchannel != gm20b::SUBC_COPY)
                continue;
            if (m[i].method == NVB0B5_OFFSET_OUT_UPPER)
                va = uint64_t(m[i].value) << 32 | (va & 0xffffffffu);
            else if (m[i].method == NVB0B5_OFFSET_OUT_LOWER)
                va = (va & ~0xffffffffull) | m[i].value;
            else if (m[i].method == NVB0B5_SET_REMAP_CONST_A)
                constA = m[i].value;
            else if (m[i].method == NVB0B5_LAUNCH_DMA &&
                     (m[i].value & 3) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE && (m[i].value & (1u << 10)))
                out.push_back({va, constA});
        }
        return out;
    }

    static bool filledAt(const std::vector<Fill> &fills, uint64_t va)
    {
        for (const Fill &f : fills)
            if (f.va == va && f.constA == 0)
                return true;
        return false;
    }
};

TEST_F(DeviceScrub, AFreedSharedBufferIsZeroedByTheCopyEngineBeforeItsSpanIsReused)
{
    constexpr uint64_t kBytes = 16 * 1024;
    {
        // the channel's own first job out of the way, so the count below is the command buffer's alone
        gm20b::CommandBuffer *warm = dev.begin();
        ASSERT_NE(warm, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(warm->commit(seqno));
    }
    // a neighbour keeps the chunk live
    gm20b::Buffer neighbour;
    ASSERT_TRUE(dev.createBuffer(kBytes, neighbour, true, true, true));
    gm20b::Buffer first;
    ASSERT_TRUE(dev.createBuffer(kBytes, first, true, true, true));
    ASSERT_TRUE(first.gpuVolatile) << "a Shared buffer lives in the VOL=1 write-combined chunks";
    ASSERT_EQ(first.span.chunk, neighbour.span.chunk) << "the test wants both in one chunk";
    const uint64_t va = first.va;
    uint8_t *const cpu = first.cpu;
    memset(cpu, 0xab, kBytes);
    dev.destroyBuffer(first);
    EXPECT_TRUE(dev.scrubDue());

    gm20b::Buffer early;
    ASSERT_TRUE(dev.createBuffer(kBytes, early, true, true, true));
    EXPECT_NE(early.va, va) << "a create before the span was zeroed got it back";

    // the next command buffer carries the fill at its front, in its own job: no job of its own
    const size_t before = kernel_model::model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(kernel_model::model.jobs.size(), before + 1);
    EXPECT_TRUE(filledAt(fills(before), va)) << "no zero fill at the freed span's address";
    EXPECT_TRUE(kernel_model::model.jobs.back().flags & (TegraGM20BJobVolatileWrites | TegraGM20BJobExternal))
        << "the job that carries the fill ends without flushing its zeros to DRAM";
    dev.destroyBuffer(early);

    // the model completed the job as it was submitted: a look (trim, or an allocation that finds no room) retires it
    dev.trim();
    gm20b::Buffer later;
    ASSERT_TRUE(dev.createBuffer(kBytes, later, true, true, true));
    EXPECT_EQ(later.va, va) << "the zeroed span did not come back";
    // nothing runs the model's pushes, so the bytes the copy engine would zero are what the last owner left: a CPU
    // memset at the create would have cleared them
    EXPECT_EQ(later.cpu[0], 0xab);
    EXPECT_EQ(later.cpu[kBytes - 1], 0xab);
    dev.destroyBuffer(later);
    dev.destroyBuffer(neighbour);
}

TEST_F(DeviceScrub, TheScrubJobEndsVolatileSoItsZerosAreInDram)
{
    gm20b::Buffer neighbour, freed;
    ASSERT_TRUE(dev.createBuffer(16 * 1024, neighbour, true, true, true));
    ASSERT_TRUE(dev.createBuffer(16 * 1024, freed, true, true, true));
    dev.destroyBuffer(freed);
    const size_t before = kernel_model::model.jobs.size();
    uint32_t seqno = 0;
    uint64_t generation = 0;
    ASSERT_TRUE(dev.scrubFreed(seqno, generation));
    EXPECT_NE(seqno, 0u);
    ASSERT_GT(kernel_model::model.jobs.size(), before);
    EXPECT_TRUE(kernel_model::model.jobs.back().flags & TegraGM20BJobVolatileWrites);
    EXPECT_FALSE(kernel_model::model.jobs.back().flags & TegraGM20BJobExternal);
    dev.destroyBuffer(neighbour);
}

// A command buffer that splits into several jobs ends Volatile (or External) on its closing job alone: a scrub it carries
// waits for the seqno commit() returns, never for the job that holds the fill
TEST_F(DeviceScrub, ARideAlongScrubWaitsForItsCommandBuffersLastJob)
{
    constexpr uint64_t kBytes = 16 * 1024;
    gm20b::Buffer neighbour, freed;
    ASSERT_TRUE(dev.createBuffer(kBytes, neighbour, true, true, true));
    ASSERT_TRUE(dev.createBuffer(kBytes, freed, true, true, true));
    const uint64_t va = freed.va;
    dev.destroyBuffer(freed);
    kernel_model::model.inFlight = true;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    kernel_model::model.inFlight = false;
    TegraGM20BAbiFence &fence = kernel_model::model.fence(liveChannel() - 1);
    __atomic_store_n(&fence.completed, seqno - 1, __ATOMIC_RELEASE);
    dev.trim();
    gm20b::Buffer early;
    ASSERT_TRUE(dev.createBuffer(kBytes, early, true, true, true));
    EXPECT_NE(early.va, va) << "the span came back before its command buffer's last job completed";
    __atomic_store_n(&fence.completed, seqno, __ATOMIC_RELEASE);
    dev.trim();
    gm20b::Buffer later;
    ASSERT_TRUE(dev.createBuffer(kBytes, later, true, true, true));
    EXPECT_EQ(later.va, va) << "the zeroed span did not come back once the last job completed";
    dev.destroyBuffer(later);
    dev.destroyBuffer(early);
    dev.destroyBuffer(neighbour);
}

// A kill moves the fence past every seqno before the next begin() opens a channel: a scrub whose job was in flight is
// not finished by that, and its span is filled again rather than handed back
TEST_F(DeviceScrub, AKilledScrubIsFilledAgainNotRetired)
{
    constexpr uint64_t kBytes = 16 * 1024;
    gm20b::Buffer neighbour, freed;
    ASSERT_TRUE(dev.createBuffer(kBytes, neighbour, true, true, true));
    ASSERT_TRUE(dev.createBuffer(kBytes, freed, true, true, true));
    const uint64_t va = freed.va;
    dev.destroyBuffer(freed);
    kernel_model::model.inFlight = true;
    uint32_t seqno = 0;
    uint64_t generation = 0;
    ASSERT_TRUE(dev.scrubFreed(seqno, generation));
    kernel_model::model.kill(liveChannel(), 2);
    kernel_model::model.inFlight = false;
    dev.trim();  // retires what is finished, as an allocation that finds no room does
    gm20b::Buffer taken;
    ASSERT_TRUE(dev.createBuffer(kBytes, taken, true, true, true));
    EXPECT_NE(taken.va, va) << "a span whose fill was killed came back unzeroed";
    EXPECT_TRUE(dev.scrubDue());
    dev.destroyBuffer(taken);
    dev.destroyBuffer(neighbour);
}

TEST_F(DeviceScrub, NoFreedSpanNoScrubJob)
{
    const size_t before = kernel_model::model.jobs.size();
    uint32_t seqno = 1;
    uint64_t generation = 1;
    ASSERT_TRUE(dev.scrubFreed(seqno, generation));
    EXPECT_EQ(seqno, 0u);
    EXPECT_EQ(kernel_model::model.jobs.size(), before);
    EXPECT_FALSE(dev.scrubDue());
}

}  // namespace
