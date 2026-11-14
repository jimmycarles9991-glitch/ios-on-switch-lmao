// SPDX-License-Identifier: MIT
// recover_engine.c's owed preempt (owed_preempt, owed_ask and tgm_recover_owed, cut out by :owed_preempt_source) over
// one PBDMA's registers. Its step runs under device->intr.lock with interrupts off, at every engine reset's release: the
// stand-ins throw on a lock, sleep or allocation then. PFIFO_PREEMPT is written only under the runlist's mutex, as
// every other writer of it holds that.
#include <gtest/gtest.h>
#include <cstdint>
#include "switch_gpu_interrupts_off_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BHost.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

typedef uint32_t u32;

struct nvkm_runq {
	int id;
	nvkm_runq *next;
};

struct nvkm_runl {
	struct mutex mutex;
	nvkm_runl *next;
};

struct nvkm_fifo {
	nvkm_runq *runqs;
	nvkm_runl *runls;
};

struct nvkm_intr {
	int lock;
};

struct nvkm_device {
	nvkm_fifo *fifo;
	nvkm_intr intr;
};

namespace
{
u32 preemptReg, pbdmaStatus, pbdmaChannel, written;
int writes, held, heldAtWrite = -1;
bool alive = true, aliveTakesMutex = false;
mutex chanMutex;

u32 nvkm_rd32(nvkm_device *, u32 addr)
{
	if (addr == PFIFO_PREEMPT)
		return preemptReg;
	if (addr == PBDMA_STATUS(0))
		return pbdmaStatus;
	if (addr == PBDMA_CHANNEL(0))
		return pbdmaChannel;
	return 0;
}

void nvkm_wr32(nvkm_device *, u32 addr, u32 value)
{
	if (addr == PFIFO_PREEMPT) {
		written = value;
		writes++;
		heldAtWrite = held;
		preemptReg = PFIFO_PREEMPT_PENDING | value;
	}
}

// recover_engine.c's: nvkm_chan_get_chid takes the FIFO's spinlock, never a mutex
bool chan_alive(nvkm_fifo *, u32)
{
	if (aliveTakesMutex)
		mutex_lock(&chanMutex);
	return alive;
}

struct {
	u32 holds;
	int owed;
} rst;

void runlLock(mutex *m)
{
	mutex_lock(m);
	held++;
}

void runlUnlock(mutex *)
{
	held--;
}
}  // namespace

#define nvkm_runq_foreach(runq, fifo) for ((runq) = (fifo)->runqs; (runq); (runq) = (runq)->next)
#define nvkm_runl_foreach(runl, fifo) for ((runl) = (fifo)->runls; (runl); (runl) = (runl)->next)
#define mutex_lock(m) runlLock(m)
#define mutex_unlock(m) runlUnlock(m)
#define spin_lock_irqsave(lock, flags) local_irq_save(flags)
#define spin_unlock_irqrestore(lock, flags) local_irq_restore(flags)

#include "drivers/TegraGM20B/test/owed_preempt_source.h"

namespace
{
class OwedPreempt : public testing::Test
{
protected:
	nvkm_runq runq{0, nullptr};
	nvkm_runl runl{{nullptr}, nullptr};
	nvkm_fifo fifo{&runq, &runl};
	nvkm_device device{&fifo, {}};

	void SetUp() override
	{
		preemptReg = written = 0;
		writes = held = 0;
		heldAtWrite = -1;
		alive = true;
		aliveTakesMutex = false;
		pbdmaStatus = 1u << 13 | 21u << 16 | 21u;
		pbdmaChannel = 21;
		rst = {0, 21};
		interrupts_off::depth = 0;
	}
};

TEST_F(OwedPreempt, AskedOnceNoPreemptIsInFlightWithInterruptsOff)
{
	EXPECT_EQ(tgm_recover_owed(&device), 21);
	EXPECT_EQ(writes, 1);
	EXPECT_EQ(written, 21u);
	EXPECT_EQ(rst.owed, -1);
	EXPECT_EQ(heldAtWrite, 1) << "under the runlist's mutex";
	EXPECT_EQ(held, 0);
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 1) << "asked once";
	EXPECT_EQ(interrupts_off::depth, 0);
}

TEST_F(OwedPreempt, WaitsWhileAnotherPreemptIsInFlight)
{
	preemptReg = PFIFO_PREEMPT_PENDING | 10u;
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 0);
	EXPECT_EQ(rst.owed, 21);
	preemptReg = 10u;
	EXPECT_EQ(tgm_recover_owed(&device), 21);
	EXPECT_EQ(written, 21u);
}

TEST_F(OwedPreempt, AChannelThatLeftThePbdmaOrDiedIsOwedNothing)
{
	pbdmaChannel = 24;
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 0);
	EXPECT_EQ(rst.owed, -1);
	rst.owed = 21;
	pbdmaChannel = 21;
	alive = false;
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 0);
	rst.owed = 21;
	alive = true;
	pbdmaStatus = 0;
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 0);
}

TEST_F(OwedPreempt, NeverAskedDuringAHold)
{
	rst.holds = 1;
	EXPECT_EQ(tgm_recover_owed(&device), -1);
	EXPECT_EQ(writes, 0);
	EXPECT_EQ(rst.owed, 21) << "the hold's release asks";
}

TEST_F(OwedPreempt, TheReleasesStepRunsWithInterruptsOffAndTakesNoMutex)
{
	unsigned long flags = 0;
	local_irq_save(flags);
	EXPECT_EQ(owed_preempt(&device), TGM_OWED_ASK);
	local_irq_restore(flags);
	rst.owed = 21;
	aliveTakesMutex = true;
	EXPECT_THROW(tgm_recover_owed(&device), std::runtime_error)
		<< "the stand-ins see a mutex taken with interrupts off";
}
}  // namespace
