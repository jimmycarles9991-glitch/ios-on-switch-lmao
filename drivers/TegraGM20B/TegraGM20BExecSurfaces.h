// SPDX-License-Identifier: MIT
// A connection's host imports as its Execs see them, and the surfaces each of its channels' next Exec touches (TegraGM20BABI.h
// method 17, decided by TegraGM20BFences.h). The user client's lock is held for every call.
#ifndef TEGRA_GM20B_EXEC_SURFACES_H
#define TEGRA_GM20B_EXEC_SURFACES_H
#include <stdint.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BGpu.h"

namespace tgm_fences
{
constexpr uint32_t kChannels = 8;  // a connection's channel slots

class ExecSurfaces
{
public:
	// BoImport, before anything else: room for its entry. False: no memory (the import is refused).
	bool reserve();
	// A coherency entry's import once it has its handle. Handles only grow, which keeps the index sorted.
	void add(uint64_t handle, tgm_coh *coh, bool writable);
	void remove(uint64_t handle);
	// Every import and list goes (the connection closes); idempotent.
	void clear();
	// The list of channel slot `slot` goes (the channel was destroyed).
	void forget(uint32_t slot);

	// Method 17's record, apart from its channel: well formed, with `count` uses.
	static bool valid(const TegraGM20BAbiExecSurfaces *in);
	static bool validSubmit(const TegraGM20BAbiSubmit *in, uint32_t bytes,
				bool addressed = false);
	// The list for slot's channel's next Exec. False: no memory.
	bool set(uint32_t slot, const TegraGM20BAbiExecSurfaces *in);
	bool set(uint32_t slot, const TegraGM20BAbiSurfaceUse *uses, uint32_t count,
		 uint32_t flags);
	// Exec of slot's channel `chan`, before tgm_chan_exec: takes the slot's list and, when the Exec follows it, queues the
	// acquires on the listed surfaces' writers of other channels. 1 when listed, 0 unlisted, negative errno if any dependency could not be queued.
	int before(uint32_t slot, tgm_chan *chan, uint32_t pushes, bool dependencies = true);
	// Once it submitted `seqno` on channel `uid`: the imports it counts as a writer of.
	void after(bool listed, bool importsWritten, uint32_t uid, uint32_t seqno);
	// Between before() and after(): the TegraGM20BSurface* access the Exec's list gives `handle`, 0 if it lists none.
	uint32_t listedAccess(uint64_t handle) const;

private:
	struct Ref {
		uint64_t handle;
		tgm_coh *coh;
		bool writable;
	};

	struct List {
		bool given;  // set since the slot's last Exec
		uint32_t count, flags;
		uint64_t writes;  // bit i: uses[i] is TegraGM20BSurfaceWrite
		uint64_t reads;   // bit i: uses[i] is TegraGM20BSurfaceRead
		uint64_t handles[TegraGM20BMaxExecSurfaces];
	};

	static_assert(TegraGM20BMaxExecSurfaces <= 64, "List::writes");

	Ref *refs_ = nullptr;  // sorted by handle
	uint32_t nrefs_ = 0, cap_ = 0;
	List *lists_[kChannels] = {};
	const List *taken_ = nullptr;  // the Exec in progress's: from before() to after()

	const Ref *find(uint64_t handle) const;
	int follow(tgm_chan *chan, const List &list);
};
}  // namespace tgm_fences

#endif
