// TegraGM20BRamfcOrder.h over a model of the host's RAMFC: a loaded channel's RAMFC lives in the PBDMA and goes back to
// memory when the host switches the channel out, and that save can land after ENG_STATUS and PBDMA_CHANNEL already
// show the channel gone. A timeslice written into memory before the save is overwritten by it; the next load then
// runs the channel with the old one.
#include <cerrno>
#include <cstdint>
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BRamfcOrder.h"

namespace
{
constexpr uint32_t kDefault = 0x10003080, kHigh = 0x10005080;

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
}  // namespace
