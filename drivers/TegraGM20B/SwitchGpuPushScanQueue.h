// SPDX-License-Identifier: MIT
// Debug kernels: the push scan's submissions waiting for the thread that reads them. A submission's thread only records
// where its pushes' words lie (in the GPU pool's kernel mapping, which lives as long as the kernel) and returns; the
// render server waits for that thread (waitUntilScheduled), and reading 36-57 KiB of a compositor frame's pushes through
// the write-combined mapping cost it every 4th submission on boot 21. The caller serialises reserve/publish and
// take/done under its lock; a record taken is read outside it, since reserve never hands out a slot before its done.
// One reader at a time: take hands the oldest record to every caller until its done, so a second reader would read it
// twice and its two dones would free a slot still being read.
#pragma once
#include <stdint.h>
#include <string.h>

struct switch_gpu_scan_words {
	const uint32_t *words;  // null: not read (outside the pool, or past the record)
	uint32_t dwords;
};

template <uint32_t Records, uint32_t Pushes> struct SwitchGpuPushScanQueue {
	struct Record {
		char owner[17];
		bool rawMacros, compositor;
		uint32_t count;
		// its channel's fence word (an offset in the fence pool), that page's owner then, and its last seqno
		uint32_t fence, fenceOwner, seqno;
		switch_gpu_scan_words push[Pushes];
	};

	Record records[Records];
	uint32_t head = 0, tail = 0;
	uint64_t dropped = 0;  // submissions not read: every slot held one not read yet
	uint64_t late = 0;     // read once their submission may have completed: not counted

	Record *reserve()
	{
		if (head - tail == Records) {
			++dropped;
			return nullptr;
		}
		return &records[head % Records];
	}

	void publish()
	{
		++head;
	}

	Record *take()
	{
		return head == tail ? nullptr : &records[tail % Records];
	}

	void done()
	{
		++tail;
	}
};

// A record of `count` pushes from `owner`: `view(i)` gives push i's words in the pool, or null
template <typename Record, typename View>
inline void switch_gpu_scan_fill(Record &r, const char *owner, bool rawMacros, uint32_t count,
				 uint32_t capacity, View &&view)
{
	strncpy(r.owner, owner, sizeof(r.owner) - 1);
	r.owner[sizeof(r.owner) - 1] = 0;
	r.rawMacros = rawMacros;
	r.count = count;
	for (uint32_t i = 0; i < count && i < capacity; i++)
		r.push[i] = view(i);
}

// The thread call's read of a record. Its words are the submission's only until the submission completes; its process
// may then write them again or free them, so a read is kept only when `passed(r)` (the submission may have completed)
// holds neither before nor after it.
template <typename Record, typename Passed, typename Read>
inline bool switch_gpu_scan_record(const Record &r, Passed &&passed, Read &&read)
{
	if (passed(r))
		return false;
	read(r);
	return !passed(r);
}
