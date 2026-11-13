// SPDX-License-Identifier: MIT
#include "switch_gpu_recovery_stubs.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> calls;
int notifications, locks, resetError, updateError, waitError;
bool idle, preemptPending;

int cxid(nvkm_engn *engine, bool *group)
{
	*group = false;
	return engine->channel;
}

bool runqIdle(nvkm_runq *)
{
	calls.emplace_back("idle");
	return idle;
}

void preempt(nvkm_runl *)
{
}

int update(nvkm_runl *)
{
	calls.emplace_back("update");
	return updateError;
}

void clear(nvkm_runl *)
{
	calls.emplace_back("clear");
}

int wait(nvkm_runl *)
{
	calls.emplace_back("wait");
	return waitError;
}

bool pending(nvkm_runl *)
{
	calls.emplace_back("preempt");
	return preemptPending;
}

const nvkm_engn_func engineFunctions{cxid};
const nvkm_runq_func runqFunctions{runqIdle};
const nvkm_runl_func runlFunctions{preempt, update, clear, wait, pending};

class Recovery : public testing::Test
{
protected:
	nvkm_fifo fifo{};
	nvkm_chid ids{};
	nvkm_runl run{};
	nvkm_cgrp group{}, unrelated{};
	nvkm_chan first{}, second{}, other{};
	nvkm_engine engine{};
	nvkm_engn engn{};
	nvkm_runq queue{};

	void SetUp() override
	{
		calls.clear();
		notifications = locks = resetError = updateError = waitError = 0;
		idle = true;
		preemptPending = false;
		run.func = &runlFunctions;
		run.fifo = &fifo;
		run.chid = &ids;
		run.mutex = true;
		run.rc_pending = 1;
		run.cgrps = &group;
		group.runl = &run;
		group.rc = NVKM_CGRP_RC_PENDING;
		group.chans = &first;
		first.cgrp = &group;
		first.id = 0;
		first.next = &second;
		second.cgrp = &group;
		second.id = 1;
		unrelated.runl = &run;
		unrelated.chans = &other;
		other.cgrp = &unrelated;
		other.id = 2;
		other.errored = 1;
		ids.nr = 4;
		ids.data[0] = &first;
		ids.data[1] = &second;
		ids.data[2] = &other;
		engn.func = &engineFunctions;
		engn.runl = &run;
		engn.engine = &engine;
		run.engns = &engn;
		queue.func = &runqFunctions;
		run.runq[0] = &queue;
		run.runq_nr = 1;
	}
};
}

extern "C" {
void switch_gpu_recovery_abort(const char *condition)
{
	std::fprintf(stderr, "%s: %s\n", notifications ? "unsafe terminal" : "before terminal",
		     condition);
	std::abort();
}

nvkm_chan *nvkm_runl_chan_get_chid(nvkm_runl *run, int id, unsigned long *flags)
{
	auto *channel = run->chid->data[id];
	if (channel) {
		++locks;
		*flags = 1;
	}
	return channel;
}

nvkm_cgrp *nvkm_runl_cgrp_get_cgid(nvkm_runl *run, int id, unsigned long *flags)
{
	auto *channel = nvkm_runl_chan_get_chid(run, id, flags);
	return channel ? channel->cgrp : nullptr;
}

void nvkm_chan_put(nvkm_chan **channel, unsigned long)
{
	if (*channel)
		--locks;
	*channel = nullptr;
}

void nvkm_cgrp_put(nvkm_cgrp **group, unsigned long)
{
	if (*group)
		--locks;
	*group = nullptr;
}

void nvkm_chan_error(nvkm_chan *channel, bool shouldPreempt)
{
	EXPECT_FALSE(shouldPreempt);
	EXPECT_EQ(notifications, 0);
	channel->errored = 1;
	calls.emplace_back("disable");
}

void nvkm_chan_remove_locked(nvkm_chan *channel)
{
	EXPECT_EQ(notifications, 0);
	auto *group = channel->cgrp;
	group->chans = channel->next;
	if (!group->chans)
		group->runl->cgrps = group->next;
	calls.emplace_back("remove");
}

void tgm_recover_reset_hold(nvkm_engine *)
{
	calls.emplace_back("hold");
}

void tgm_recover_reset_release(nvkm_engine *)
{
	calls.emplace_back("release");
}

void tgm_recover_preempt_unblock(nvkm_runl *)
{
	calls.emplace_back("unblock");
}

int nvkm_engine_reset(nvkm_engine *)
{
	EXPECT_EQ(notifications, 0);
	calls.emplace_back("reset");
	return resetError;
}

void nvkm_runl_allow(nvkm_runl *)
{
	calls.emplace_back("allow");
}

void nvkm_event_ntfy(nvkm_event *, int, int type)
{
	EXPECT_EQ(type, NVKM_CHAN_EVENT_ERRORED);
	EXPECT_EQ(locks, 1);
	++notifications;
	calls.emplace_back("terminal");
}
}

TEST_F(Recovery, RemovedChannelsPublishOnlyAfterQuiescence)
{
	nvkm_runl_rc(&run);
	EXPECT_EQ(calls,
		  (std::vector<std::string>{"disable", "remove", "disable", "remove", "idle",
					    "hold", "reset", "release", "update", "clear", "allow",
					    "wait", "unblock", "preempt", "terminal", "terminal"}));
	EXPECT_EQ(notifications, 2);
	EXPECT_EQ(locks, 0);
	EXPECT_EQ(run.cgrps, nullptr);
	EXPECT_EQ(first.errored, 2);
	EXPECT_EQ(second.errored, 2);
	EXPECT_EQ(other.errored, 1);
}

TEST_F(Recovery, TerminalNotificationIsOnceEvenWhenRecoveryRunsAgain)
{
	nvkm_runl_rc(&run);
	calls.clear();
	engn.channel = -1;
	run.rc_pending = 1;
	nvkm_runl_rc(&run);
	EXPECT_EQ(notifications, 2);
	EXPECT_EQ(locks, 0);
}

TEST_F(Recovery, OtherRunlistChannelIsNeverRetired)
{
	nvkm_runl otherRun{};
	unrelated.runl = &otherRun;
	unrelated.rc = NVKM_CGRP_RC_RUNNING;
	nvkm_runl_rc(&run);
	EXPECT_EQ(other.errored, 1);
	EXPECT_EQ(notifications, 2);
}

TEST_F(Recovery, NoPendingRecoveryHasNoWork)
{
	run.rc_pending = 0;
	nvkm_runl_rc(&run);
	EXPECT_TRUE(calls.empty());
	EXPECT_EQ(notifications, 0);
}

TEST_F(Recovery, PbdmaTimeoutCannotPublishTerminal)
{
	idle = false;
	EXPECT_DEATH(nvkm_runl_rc(&run), "before terminal");
}

TEST_F(Recovery, EngineResetFailureCannotPublishTerminal)
{
	resetError = -1;
	EXPECT_DEATH(nvkm_runl_rc(&run), "before terminal");
}

TEST_F(Recovery, RunlistUpdateFailureCannotPublishTerminal)
{
	updateError = -1;
	EXPECT_DEATH(nvkm_runl_rc(&run), "before terminal");
}

TEST_F(Recovery, RunlistWaitFailureCannotPublishTerminal)
{
	waitError = -1;
	EXPECT_DEATH(nvkm_runl_rc(&run), "before terminal");
}

TEST_F(Recovery, PostedChannelPreemptMustFinishBeforeTerminal)
{
	nvkm_runl_func channelPreemptOnly = runlFunctions;
	channelPreemptOnly.preempt = nullptr;
	run.func = &channelPreemptOnly;
	preemptPending = true;
	EXPECT_DEATH(nvkm_runl_rc(&run), "before terminal");
}
