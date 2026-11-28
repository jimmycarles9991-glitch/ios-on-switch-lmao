// TegraGM20BRamfcOrder.h over a model of the host's RAMFC: a loaded channel's RAMFC lives in the PBDMA and goes back to
// memory when the host switches the channel out, and that save can land after ENG_STATUS and PBDMA_CHANNEL already
// show the channel gone. A timeslice written into memory before the save is overwritten by it; the next load then
// runs the channel with the old one.
#include <cerrno>
#include <cstdint>
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BGrPreempt.h"
#include "drivers/TegraGM20B/TegraGM20BRamfcOrder.h"

namespace
{
constexpr uint32_t kDefault = 0x10003080, kHigh = 0x100080c3;

struct Host {
	uint32_t memory = kDefault; /* RAMFC 0xf8 in the instance block */
	uint32_t pbdma = kDefault;  /* the loaded channel's copy */
	bool loaded = true;
	bool saving =
		false; /* switched out on ENG_STATUS/PBDMA_CHANNEL, the save still in flight */
	bool enabled = true;
	bool lockHeld = false;
	bool preemptHangs = false;
	int writes = 0;

	void saveLands()
	{
		if (loaded)
			memory = pbdma;
		loaded = saving = false;
	}

	/* a later job: the host loads RAMFC from memory and saves it back when it is done */
	void runsAJob()
	{
		if (saving)
			saveLands();
		if (!enabled)
			return;
		pbdma = memory;
		loaded = true;
		saveLands();
	}
};

Host gHost;

int own(void *)
{
	if (gHost.lockHeld)
		return 0;
	gHost.lockHeld = true;
	return 1;
}

void release(void *)
{
	gHost.lockHeld = false;
}

void disable(void *)
{
	gHost.enabled = false;
}

void enable(void *)
{
	gHost.enabled = true;
}

int preempt(void *)
{
	if (gHost.preemptHangs)
		return -ETIMEDOUT;
	gHost.saveLands();
	return 0;
}

void write(void *)
{
	gHost.memory = kHigh;
	++gHost.writes;
}

const tgm_ramfc_steps kSteps = {nullptr, own, release, disable, enable, preempt, write};

TEST(Gm20bRamfcOrder, ALevelChangeWrittenWhileTheHostsSaveIsInFlightHolds)
{
	gHost = Host{};
	gHost.saving = true;
	EXPECT_EQ(tgm_ramfc_write_quiesced(&kSteps), 0);
	gHost.runsAJob();
	EXPECT_EQ(gHost.memory, kHigh)
		<< "the host's save of the old RAMFC landed on the new timeslice";
	EXPECT_EQ(gHost.pbdma, kHigh);
	EXPECT_TRUE(gHost.enabled);
	EXPECT_FALSE(gHost.lockHeld);
}

TEST(Gm20bRamfcOrder, ALevelChangeOnAnIdleChannelHolds)
{
	gHost = Host{};
	gHost.loaded = false;
	EXPECT_EQ(tgm_ramfc_write_quiesced(&kSteps), 0);
	gHost.runsAJob();
	EXPECT_EQ(gHost.memory, kHigh);
}

TEST(Gm20bRamfcOrder, ARunlistLockHeldElsewhereLeavesEverythingAsItWas)
{
	gHost = Host{};
	gHost.lockHeld = true;
	EXPECT_EQ(tgm_ramfc_write_quiesced(&kSteps), -EBUSY);
	EXPECT_EQ(gHost.writes, 0);
	EXPECT_TRUE(gHost.enabled);
}

TEST(Gm20bRamfcOrder, APreemptThatNeverCompletesWritesNothingAndReenables)
{
	gHost = Host{};
	gHost.preemptHangs = true;
	EXPECT_EQ(tgm_ramfc_write_quiesced(&kSteps), -ETIMEDOUT);
	EXPECT_EQ(gHost.writes, 0);
	EXPECT_TRUE(gHost.enabled);
	EXPECT_FALSE(gHost.lockHeld);
}

struct GrCtxImage {
	uint32_t memory = TGM_GR_CTX_PREEMPT_WFI;
	uint32_t fecs = TGM_GR_CTX_PREEMPT_WFI;
	uint32_t last_offset = 0;
	int reads = 0;
	int writes = 0;
};

GrCtxImage gGrCtx;

uint32_t grRead(void *, uint32_t offset)
{
	gGrCtx.last_offset = offset;
	++gGrCtx.reads;
	return gGrCtx.memory;
}

void grWrite(void *, uint32_t offset, uint32_t value)
{
	gGrCtx.last_offset = offset;
	++gGrCtx.writes;
	gGrCtx.memory = value;
}

int grPreempt(void *arg)
{
	const int ret = preempt(arg);
	if (!ret)
		gGrCtx.memory = gGrCtx.fecs;
	return ret;
}

const tgm_ramfc_steps kGrSteps = {nullptr, own, release, disable, enable, grPreempt, nullptr};
const tgm_gr_preempt_io kGrIo = {nullptr, grRead, grWrite};

TEST(Gm20bGrPreempt, QuiescedWriteUpdatesCtxswOption0x60AfterSaveAndTogglesBack)
{
	gHost = Host{};
	gHost.saving = true;
	gGrCtx = GrCtxImage{};
	uint32_t old_mode = 99, readback = 99;

	EXPECT_EQ(tgm_gr_preempt_write_quiesced(&kGrSteps, &kGrIo, TGM_GR_CTX_PREEMPT_CTA,
						&old_mode, &readback),
		  0);
	EXPECT_EQ(gGrCtx.last_offset, TGM_GR_CTX_PREEMPT_OPTIONS);
	EXPECT_EQ(old_mode, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_EQ(readback, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(gGrCtx.memory, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_TRUE(gHost.enabled);
	EXPECT_FALSE(gHost.lockHeld);

	gGrCtx.fecs = gGrCtx.memory;
	gHost.loaded = true;
	EXPECT_EQ(tgm_gr_preempt_write_quiesced(&kGrSteps, &kGrIo, TGM_GR_CTX_PREEMPT_WFI,
						&old_mode, &readback),
		  0);
	EXPECT_EQ(old_mode, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(readback, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_EQ(gGrCtx.memory, TGM_GR_CTX_PREEMPT_WFI);
}

TEST(Gm20bGrPreempt, InvalidModeAndPreemptFailureTouchNothing)
{
	gHost = Host{};
	gGrCtx = GrCtxImage{};
	uint32_t old_mode = 7, readback = 7;

	EXPECT_EQ(tgm_gr_preempt_write_quiesced(&kGrSteps, &kGrIo, 2, &old_mode, &readback),
		  -EINVAL);
	EXPECT_EQ(gGrCtx.writes, 0);
	EXPECT_EQ(gGrCtx.reads, 0);
	EXPECT_TRUE(gHost.enabled);
	EXPECT_FALSE(gHost.lockHeld);

	gHost.preemptHangs = true;
	EXPECT_EQ(tgm_gr_preempt_write_quiesced(&kGrSteps, &kGrIo, TGM_GR_CTX_PREEMPT_CTA,
						&old_mode, &readback),
		  -ETIMEDOUT);
	EXPECT_EQ(gGrCtx.writes, 0);
	EXPECT_EQ(gGrCtx.memory, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_TRUE(gHost.enabled);
	EXPECT_FALSE(gHost.lockHeld);
}
}  // namespace
