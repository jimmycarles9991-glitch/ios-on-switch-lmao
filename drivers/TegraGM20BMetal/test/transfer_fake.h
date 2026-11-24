// SPDX-License-Identifier: MIT
// gm20b::Kernel as TextureTransfers calls it, over a copy engine the test runs (FakeGpu::complete executes the copies), and the
// tests' fixture. Its definitions replace kernel.cpp's: include it in one unit of a test.
#pragma once
#include "../transfer.h"
#include "../kit.h"
#include "../fx/fx_image.h"
#include <IOKit/IOReturn.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
struct FakeGpu {
    struct Job {
        uint64_t va;
        uint32_t words, sequence;
    };

    TegraGM20BAbiFence fence{};
    TegraGM20BAbiFenceSemantic semantic{};
    std::mutex lock;  // held through each kernel call but its hook (onWait, onClose): a test's threads call in at once
    uint32_t sequence = 0, allocations = 0, waits = 0, submissions = 0, set = 0, registrations = 0, channelAsks = 0;
    uint64_t handle = 0;
    bool reject = false, completeOnSubmit = false, refuseWait = false, refuseChannel = false;
    uint32_t channelEpoch = 0;  // TegraGM20BChannelEpochWord as the fence page holds it
    uint32_t semanticOnSubmit = 0;
    std::function<void()> onWait, onClose;
    std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> memory;
    std::deque<Job> jobs;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    std::vector<uint32_t> launches;  // LAUNCH_DMA words, in the order they ran
    uint32_t kindSwitches = 0;  // MEM_OP_D L2_SYSMEM_INVALIDATEs the jobs ran (the kit's kind switch ends with one)
    gm20b::JobVisibility visibility = gm20b::JobVisibility::Legacy;

    void execute(const Job &job)
    {
        std::array<uint32_t, 0x800 / 4> state{};
        auto method = [&](uint32_t method, uint32_t value) {
            kindSwitches += method == 0x0034 && value == 0x0eu << 27;
            state.at(method / 4) = value;
            if (method != 0x0300)
                return;
            launches.push_back(value);
            const uint32_t remap = state[0x0708 / 4];
            const uint32_t unit = ((remap >> 16) & 3) + 1;
            const uint32_t sourceBpp = (((remap >> 20) & 3) + 1) * unit, bpp = (((remap >> 24) & 3) + 1) * unit;
            const uint64_t source = uint64_t(state[0x0400 / 4]) << 32 | state[0x0404 / 4];
            const uint64_t destination = uint64_t(state[0x0408 / 4]) << 32 | state[0x040c / 4];
            auto offset = [&](bool source, uint32_t x, uint32_t y) -> uint64_t {
                const uint32_t bpp = source ? sourceBpp : (((remap >> 24) & 3) + 1) * unit;
                const uint32_t pitch = state[(source ? 0x0410 : 0x0414) / 4];
                if (value & (source ? 1u << 7 : 1u << 8))
                    return uint64_t(y) * pitch + x * bpp;
                const uint32_t base = (source ? 0x0728 : 0x070c) / 4;
                const uint32_t origin = state[base + 5];
                if (state[base + 3] > 1) {
                    const uint32_t rowBytes = state[base + 1] * bpp;
                    const uint32_t xByte = ((origin & 0xffff) + x) * bpp;
                    const uint32_t row = (origin >> 16) + y;
                    const uint32_t blockRows = 8u << (state[base] >> 4 & 15);
                    const uint32_t blockPlanes = 1u << (state[base] >> 8 & 15);
                    const uint32_t plane = state[base + 4];
                    const uint64_t block =
                        (uint64_t(plane / blockPlanes) * ((state[base + 2] + blockRows - 1) / blockRows) +
                         row / blockRows) *
                            (rowBytes / 64) +
                        xByte / 64;
                    return block * blockRows * blockPlanes * 64 +
                           (uint64_t(plane % blockPlanes) * (blockRows / 8) + row % blockRows / 8) * 512 +
                           tgm_layout_gob_offset(xByte, row);
                }
                return tgm_layout_offset_of(state[base + 1] * bpp, uint8_t(state[base] >> 4 & 15),
                                            ((origin & 0xffff) + x) * bpp, (origin >> 16) + y);
            };
            for (uint32_t y = 0; y < state[0x041c / 4]; ++y)
                for (uint32_t x = 0; x < state[0x0418 / 4]; ++x)
                    for (uint32_t c = 0; c < bpp / unit; ++c) {
                        const uint32_t from = remap >> 4 * c & 7;
                        if (from == 6)  // NO_WRITE
                            continue;
                        ASSERT_LT(from * unit, sourceBpp);
                        std::memcpy(reinterpret_cast<void *>(destination + offset(false, x, y) + c * unit),
                                    reinterpret_cast<const void *>(source + offset(true, x, y) + from * unit), unit);
                    }
        };
        const auto *words = reinterpret_cast<const uint32_t *>(job.va);
        for (uint32_t i = 0; i < job.words;) {
            const uint32_t header = words[i++], op = header >> 29, count = header >> 16 & 0x1fff;
            const uint32_t first = (header & 0xfff) * 4;
            if (first >= 0x100)  // host methods (below 0x100) go on any subchannel
                ASSERT_EQ((header >> 13) & 7, gm20b::SUBC_COPY);
            if (op == 4) {
                method(first, count);
            } else {
                ASSERT_EQ(op, 1u);
                ASSERT_LE(count, job.words - i);
                for (uint32_t at = 0; at < count; ++at)
                    method(first + 4 * at, words[i++]);
            }
        }
    }

    void complete(uint32_t through)
    {
        while (!jobs.empty() && int32_t(through - jobs.front().sequence) >= 0) {
            execute(jobs.front());
            fence.completed = jobs.front().sequence;
            jobs.pop_front();
        }
    }
};

static_assert(offsetof(FakeGpu, semantic) == sizeof(TegraGM20BAbiFence));
FakeGpu *fake;
}  // namespace

namespace gm20b {
Kernel::~Kernel() = default;

bool Kernel::open(std::string &)
{
    info_.flags = TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord;
    return true;
}

bool Kernel::alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t, uint64_t, bool gpuVolatile, bool control, bool,
                   uint64_t, bool)
{
    std::lock_guard<std::mutex> l(fake->lock);
    EXPECT_TRUE(coherent);
    EXPECT_TRUE(gpuVolatile);
    EXPECT_TRUE(control);
    const uint64_t handle = ++fake->handle;
    auto memory = std::make_unique<uint8_t[]>(bytes);
    bo = {};
    bo.handle = handle;
    bo.cpu = memory.get();
    bo.va = reinterpret_cast<uint64_t>(bo.cpu);
    bo.bytes = bytes;
    fake->memory.emplace(handle, std::move(memory));
    ++fake->allocations;
    return true;
}

void Kernel::free(Bo &bo)
{
    std::lock_guard<std::mutex> l(fake->lock);
    fake->memory.erase(bo.handle);
    bo = {};
}

bool Kernel::channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence)
{
    EXPECT_EQ(engines, uint32_t(TegraGM20BEngineCopy));
    std::lock_guard<std::mutex> l(fake->lock);
    ++fake->channelAsks;
    if (fake->refuseChannel)
        return false;
    channel = 7;
    fake->fence.error = 0;
    fake->semantic = {};
    fence = &fake->fence.completed;
    return true;
}

const volatile uint32_t *Kernel::channelEpoch()
{
    return &fake->channelEpoch;
}

void Kernel::closeChannel(uint32_t)
{
    __atomic_fetch_add(&fake->channelEpoch, 1u, __ATOMIC_RELEASE);
    if (fake->onClose)
        fake->onClose();
    std::lock_guard<std::mutex> l(fake->lock);
    if (fake->fence.error)
        fake->jobs.clear();
    else
        fake->complete(fake->sequence);
}

bool Kernel::submitJob(const KernelJob &job, uint32_t &sequence, int *)
{
    EXPECT_TRUE(job.events.empty());
    EXPECT_EQ(job.channel, 7u);
    EXPECT_EQ(job.pushCount, 1u);
    EXPECT_NE(job.visibility, JobVisibility::Legacy);
    std::lock_guard<std::mutex> l(fake->lock);
    if (fake->reject)
        return false;
    sequence = ++fake->sequence;
    fake->jobs.push_back({job.pushes->va, job.pushes->dwords, sequence});
    fake->uses.assign(job.uses, job.uses + job.useCount);
    fake->visibility = job.visibility;
    fake->set = job.set;
    ++fake->submissions;
    fake->semantic = {2, sequence, sequence, fake->semanticOnSubmit};
    if (fake->completeOnSubmit)
        fake->complete(sequence);
    return true;
}

int Kernel::resourceSet(std::vector<TegraGM20BAbiSurfaceUse> &, uint32_t &set, uint32_t &generation,
                        const volatile uint32_t *&mirror)
{
    static const volatile uint32_t word = 1;
    std::lock_guard<std::mutex> l(fake->lock);
    set = 1;
    generation = ++fake->registrations;
    mirror = &word;
    return KERN_SUCCESS;
}

int Kernel::releaseSet(uint32_t)
{
    return KERN_SUCCESS;
}

bool Kernel::wait(uint32_t, uint32_t sequence, uint64_t, int *result)
{
    if (fake->onWait)
        fake->onWait();
    std::lock_guard<std::mutex> l(fake->lock);
    ++fake->waits;
    if (fake->fence.error || fake->refuseWait)
        return false;
    fake->complete(sequence);
    if ((fake->semantic.flags & TegraGM20BFenceSemanticFailed) && sequence == fake->semantic.failed_first) {
        if (result)
            *result = int(0xe00002eb);
        return false;
    }
    fake->semantic = {};
    return true;
}
}  // namespace gm20b

namespace {
class GM20BTransfer : public ::testing::Test {
protected:
    FakeGpu gpu;
    gm20b::Kernel kernel;
    gm20b::TextureTransfers transfers;
    gm20b::ResourceSet set;
    gm20b::Mutex allocationLock;

    void SetUp() override
    {
        fake = &gpu;
        std::string error;
        ASSERT_TRUE(kernel.open(error));
    }

    void TearDown() override
    {
        gpu.onWait = gpu.onClose = {};
        transfers.close(kernel, allocationLock);
    }

    bool copy(const gm20b::TextureTransfer &region, void *bytes, uint64_t row, bool write,
              std::shared_ptr<void> owner = {})
    {
        return transfers.copy(kernel, set, allocationLock, region, bytes, row, 0, write, std::move(owner));
    }
};
}  // namespace
