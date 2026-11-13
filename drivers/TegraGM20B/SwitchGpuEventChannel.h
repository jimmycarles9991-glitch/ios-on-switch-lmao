// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLocks.h>
#include "SwitchGpuSharedEvent.h"

class SwitchGpuEventChannel
{
public:
	struct Record;
	bool initialize();
	void reset();
	void destroy();
	int prepare(switch_gpu_event_use *const *uses, uint32_t count,
		    switch_gpu_event_waits **waits, Record **record);
	int adopt(switch_gpu_event_waits *waits, Record **record);
	switch_gpu_event_waits *tail();
	void bind(Record *record, uint32_t seqno);
	void commit(Record **record);
	void abort(Record **record);
	uint64_t generation();
	int wait(uint32_t seqno, uint64_t deadline, uint64_t generation = 0);

private:
	static constexpr uint32_t kRecords = 256;
	IOLock *lock_ = nullptr;
	Record *records_ = nullptr, **last_ = &records_;
	switch_gpu_event_waits *tail_ = nullptr;
	uint32_t count_ = 0, failedSeq_ = 0;
	uint64_t generation_ = 1;
	int failure_ = 0;
};
