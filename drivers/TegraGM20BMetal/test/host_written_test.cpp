// SPDX-License-Identifier: MIT
// The GPU's L2 is not coherent with the CPU: the kernel drops its lines of memory the CPU wrote (L2_SYSMEM_INVALIDATE,
// gpu_exec.c) only for a job it knows the write came before. gm20b::Device over the kernel model (device_harness.h),
// whose SubmitJob says which jobs the kernel takes as host-written.
#include "device_harness.h"
#include "../heap_members.h"
#include "TegraGM20BVolatileABI.h"
#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t kBgra8 = 80, kSide = 16;

class HostWritten : public harness::DeviceTest {
protected:
    gm20b::Texture target, other;

    void SetUp() override
    {
        harness::DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, target, true));
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, other, true));
        ASSERT_TRUE(target.wc);
    }

    // One job copying `from` into `to`: whether the kernel took it as host-written
    bool copy(const gm20b::Texture &from, const gm20b::Texture &to)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e) {
            ADD_FAILURE() << "no blit encoder";
            return false;
        }
        e->copyTexels(from, 0, 0, 0, to, 0, 0, 0, kSide, kSide);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return !kernel_model::model.jobs.empty() && kernel_model::model.jobs.back().hostWritten;
    }

    uint64_t bytes() const
    {
        return uint64_t(target.pitch) * kSide;
    }
};

// The bench's fill before it took a lease: the GPU wrote the target, the CPU rewrote it with a bare memcpy, and the next
// job's ROP read the GPU's older lines from the L2
TEST_F(HostWritten, ABareCpuWriteTellsTheKernelNothing)
{
    copy(other, target);
    memset(target.cpu, 0x5a, bytes());
    EXPECT_FALSE(copy(target, other));
}

// Shared textures' replaceRegion (TextureCpuAccess) and now the bench's fill
TEST_F(HostWritten, AWriteUnderALeaseMakesTheNextJobThatReadsItDropTheL2First)
{
    copy(other, target);
    {
        gm20b::HostAccess lease;
        ASSERT_TRUE(dev.hostAccess(target, target.cpu, bytes(), true, lease));
        memset(target.cpu, 0x5a, bytes());
    }
    EXPECT_TRUE(copy(target, other));
    EXPECT_FALSE(copy(target, other)) << "a later job: the L2 holds no line older than the CPU's write";
}

TEST_F(HostWritten, AReadLeaseIsNoWrite)
{
    copy(other, target);
    {
        gm20b::HostAccess lease;
        ASSERT_TRUE(dev.hostAccess(target, target.cpu, bytes(), false, lease));
    }
    EXPECT_FALSE(copy(target, other));
}

// The idle give-back's order (TegraGM20BMetal.mm): the freed Shared spans zeroed and back in their chunks, then trim, so
// an emptied chunk parks or goes back
void giveBack(gm20b::Device &dev)
{
    uint32_t seqno = 0;
    uint64_t generation = 0;
    ASSERT_TRUE(dev.scrubFreed(seqno, generation));
    dev.scrubWait(seqno, generation);
    dev.trim();
}

TEST(HostWrittenVolatileWc, VolatileWcBufferSkipsHostWrittenKeepsSetAndEmitsVolatileWrites)
{
    kernel_model::model = {};
    std::vector<TegraGM20BAbiBindOp> bindOps;
    kernel_model::model.bound = [&](const TegraGM20BAbiBindOp &op) {
        bindOps.push_back(op);
    };
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    // Drain the initial shader-heap invalidation before creating the buffer so firstRead measures createBuffer alone.
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
    const unsigned acquiresBefore = kernel_model::model.cpuAcquireCalls;
    gm20b::Buffer shared, priv;
    ASSERT_TRUE(dev.createBuffer(4096, shared, true, true, true));
    ASSERT_TRUE(dev.createBuffer(4096, priv, false, false));
    kernel_model::model.bound = nullptr;
    ASSERT_TRUE(shared.wc);
    ASSERT_TRUE(shared.gpuVolatile);
    EXPECT_EQ(kernel_model::model.cpuAcquireCalls, acquiresBefore);

    const uint64_t chunkVa = shared.span.va - shared.span.offset;
    const auto bind = std::find_if(bindOps.begin(), bindOps.end(), [&](const TegraGM20BAbiBindOp &op) {
        return op.va == chunkVa;
    });
    ASSERT_NE(bind, bindOps.end());
    EXPECT_EQ(bind->op, uint32_t(TegraGM20BBindMapVolatile));
    const uint64_t chunkHandle = bind->handle;
    ASSERT_TRUE(kernel_model::model.bos.count(chunkHandle));
    EXPECT_FALSE(kernel_model::model.bos[chunkHandle].cpuReached);

    const auto copyBuf = [&](const gm20b::Buffer &src, const gm20b::Buffer &dst) {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        EXPECT_NE(e, nullptr);
        cb->use(src, false);
        cb->use(dst, true, false);
        e->copyBytes(src.va, dst.va, 4096);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return kernel_model::model.jobs.back();
    };

    const kernel_model::Job firstRead = copyBuf(shared, priv);
    EXPECT_FALSE(firstRead.hostWritten);
    EXPECT_EQ(firstRead.flags & TegraGM20BJobHostWritten, 0u);
    for (const TegraGM20BAbiSurfaceUse &u : firstRead.uses)
        EXPECT_EQ(u.access & TegraGM20BSurfaceHostWritten, 0u);
    EXPECT_NE(kernel_model::model.bos[chunkHandle].set, 0u);

    const kernel_model::Job steadyRead = copyBuf(shared, priv);
    EXPECT_FALSE(steadyRead.hostWritten);
    EXPECT_EQ(steadyRead.flags & TegraGM20BJobHostWritten, 0u);

    const kernel_model::Job writeBack = copyBuf(priv, shared);
    EXPECT_EQ(writeBack.flags & TegraGM20BJobExternal, 0u);
    EXPECT_NE(writeBack.flags & TegraGM20BJobVolatileWrites, 0u);
    EXPECT_FALSE(writeBack.hostWritten);

    // Explicit hostAccess and span reuse never evict the volatile chunk or bump host_epoch
    {
        gm20b::HostAccess access;
        ASSERT_TRUE(dev.hostAccess(shared, true, access));
        ASSERT_TRUE(access.finish());
    }
    EXPECT_EQ(kernel_model::model.cpuAcquireCalls, acquiresBefore);
    dev.destroyBuffer(shared);
    ASSERT_TRUE(dev.createBuffer(4096, shared, true, true, true));
    EXPECT_EQ(kernel_model::model.cpuAcquireCalls, acquiresBefore);
    const kernel_model::Job reusedRead = copyBuf(shared, priv);
    EXPECT_FALSE(reusedRead.hostWritten);
    EXPECT_EQ(reusedRead.flags & TegraGM20BJobHostWritten, 0u);

    dev.destroyBuffer(shared);
    dev.destroyBuffer(priv);
}

TEST(HostWrittenVolatileWc, PlainSharedTexturesAndCachedWcBuffersStayL2Cached)
{
    kernel_model::model = {};
    std::vector<TegraGM20BAbiBindOp> bindOps;
    kernel_model::model.bound = [&](const TegraGM20BAbiBindOp &op) {
        bindOps.push_back(op);
    };
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Texture sharedTex;
    gm20b::Buffer cachedWc, volatileWc;
    ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, sharedTex, true));
    ASSERT_TRUE(dev.createBuffer(4096, cachedWc, true, true, false));
    ASSERT_TRUE(dev.createBuffer(4096, volatileWc, true, true, true));
    kernel_model::model.bound = nullptr;

    ASSERT_TRUE(sharedTex.wc);
    EXPECT_FALSE(sharedTex.storageRange().gpuVolatile);
    ASSERT_TRUE(cachedWc.wc);
    EXPECT_FALSE(cachedWc.gpuVolatile);
    ASSERT_TRUE(volatileWc.wc);
    EXPECT_TRUE(volatileWc.gpuVolatile);

    const auto findBind = [&](uint64_t chunkVa) {
        return std::find_if(bindOps.begin(), bindOps.end(), [&](const TegraGM20BAbiBindOp &op) {
            return op.va == chunkVa;
        });
    };
    const auto texBind = findBind(sharedTex.span.va - sharedTex.span.offset);
    const auto cachedBind = findBind(cachedWc.span.va - cachedWc.span.offset);
    const auto volBind = findBind(volatileWc.span.va - volatileWc.span.offset);
    ASSERT_NE(texBind, bindOps.end());
    ASSERT_NE(cachedBind, bindOps.end());
    ASSERT_NE(volBind, bindOps.end());
    EXPECT_EQ(texBind->op, uint32_t(TegraGM20BBindMap));
    EXPECT_EQ(cachedBind->op, uint32_t(TegraGM20BBindMap));
    EXPECT_EQ(volBind->op, uint32_t(TegraGM20BBindMapVolatile));
    EXPECT_NE(texBind->handle, volBind->handle);

    dev.destroyTexture(sharedTex);
    dev.destroyBuffer(cachedWc);
    dev.destroyBuffer(volatileWc);
}

TEST(HostWrittenVolatileWc, BufferTextureViewTexelBufferAndHeapPropagateGpuVolatile)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
    gm20b::Buffer shared, privBuf;
    gm20b::Texture bufTex, bufTexView, texelBuf, privTex;
    ASSERT_TRUE(dev.createBuffer(4096, shared, true, true, true));
    ASSERT_TRUE(dev.createBuffer(4096, privBuf, false, false));
    ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, privTex, false));
    ASSERT_TRUE(dev.createBufferTexture(shared, 0, kSide, kSide, kSide * 4, kBgra8, bufTex));
    ASSERT_TRUE(dev.createView(bufTex, kBgra8, bufTexView));
    ASSERT_TRUE(dev.createTexelBuffer(shared, 0, 64, kBgra8, texelBuf));

    EXPECT_TRUE(bufTex.storageRange().gpuVolatile);
    EXPECT_TRUE(bufTexView.storageRange().gpuVolatile);
    EXPECT_TRUE(texelBuf.storageRange().gpuVolatile);

    const auto copyTex = [&](const gm20b::Texture &src, const gm20b::Texture &dst) {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        EXPECT_NE(e, nullptr);
        e->copyTexels(src, 0, 0, 0, dst, 0, 0, 0, kSide, kSide);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return kernel_model::model.jobs.back();
    };

    const kernel_model::Job readBufTex = copyTex(bufTex, privTex);
    EXPECT_FALSE(readBufTex.hostWritten);
    EXPECT_EQ(readBufTex.flags & TegraGM20BJobHostWritten, 0u);

    const kernel_model::Job readView = copyTex(bufTexView, privTex);
    EXPECT_FALSE(readView.hostWritten);
    EXPECT_EQ(readView.flags & TegraGM20BJobHostWritten, 0u);

    const kernel_model::Job writeBufTex = copyTex(privTex, bufTex);
    EXPECT_EQ(writeBufTex.flags & TegraGM20BJobExternal, 0u);
    EXPECT_NE(writeBufTex.flags & TegraGM20BJobVolatileWrites, 0u);

    const kernel_model::Job writeView = copyTex(privTex, bufTexView);
    EXPECT_EQ(writeView.flags & TegraGM20BJobExternal, 0u);
    EXPECT_NE(writeView.flags & TegraGM20BJobVolatileWrites, 0u);

    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(texelBuf, false);
        cb->use(privBuf, true, false);
        e->copyBytes(shared.va, privBuf.va, 256);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        EXPECT_FALSE(kernel_model::model.jobs.back().hostWritten);
        EXPECT_EQ(kernel_model::model.jobs.back().flags & TegraGM20BJobHostWritten, 0u);
    }

    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(privBuf, false);
        cb->use(texelBuf, true);
        e->copyBytes(privBuf.va, shared.va, 256);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        EXPECT_EQ(kernel_model::model.jobs.back().flags & TegraGM20BJobExternal, 0u);
        EXPECT_NE(kernel_model::model.jobs.back().flags & TegraGM20BJobVolatileWrites, 0u);
    }

    // HeapMember over a genuinely VOL=1 buffer omits TegraGM20BSurfaceHostWritten even when evicted from ResourceSet
    gm20b::Buffer offSet;
    ASSERT_TRUE(dev.createBuffer(1u << 20, offSet, true, true, true));
    ASSERT_TRUE(offSet.wc && offSet.gpuVolatile);
    const uint64_t offSetHandle = kernel_model::model.resolve({offSet.va, TegraGM20BSurfaceGpuAddress, 0});
    ASSERT_NE(offSetHandle, 0u);
    kernel_model::model.cpuLease(offSetHandle);
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(privBuf, true, false);
        e->fillWords(privBuf.va, 64, 0);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
    ASSERT_FALSE(gm20b::DeviceHarness::setHolds(dev, offSet.va, offSet.length));
    gm20b::HeapMembers heap(gm20b::HeapMembers::kNoArena, true);
    heap.add({shared.va, shared.length, shared.hostVisible, shared.gpuVolatile});
    heap.add({offSet.va, offSet.length, offSet.hostVisible, offSet.gpuVolatile});
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        cb->internalCompletion();
        cb->useHeap(heap);
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(privBuf, true, false);
        e->fillWords(privBuf.va, 64, 0);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        const kernel_model::Job &job = kernel_model::model.jobs.back();
        EXPECT_FALSE(job.hostWritten);
        EXPECT_EQ(job.flags & TegraGM20BJobHostWritten, 0u);
        bool sawOffSet = false;
        for (const TegraGM20BAbiSurfaceUse &u : job.uses) {
            if (u.handle == offSetHandle && (u.access & TegraGM20BSurfaceGpuAddress))
                sawOffSet = true;
            EXPECT_EQ(u.access & TegraGM20BSurfaceHostWritten, 0u);
        }
        EXPECT_TRUE(sawOffSet);
    }

    dev.destroyBuffer(offSet);
    dev.destroyTexture(texelBuf);
    dev.destroyTexture(bufTexView);
    dev.destroyTexture(bufTex);
    dev.destroyTexture(privTex);
    dev.destroyBuffer(shared);
    dev.destroyBuffer(privBuf);
}

TEST(HostWrittenVolatileWc, FallsBackToCachedWcAndExternalWritesWithoutKernelCapability)
{
    kernel_model::model = {};
    kernel_model::model.volatileWrites = false;
    std::vector<TegraGM20BAbiBindOp> bindOps;
    kernel_model::model.bound = [&](const TegraGM20BAbiBindOp &op) {
        bindOps.push_back(op);
    };
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Buffer shared, priv;
    ASSERT_TRUE(dev.createBuffer(4096, shared, true, true, true));
    ASSERT_TRUE(dev.createBuffer(4096, priv, false, false));
    kernel_model::model.bound = nullptr;
    ASSERT_TRUE(shared.wc);
    EXPECT_FALSE(shared.gpuVolatile);

    const uint64_t chunkVa = shared.span.va - shared.span.offset;
    const auto bind = std::find_if(bindOps.begin(), bindOps.end(), [&](const TegraGM20BAbiBindOp &op) {
        return op.va == chunkVa;
    });
    ASSERT_NE(bind, bindOps.end());
    EXPECT_EQ(bind->op, uint32_t(TegraGM20BBindMap));

    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->internalCompletion();
    gm20b::BlitEncoder *e = cb->blit();
    ASSERT_NE(e, nullptr);
    cb->use(priv, false);
    cb->use(shared, true, false);
    e->copyBytes(priv.va, shared.va, 4096);
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_NE(kernel_model::model.jobs.back().flags & TegraGM20BJobExternal, 0u);
    EXPECT_EQ(kernel_model::model.jobs.back().flags & TegraGM20BJobVolatileWrites, 0u);

    dev.destroyBuffer(shared);
    dev.destroyBuffer(priv);
}

TEST(HostWrittenVolatileWc, NoCopyBufferRefusesBaseAndInteriorOfVolatileAndCachedWcBuffers)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer volBuf, cachedWcBuf;
    ASSERT_TRUE(dev.createBuffer(16384, volBuf, true, true, true));
    ASSERT_TRUE(dev.createBuffer(16384, cachedWcBuf, true, true, false));
    ASSERT_TRUE(volBuf.wc && volBuf.gpuVolatile && volBuf.cpu != nullptr);
    ASSERT_TRUE(cachedWcBuf.wc && !cachedWcBuf.gpuVolatile && cachedWcBuf.cpu != nullptr);

    const size_t importsBefore = kernel_model::model.imports.size();
    gm20b::Buffer over;
    bool tiled = false;
    EXPECT_FALSE(dev.createBufferNoCopy(volBuf.cpu, 4096, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_FALSE(dev.createBufferNoCopy(volBuf.cpu + 4096, 4096, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_FALSE(dev.createBufferNoCopy(cachedWcBuf.cpu, 4096, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_FALSE(dev.createBufferNoCopy(cachedWcBuf.cpu + 4096, 4096, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);

    dev.destroyBuffer(cachedWcBuf);
    dev.destroyBuffer(volBuf);
}

TEST(HostWrittenVolatileWc, IdleVolatileWcChunksParkAndUnparkResidentWithoutAliasingCachedWc)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    kernel_model::model.coherentFromPool = false;
    std::vector<TegraGM20BAbiBindOp> bindOps;
    kernel_model::model.bound = [&](const TegraGM20BAbiBindOp &op) {
        bindOps.push_back(op);
    };
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }

    gm20b::Buffer volBuf, privBuf;
    ASSERT_TRUE(dev.createBuffer(64 << 10, volBuf, true, true, true));
    ASSERT_TRUE(dev.createBuffer(4096, privBuf, false, false));
    ASSERT_TRUE(volBuf.wc && volBuf.gpuVolatile);
    const uint64_t volVa = volBuf.span.va - volBuf.span.offset;
    const uint64_t volHandle = kernel_model::model.resolve({volBuf.va, TegraGM20BSurfaceGpuAddress, 0});
    ASSERT_NE(volHandle, 0u);
    const auto firstBind = std::find_if(bindOps.begin(), bindOps.end(), [&](const TegraGM20BAbiBindOp &op) {
        return op.va == volVa;
    });
    ASSERT_NE(firstBind, bindOps.end());
    EXPECT_EQ(firstBind->op, uint32_t(TegraGM20BBindMapVolatile));
    EXPECT_EQ(dev.wcChunksHeld(), 1u);
    EXPECT_GT(dev.wcChunkBytesHeld(), 0u);

    dev.destroyBuffer(volBuf);
    giveBack(dev);
    EXPECT_EQ(kernel_model::model.parkCalls, 1u);
    ASSERT_EQ(kernel_model::model.bos.count(volHandle), 1u);
    EXPECT_EQ(kernel_model::model.bos[volHandle].parkState, kernel_model::ParkState::Parked);
    EXPECT_EQ(dev.wcChunksHeld(), 0u);
    EXPECT_EQ(dev.wcChunkBytesHeld(), 0u);
    gm20b::Device::Held h = {};
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.writeCombined, 0u);

    // A non-volatile WC allocation must not unpark a parked volatile chunk.
    gm20b::Buffer cachedWc;
    ASSERT_TRUE(dev.createBuffer(64 << 10, cachedWc, true, true, false));
    ASSERT_TRUE(cachedWc.wc && !cachedWc.gpuVolatile);
    const uint64_t cachedHandle = kernel_model::model.resolve({cachedWc.va, TegraGM20BSurfaceGpuAddress, 0});
    EXPECT_NE(cachedHandle, volHandle);
    EXPECT_EQ(kernel_model::model.unparkResident, 0u);
    EXPECT_EQ(kernel_model::model.bos[volHandle].parkState, kernel_model::ParkState::Parked);
    dev.destroyBuffer(cachedWc);

    // Reallocating a volatile WC buffer unparks the parked VOL=1 chunk resident. It rejoins the ResourceSet at the next
    // begin, not on the unparking thread; a command buffer already open names it per job by address until then.
    bindOps.clear();
    gm20b::CommandBuffer *open = dev.begin();
    ASSERT_NE(open, nullptr);
    ASSERT_TRUE(dev.createBuffer(64 << 10, volBuf, true, true, true));
    ASSERT_TRUE(volBuf.wc && volBuf.gpuVolatile);
    EXPECT_EQ(kernel_model::model.unparkResident, 1u);
    EXPECT_EQ(kernel_model::model.resolve({volBuf.va, TegraGM20BSurfaceGpuAddress, 0}), volHandle);
    EXPECT_TRUE(bindOps.empty());
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, volBuf.va, volBuf.length));
    {
        open->internalCompletion();
        gm20b::BlitEncoder *e = open->blit();
        ASSERT_NE(e, nullptr);
        open->use(volBuf, false);
        open->use(privBuf, true, false);
        e->copyBytes(volBuf.va, privBuf.va, 4096);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(open->commit(seqno));
        // The model stores each use under the BO it resolved to (kernel_model.h run)
        const auto &uses = kernel_model::model.jobs.back().uses;
        EXPECT_TRUE(std::any_of(uses.begin(), uses.end(), [&](const TegraGM20BAbiSurfaceUse &use) {
            return use.handle == volHandle && (use.access & TegraGM20BSurfaceGpuAddress);
        })) << "the open command buffer's job names the unparked chunk by its address";
    }

    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, volBuf.va, volBuf.length));
        cb->internalCompletion();
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(volBuf, false);
        cb->use(privBuf, true, false);
        e->copyBytes(volBuf.va, privBuf.va, 4096);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        EXPECT_FALSE(kernel_model::model.jobs.back().hostWritten);
        EXPECT_EQ(kernel_model::model.jobs.back().flags & TegraGM20BJobHostWritten, 0u);
    }
    // unparked as it was made, a written member, so a write to it does not make the set again
    uint32_t access = 0;
    for (const kernel_model::SetState &set : kernel_model::model.sets)
        for (const TegraGM20BAbiSurfaceUse &m : set.members)
            access |= set.live && m.handle == volHandle ? m.access : 0u;
    EXPECT_TRUE(access & TegraGM20BSurfaceWrite) << "the unparked chunk rejoined the set read";
    const auto generations = [] {
        std::vector<uint32_t> g;
        for (const kernel_model::SetState &set : kernel_model::model.sets)
            g.push_back(set.live ? set.generation : 0);
        return g;
    };
    const std::vector<uint32_t> setBefore = generations();
    {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::BlitEncoder *e = cb->blit();
        ASSERT_NE(e, nullptr);
        cb->use(privBuf, false);
        cb->use(volBuf, true);
        e->copyBytes(privBuf.va, volBuf.va, 4096);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
    EXPECT_EQ(generations(), setBefore) << "a write to the unparked chunk made the set again";

    // Reclaimed volatile WC chunks drop the dead BO and allocate a fresh VOL=1 chunk.
    dev.destroyBuffer(volBuf);
    giveBack(dev);
    ASSERT_EQ(kernel_model::model.bos[volHandle].parkState, kernel_model::ParkState::Parked);
    kernel_model::model.reclaimAllParked();
    bindOps.clear();
    ASSERT_TRUE(dev.createBuffer(64 << 10, volBuf, true, true, true));
    ASSERT_TRUE(volBuf.wc && volBuf.gpuVolatile);
    EXPECT_GE(kernel_model::model.unparkReclaimed, 1u);
    EXPECT_EQ(kernel_model::model.bos.count(volHandle), 0u);
    const uint64_t freshVa = volBuf.span.va - volBuf.span.offset;
    const auto freshBind = std::find_if(bindOps.begin(), bindOps.end(), [&](const TegraGM20BAbiBindOp &op) {
        return op.va == freshVa;
    });
    ASSERT_NE(freshBind, bindOps.end());
    EXPECT_EQ(freshBind->op, uint32_t(TegraGM20BBindMapVolatile));

    kernel_model::model.bound = nullptr;
    dev.destroyBuffer(volBuf);
    dev.destroyBuffer(privBuf);
}

TEST(HostWrittenVolatileWc, IdleVolatileWcChunksFreeDirectlyWithoutBoPark)
{
    kernel_model::model = {};
    kernel_model::model.coherentFromPool = false;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer volBuf;
    ASSERT_TRUE(dev.createBuffer(64 << 10, volBuf, true, true, true));
    ASSERT_TRUE(volBuf.wc && volBuf.gpuVolatile);
    const uint64_t handle = kernel_model::model.resolve({volBuf.va, TegraGM20BSurfaceGpuAddress, 0});
    ASSERT_NE(handle, 0u);

    dev.destroyBuffer(volBuf);
    giveBack(dev);
    EXPECT_EQ(kernel_model::model.parkCalls, 0u);
    EXPECT_EQ(kernel_model::model.bos.count(handle), 0u);
    EXPECT_EQ(dev.wcChunksHeld(), 0u);
    EXPECT_EQ(dev.wcChunkBytesHeld(), 0u);
    gm20b::Device::Held h = {};
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.writeCombined, 0u);
}

TEST(HostWrittenVolatileWc, CachedHostBufferOnlyInvalidatesL2WhenCpuDirty)
{
    kernel_model::model = {};
    kernel_model::model.coherentFromPool = false;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer shared, dst;
    ASSERT_TRUE(dev.createBuffer(4096, shared, false, true, false));
    ASSERT_TRUE(dev.createBuffer(4096, dst, false, false, false));
    ASSERT_TRUE(shared.hostVisible && !shared.gpuVolatile);

    auto copyOnce = [&]() -> bool {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e) {
            ADD_FAILURE() << "no blit encoder";
            return false;
        }
        cb->use(shared, false);
        cb->use(dst, true);
        e->copyBytes(shared.va, dst.va, 256);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return !kernel_model::model.jobs.empty() && kernel_model::model.jobs.back().hostWritten;
    };

    EXPECT_TRUE(copyOnce()) << "first GPU read of a new cached-host buffer must invalidate L2";
    EXPECT_FALSE(copyOnce()) << "steady read of an unmodified cached-host buffer skips L2 invalidate";
    shared.markCpuDirty();
    EXPECT_TRUE(copyOnce()) << "CPU modification re-arms L2 invalidate once";
    EXPECT_FALSE(copyOnce()) << "subsequent steady read skips L2 invalidate again";

    dev.destroyBuffer(shared);
    dev.destroyBuffer(dst);
}

}  // namespace
