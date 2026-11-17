/* SPDX-License-Identifier: MIT */
#pragma once
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "drivers/TegraGM20B/TegraGM20BHost.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

using u32 = uint32_t;
using atomic_t = int;

inline int atomic_read(const atomic_t *value)
{
	return *value;
}

struct nvkm_device;
struct nvkm_runl;
struct nvkm_cgrp;
struct nvkm_chan;

struct nvkm_engine {
	struct {
		nvkm_device *device;
	} subdev;

	bool loading = false;
};

struct nvkm_fifo {
	nvkm_engine engine;
};

struct nvkm_cgrp {
	nvkm_runl *runl;
	atomic_t rc;
};

struct nvkm_chan {
	nvkm_cgrp *cgrp;
	atomic_t errored;
};

struct nvkm_engn_func {
	int (*cxid)(void);
};

struct nvkm_engn {
	const nvkm_engn_func *func;
	nvkm_engn *next;
	int id;
	nvkm_engine *engine;
};

struct nvkm_runq {
	int id;
};

struct nvkm_runl {
	nvkm_fifo *fifo;
	nvkm_engn *engns;
	nvkm_runq *runq[1];
	int runq_nr;
	bool mutex;
	atomic_t rc_pending;
	std::array<nvkm_chan *, 16> channels{};
	std::array<nvkm_cgrp *, 16> groups{};
};

struct nvkm_device {
	nvkm_fifo *fifo;

	struct {
		int lock;
	} intr;
};

#define NVKM_CGRP_RC_NONE 0
#define NVKM_CGRP_RC_PENDING 1
#define NVKM_CGRP_RC_RUNNING 2
#define mutex_is_locked(p) (*(p))
#define BUG_ON(value)                 \
	do {                          \
		if (value)            \
			std::abort(); \
	} while (0)
#define nvkm_runl_foreach_engn(e, r) for ((e) = (r)->engns; (e); (e) = (e)->next)
#define nvkm_runl_foreach_engn_cond(e, r, cond)      \
	for ((e) = (r)->engns; (e); (e) = (e)->next) \
		if (cond)

namespace switch_recovery_phase
{
struct Hardware {
	struct Reset {
		int engine, channel;
	};

	u32 preempt = 0, pbdma = 0, method = 0, data = 0;
	std::array<u32, 2> engines{};
	std::array<unsigned, 2> engineReads{};
	std::array<bool, 2> engineUnreadable{};
	std::array<bool, 16> unowned{};
	std::vector<u32> writes;
	std::vector<Reset> resets;
	int locks = 0, lookups = 0, unblocks = 0;
	nvkm_runl *runl = nullptr;
};

inline Hardware *hardware = nullptr;

inline void lock(unsigned long *flags)
{
	EXPECT_TRUE(hardware->runl->mutex);
	EXPECT_EQ(hardware->locks, 0);
	*flags = 0;
	hardware->locks++;
}

inline void unlock(unsigned long flags)
{
	(void)flags;
	EXPECT_EQ(hardware->locks, 1);
	EXPECT_EQ(hardware->lookups, 0);
	hardware->locks--;
}
}

#define spin_lock_irqsave(lock_pointer, flags) \
	((void)(lock_pointer), switch_recovery_phase::lock(&(flags)))
#define spin_unlock_irqrestore(lock_pointer, flags) \
	((void)(lock_pointer), switch_recovery_phase::unlock(flags))

inline u32 nvkm_rd32(nvkm_device *, u32 address)
{
	auto &hw = *switch_recovery_phase::hardware;
	EXPECT_TRUE(hw.runl->mutex);
	if (address == PFIFO_PREEMPT) {
		if (!hw.locks)
			hw.unblocks++;
		return hw.preempt;
	}
	if (address == PBDMA_STATUS(0))
		return hw.pbdma;
	if (address == PBDMA_METHOD0(0))
		return hw.method;
	if (address == PBDMA_DATA0(0))
		return hw.data;
	for (unsigned i = 0; i < hw.engines.size(); i++)
		if (address == PFIFO_ENG_STATUS(i)) {
			EXPECT_FALSE(hw.engineUnreadable[i])
				<< "software engine has no PFIFO context register";
			++hw.engineReads[i];
			return hw.engines[i];
		}
	ADD_FAILURE() << "unexpected MMIO read " << address;
	return 0;
}

inline void nvkm_wr32(nvkm_device *, u32 address, u32 value)
{
	auto &hw = *switch_recovery_phase::hardware;
	EXPECT_EQ(address, PFIFO_PREEMPT);
	EXPECT_EQ(hw.locks, 1);
	EXPECT_EQ(hw.lookups, 0);
	EXPECT_TRUE(hw.runl->mutex);
	EXPECT_EQ(hw.preempt & PFIFO_PREEMPT_PENDING, 0u);
	hw.writes.push_back(value);
	hw.preempt = value | PFIFO_PREEMPT_PENDING;
}

inline nvkm_chan *nvkm_runl_chan_get_chid(nvkm_runl *runl, int id, unsigned long *flags)
{
	auto &hw = *switch_recovery_phase::hardware;
	EXPECT_TRUE(runl->mutex);
	if (id < 0 || static_cast<unsigned>(id) >= runl->channels.size())
		return nullptr;
	auto *chan = runl->channels[id];
	if (chan) {
		hw.lookups++;
		*flags = 0;
	}
	return chan;
}

inline nvkm_cgrp *nvkm_runl_cgrp_get_cgid(nvkm_runl *runl, int id, unsigned long *flags)
{
	auto &hw = *switch_recovery_phase::hardware;
	EXPECT_TRUE(runl->mutex);
	if (id < 0 || static_cast<unsigned>(id) >= runl->groups.size())
		return nullptr;
	auto *group = runl->groups[id];
	if (group) {
		hw.lookups++;
		*flags = 0;
	}
	return group;
}

inline void nvkm_chan_put(nvkm_chan **chan, unsigned long)
{
	if (*chan)
		switch_recovery_phase::hardware->lookups--;
	*chan = nullptr;
}

inline void nvkm_cgrp_put(nvkm_cgrp **group, unsigned long)
{
	if (*group)
		switch_recovery_phase::hardware->lookups--;
	*group = nullptr;
}

inline bool nvkm_engine_chsw_load(nvkm_engine *engine)
{
	return engine->loading;
}

inline bool chan_alive(nvkm_fifo *, u32 id)
{
	auto *runl = switch_recovery_phase::hardware->runl;
	EXPECT_TRUE(runl->mutex);
	if (id >= runl->channels.size() || !runl->channels[id])
		return false;
	const auto *chan = runl->channels[id];
	return !chan->errored && chan->cgrp->rc == NVKM_CGRP_RC_NONE;
}

inline int tgm_chid_unowned(int id)
{
	auto &hw = *switch_recovery_phase::hardware;
	return id >= 0 && static_cast<unsigned>(id) < hw.unowned.size() && hw.unowned[id];
}

inline void tgm_engine_reset_locked(nvkm_runl *runl, nvkm_engn *engine, int id, const char *)
{
	auto &hw = *switch_recovery_phase::hardware;
	EXPECT_EQ(hw.locks, 0);
	EXPECT_EQ(hw.lookups, 0);
	EXPECT_TRUE(runl->mutex);
	hw.resets.push_back({engine->id, id});
	hw.engines[engine->id] = 0;
}
