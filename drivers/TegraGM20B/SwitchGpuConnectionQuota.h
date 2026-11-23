// SPDX-License-Identifier: MIT
// The GPU's connections, one fence page each (TegraGM20BUserClient.cpp kMaxConnectionsAll), shared out by process: one
// holds at most kPerTask, and the last eighth only a process holding IOGPUFamily's UI-critical marker
// (com.apple.private.graphics-restart-no-kill) takes, so no app can leave a respawned backboardd without one. Under the
// caller's lock: connections open and close, never on a submission.
#pragma once
#include <stdint.h>

class SwitchGpuConnectionQuota
{
public:
	static constexpr uint32_t kPerTask = 8, kTasks = 64;

	bool take(int pid, bool critical, uint32_t total)
	{
		if (held_ >= (critical ? total : total - total / 8))
			return false;
		Entry *free = nullptr;
		for (auto &entry : entries_) {
			if (entry.count && entry.pid == pid) {
				if (entry.count >= kPerTask)
					return false;
				++entry.count;
				++held_;
				return true;
			}
			if (!entry.count && !free)
				free = &entry;
		}
		if (!free)
			return false;
		*free = {pid, 1};
		++held_;
		return true;
	}

	void give(int pid)
	{
		for (auto &entry : entries_)
			if (entry.count && entry.pid == pid) {
				--entry.count;
				--held_;
				return;
			}
	}

private:
	struct Entry {
		int pid;
		uint32_t count;
	} entries_[kTasks] = {};

	uint32_t held_ = 0;
};
