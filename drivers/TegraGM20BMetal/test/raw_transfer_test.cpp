// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_transfer.h"
#include "../raw/runtime/raw_kit.h"
#include "../raw/fx/fx_image.h"
#include <IOKit/IOReturn.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
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
    uint32_t sequence = 0, allocations = 0, waits = 0, submissions = 0, set = 0, registrations = 0;
    uint64_t handle = 0;
    bool reject = false, completeOnSubmit = false, refuseWait = false;
    uint32_t semanticOnSubmit = 0;
    std::function<void()> onWait, onClose;
    std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> memory;
    std::deque<Job> jobs;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    std::vector<uint32_t> launches;  // LAUNCH_DMA words, in the order they ran
    raw::JobVisibility visibility = raw::JobVisibility::Legacy;

    void execute(const Job &job)
    {
        std::array<uint32_t, 0x800 / 4> state{};
        auto method = [&](uint32_t method, uint32_t value) {
            state.at(method / 4) = value;
            if (method != 0x0300)
                return;
            launches.push_back(value);
            const uint32_t remap = state[0x0708 / 4];
            const uint32_t unit = ((remap >> 16) & 3) + 1;
            const uint32_t bpp = (((remap >> 20) & 3) + 1) * unit;
            ASSERT_EQ(bpp, (((remap >> 24) & 3) + 1) * unit);
            const uint64_t source = uint64_t(state[0x0400 / 4]) << 32 | state[0x0404 / 4];
            const uint64_t destination = uint64_t(state[0x0408 / 4]) << 32 | state[0x040c / 4];
            auto offset = [&](bool source, uint32_t x, uint32_t y) -> uint64_t {
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
                    std::memcpy(reinterpret_cast<void *>(destination + offset(false, x, y)),
                                reinterpret_cast<const void *>(source + offset(true, x, y)), bpp);
        };
        const auto *words = reinterpret_cast<const uint32_t *>(job.va);
        for (uint32_t i = 0; i < job.words;) {
            const uint32_t header = words[i++], op = header >> 29, count = header >> 16 & 0x1fff;
            const uint32_t first = (header & 0xfff) * 4;
            ASSERT_EQ((header >> 13) & 7, raw::SUBC_COPY);
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

namespace raw {
Kernel::~Kernel() = default;

bool Kernel::open(std::string &)
{
    info_.flags = TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord;
    return true;
}

bool Kernel::alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t, uint64_t, bool gpuVolatile, bool control, bool)
{
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
    fake->memory.erase(bo.handle);
    bo = {};
}

bool Kernel::channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence)
{
    EXPECT_EQ(engines, uint32_t(TegraGM20BEngineCopy));
    channel = 7;
    fake->fence.error = 0;
    fake->semantic = {};
    fence = &fake->fence.completed;
    return true;
}

void Kernel::closeChannel(uint32_t)
{
    if (fake->onClose)
        fake->onClose();
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
    ++fake->waits;
    if (fake->onWait)
        fake->onWait();
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
}  // namespace raw

namespace {
class RawTransfer : public ::testing::Test {
protected:
    FakeGpu gpu;
    raw::Kernel kernel;
    raw::TextureTransfers transfers;
    raw::ResourceSet set;
    raw::Mutex allocationLock;

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

    bool copy(const raw::TextureTransfer &region, void *bytes, uint64_t row, bool write,
              std::shared_ptr<void> owner = {})
    {
        return transfers.copy(kernel, set, allocationLock, region, bytes, row, 0, write, std::move(owner));
    }
};

TEST_F(RawTransfer, UploadSnapshotsCallerBytesAndKeepsOwnerWithoutWaiting)
{
    std::vector<uint8_t> target(128, 0), bytes(128, 0x35);
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 8, 4, 1, 4};
    auto owner = std::make_shared<int>(19);
    std::weak_ptr<int> alive = owner;
    ASSERT_TRUE(copy(region, bytes.data(), 32, true, owner));
    owner.reset();
    EXPECT_FALSE(alive.expired());
    EXPECT_EQ(gpu.waits, 0u);
    EXPECT_EQ(gpu.visibility, raw::JobVisibility::Internal);
    EXPECT_EQ(target[0], 0);
    std::fill(bytes.begin(), bytes.end(), 0xe8);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, std::vector<uint8_t>(128, 0x35));
    ASSERT_EQ(gpu.uses.size(), 2u);
    EXPECT_EQ(gpu.uses[1].handle, region.texture.va);
    EXPECT_EQ(gpu.uses[1].access, uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite));
}

// A texture in a set member goes with the set: the copy job names it and no use of the texture; a write to a member the set
// only read makes it a written one first
TEST_F(RawTransfer, AMemberTexturesCopyNamesTheSet)
{
    std::vector<uint8_t> target(128, 0), bytes(128, 0x35);
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 8, 4, 1, 4};
    raw::Bo chunk;
    chunk.handle = 77;
    chunk.va = chunk.vaBase = reinterpret_cast<uint64_t>(target.data());
    chunk.bytes = target.size();
    ASSERT_TRUE(set.add(kernel, chunk, TegraGM20BSurfaceRead));
    ASSERT_TRUE(copy(region, bytes.data(), 32, true));
    EXPECT_EQ(gpu.set, 1u);
    EXPECT_EQ(gpu.registrations, 2u) << "the written member was not made a written one";
    ASSERT_EQ(gpu.uses.size(), 1u) << "the member texture was named as a use";
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, std::vector<uint8_t>(128, 0x35));
}

TEST_F(RawTransfer, WarmSlotsReuseOnlyAfterTheirFenceAndHandleSequenceZero)
{
    gpu.sequence = UINT32_MAX - 2;
    gpu.fence.completed = gpu.sequence;
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    for (uint32_t i = 0; i < 12; ++i) {
        bytes[0] = uint8_t(i);
        ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    }
    EXPECT_EQ(gpu.allocations, 4u);
    EXPECT_EQ(gpu.submissions, 12u);
    EXPECT_EQ(gpu.waits, 8u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target[0], 11);
}

TEST_F(RawTransfer, ReadbackWaitsAndPreservesCallerRowPadding)
{
    std::vector<uint8_t> target(128, 0x49), bytes(160, 0xab);
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 7, 4, 1, 4};
    ASSERT_TRUE(copy(region, bytes.data(), 40, false));
    EXPECT_EQ(gpu.visibility, raw::JobVisibility::External);
    EXPECT_EQ(gpu.waits, 1u);
    for (size_t i = 0; i < bytes.size(); ++i)
        EXPECT_EQ(bytes[i], i % 40 < 28 ? 0x49 : 0xab);
}

TEST_F(RawTransfer, TiledSubregionRoundTripPreservesOtherTexels)
{
    for (uint32_t bpp : {1u, 2u, 4u, 8u, 16u}) {
        SCOPED_TRACE(bpp);
        tgm_layout layout;
        ASSERT_TRUE(tgm_layout_tiled(43, 29, bpp, &layout));
        std::vector<uint8_t> tiled(layout.bytes, 0x91);
        std::vector<uint8_t> input(23 * 11 * bpp), output(input.size(), 0);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = uint8_t(i * 23 ^ i >> 4);
        raw::TextureTransfer region;
        region.texture = {reinterpret_cast<uint64_t>(tiled.data()), layout.row_bytes, 9, 13, 29, true, layout.y_log2};
        region.width = 23;
        region.height = 11;
        region.planes = 1;
        region.bpp = bpp;
        ASSERT_TRUE(copy(region, input.data(), 23 * bpp, true));
        ASSERT_TRUE(copy(region, output.data(), 23 * bpp, false));
        EXPECT_EQ(input, output);
        for (uint32_t y = 0; y < 29; ++y)
            for (uint32_t x = 0; x < 43; ++x)
                if (x < 9 || x >= 32 || y < 13 || y >= 24)
                    for (uint32_t byte = 0; byte < bpp; ++byte)
                        ASSERT_EQ(tiled[tgm_layout_offset_of(layout.row_bytes, layout.y_log2, x * bpp + byte, y)],
                                  0x91);
    }
}

TEST_F(RawTransfer, FailedSubmissionIsNotPublishedOrAllowedToAlterOutput)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    gpu.reject = true;
    EXPECT_FALSE(copy(region, bytes.data(), 4, false));
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    EXPECT_EQ(gpu.submissions, 0u);
    EXPECT_EQ(gpu.waits, 0u);
}

TEST_F(RawTransfer, VolumePlanesPreserveTheirPhysicalBlockDepthAndCallerImageStride)
{
    for (bool view2d : {false, true}) {
        raw::fx::MipChain layout;
        ASSERT_TRUE(raw::fx::volumeLayout(23, 13, 9, 1, 4, view2d, layout));
        const auto &level = layout.level[0];
        std::vector<uint8_t> tiled(layout.bytes, 0xb3), input(128 * 7 * 4, 0xad), output(input.size(), 0x19);
        raw::TextureTransfer region;
        region.texture = {
            reinterpret_cast<uint64_t>(tiled.data()), level.rowBytes, 5, 4, 13, true, level.yLog2, 9, 3, level.zLog2};
        region.width = 17;
        region.height = 5;
        region.planes = 4;
        region.bpp = 4;
        ASSERT_TRUE(transfers.copy(kernel, set, allocationLock, region, input.data(), 128, 128 * 7, true, {}));
        ASSERT_TRUE(transfers.copy(kernel, set, allocationLock, region, output.data(), 128, 128 * 7, false, {}));
        for (uint32_t z = 0; z < 9; ++z)
            for (uint32_t y = 0; y < 13; ++y)
                for (uint32_t x = 0; x < 23; ++x)
                    for (uint32_t byte = 0; byte < 4; ++byte) {
                        const bool copied = z >= 3 && z < 7 && y >= 4 && y < 9 && x >= 5 && x < 22;
                        ASSERT_EQ(tiled[raw::fx::volumeOffset(layout, 0, x * 4 + byte, y, z)], copied ? 0xad : 0xb3);
                    }
        for (size_t i = 0; i < output.size(); ++i)
            EXPECT_EQ(output[i], i % (128 * 7) / 128 < 5 && i % 128 < 17 * 4 ? 0xad : 0x19);
    }
}

TEST_F(RawTransfer, FailedFenceKeepsQueuedStorageAndOwnerUntilQuiescentClose)
{
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(9);
    std::weak_ptr<int> held = owner;
    for (uint32_t i = 0; i < 4; ++i)
        ASSERT_TRUE(copy(region, bytes.data(), 4, true, owner));
    owner.reset();
    gpu.fence = {4, 1, 4, 1};
    EXPECT_FALSE(held.expired());
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(held.expired());
    EXPECT_TRUE(gpu.memory.empty());
}

TEST_F(RawTransfer, FailedChannelCanServeAnIndependentTextureAfterQuiescence)
{
    std::array<uint8_t, 4> abandoned{}, target{}, bytes{1, 2, 3, 4};
    raw::TextureTransfer region{{reinterpret_cast<uint64_t>(abandoned.data()), 4}, 1, 1, 1, 4};
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.fence = {1, 1, 1, 1};
    region.texture.va = reinterpret_cast<uint64_t>(target.data());
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
    EXPECT_EQ(abandoned, (std::array<uint8_t, 4>{}));
}

TEST_F(RawTransfer, PhysicallyCompleteReadbackWaitsForSemanticSuccess)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.completeOnSubmit = true;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticPending;
    gpu.onWait = [&] {
        EXPECT_EQ(gpu.fence.completed, gpu.sequence);
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    };
    EXPECT_TRUE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_EQ(bytes, target);
    EXPECT_TRUE(held.expired());
}

TEST_F(RawTransfer, PhysicallyCompleteFailedReadbackPreservesOutputAndOwner)
{
    for (uint32_t previous : {0u, UINT32_MAX}) {
        SCOPED_TRACE(previous);
        std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{9, 9, 9, 9};
        const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
        auto owner = std::make_shared<int>(7);
        std::weak_ptr<int> held = owner;
        gpu.sequence = gpu.fence.completed = previous;
        gpu.completeOnSubmit = true;
        gpu.semanticOnSubmit = TegraGM20BFenceSemanticFailed;
        EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
        EXPECT_EQ(gpu.waits, 0u);
        EXPECT_EQ(bytes, (std::array<uint8_t, 4>{9, 9, 9, 9}));
        EXPECT_FALSE(held.expired());
        gpu.onClose = [&] {
            EXPECT_FALSE(held.expired());
        };
        transfers.close(kernel, allocationLock);
        gpu.onClose = {};
        EXPECT_TRUE(held.expired());
    }
}

TEST_F(RawTransfer, FailedSemanticsCannotRetireBeforePhysicalCompletion)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{9, 9, 9, 9};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticFailed;
    gpu.onWait = [&] {
        EXPECT_LT(int32_t(gpu.fence.completed - gpu.sequence), 0);
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(gpu.jobs.size(), 1u);
    };
    EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_TRUE(gpu.jobs.empty());
    EXPECT_EQ(gpu.fence.completed, gpu.sequence);
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{9, 9, 9, 9}));
    EXPECT_FALSE(held.expired());
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(held.expired());
}

TEST_F(RawTransfer, RefusedWaitPinsStorageUntilChannelDrain)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.refuseWait = true;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticPending;
    EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_FALSE(held.expired());
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    gpu.onClose = [&] {
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(gpu.jobs.size(), 1u);
        EXPECT_FALSE(gpu.memory.empty());
    };
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(gpu.jobs.empty());
    EXPECT_TRUE(gpu.memory.empty());
    EXPECT_TRUE(held.expired());
}

TEST_F(RawTransfer, ByteCopyMovesExactlyItsBytesWithoutWaitingAndNamesBothRanges)
{
    const uint64_t line = raw::kCopyLineBytes;
    for (uint64_t bytes : {uint64_t(1), uint64_t(16384), line, line + 16, 3 * line + 512, uint64_t(1400832)}) {
        SCOPED_TRACE(bytes);
        std::vector<uint8_t> source(bytes), target(bytes + 512, 0xcc);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = uint8_t(i * 131 + (i >> 9));
        const uint64_t from = reinterpret_cast<uint64_t>(source.data()), to = reinterpret_cast<uint64_t>(target.data());
        auto owner = std::make_shared<int>(3);
        std::weak_ptr<int> held = owner;
        const uint32_t waits = gpu.waits;
        ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, from, to, bytes, std::move(owner)));
        EXPECT_EQ(gpu.waits, waits);
        EXPECT_EQ(gpu.visibility, raw::JobVisibility::Internal);
        EXPECT_FALSE(held.expired());
        EXPECT_LE(gpu.jobs.back().words, raw::kCopyBytesWords);
        ASSERT_EQ(gpu.uses.size(), 3u);
        EXPECT_EQ(gpu.uses[1].handle, from);
        EXPECT_EQ(gpu.uses[1].access, uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead |
                                               TegraGM20BSurfaceRange | TegraGM20BSurfaceHostWritten));
        EXPECT_EQ(gpu.uses[1].reserved, bytes);
        EXPECT_EQ(gpu.uses[2].handle, to);
        EXPECT_EQ(gpu.uses[2].access,
                  uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite | TegraGM20BSurfaceRange));
        EXPECT_EQ(gpu.uses[2].reserved, bytes);
        gpu.launches.clear();
        gpu.complete(gpu.sequence);
        ASSERT_EQ(gpu.launches.size(), size_t(bytes / line > 0) + size_t(bytes % line > 0));
        EXPECT_EQ(gpu.launches[0] & 3, uint32_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
        if (gpu.launches.size() > 1)
            EXPECT_EQ(gpu.launches[1] & 3, uint32_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
        EXPECT_TRUE(std::equal(source.begin(), source.end(), target.begin()));
        EXPECT_TRUE(std::all_of(target.begin() + bytes, target.end(), [](uint8_t b) {
            return b == 0xcc;
        }));
    }
}

TEST_F(RawTransfer, ByteCopyRefusesWhatItCannotNameAndHoldsNothing)
{
    std::array<uint8_t, 4> source{1, 2, 3, 4}, target{};
    const uint64_t from = reinterpret_cast<uint64_t>(source.data()), to = reinterpret_cast<uint64_t>(target.data());
    auto owner = std::make_shared<int>(5);
    std::weak_ptr<int> held = owner;
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, 0, owner));
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, 0, to, 4, owner));
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, uint64_t(UINT32_MAX) + 1, owner));
    gpu.reject = true;
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, 4, owner));
    owner.reset();
    EXPECT_TRUE(held.expired());
    EXPECT_EQ(gpu.submissions, 0u);
    EXPECT_EQ(target, (std::array<uint8_t, 4>{}));
}

// A pool image's chunk is a set member: the copy into it goes with the set, naming no use of it, and makes the member a
// written one; the caller's pages, outside the set, stay a use of their own
TEST_F(RawTransfer, AByteCopyIntoAMemberNamesTheSet)
{
    std::vector<uint8_t> source(4096, 0x5a), target(4096, 0);
    raw::Bo chunk;
    chunk.handle = 78;
    chunk.va = chunk.vaBase = reinterpret_cast<uint64_t>(target.data());
    chunk.bytes = target.size();
    ASSERT_TRUE(set.add(kernel, chunk, TegraGM20BSurfaceRead));
    ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, reinterpret_cast<uint64_t>(source.data()),
                                    chunk.va + 512, 2048, {}));
    EXPECT_EQ(gpu.set, 1u);
    EXPECT_EQ(gpu.registrations, 2u) << "the written member was not made a written one";
    ASSERT_EQ(gpu.uses.size(), 2u) << "the member was named as a use";
    EXPECT_EQ(gpu.uses[1].handle, reinterpret_cast<uint64_t>(source.data()));
    gpu.complete(gpu.sequence);
    EXPECT_TRUE(std::all_of(target.begin() + 512, target.begin() + 2560, [](uint8_t b) {
        return b == 0x5a;
    }));
    EXPECT_EQ(target[511], 0);
    EXPECT_EQ(target[2560], 0);
}

TEST_F(RawTransfer, ByteCopiesAndUploadsShareTheSlotsInOrder)
{
    std::array<uint8_t, 4> source{9, 8, 7, 6}, copied{}, uploaded{}, bytes{1, 2, 3, 4};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(uploaded.data()), 4}, 1, 1, 1, 4};
    for (uint32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, reinterpret_cast<uint64_t>(source.data()),
                                        reinterpret_cast<uint64_t>(copied.data()), 4, {}));
        ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    }
    EXPECT_EQ(gpu.allocations, 4u);
    EXPECT_EQ(gpu.submissions, 12u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(copied, source);
    EXPECT_EQ(uploaded, bytes);
}

TEST_F(RawTransfer, FailedUploadSlotDrainsChannelBeforeReuse)
{
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const raw::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.completeOnSubmit = true;
    for (uint32_t i = 0; i < 4; ++i)
        ASSERT_TRUE(copy(region, bytes.data(), 4, true, owner));
    owner.reset();
    gpu.semantic = {2, 0, 1, TegraGM20BFenceSemanticFailed};
    unsigned closes = 0;
    gpu.onClose = [&] {
        ++closes;
        EXPECT_FALSE(held.expired());
    };
    EXPECT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.onClose = {};
    EXPECT_EQ(closes, 1u);
    EXPECT_TRUE(held.expired());
}
}  // namespace
