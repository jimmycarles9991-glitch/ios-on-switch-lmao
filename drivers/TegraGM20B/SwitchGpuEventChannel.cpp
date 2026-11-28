// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuEventChannel.h"

struct SwitchGpuEventChannel::Record {
	Record *next;
	switch_gpu_event_waits *waits;
	uint32_t seqno;
	bool linked;
};

bool SwitchGpuEventChannel::initialize()
{
	lock_ = IOLockAlloc();
	return lock_ != nullptr;
}

switch_gpu_event_waits *SwitchGpuEventChannel::tail()
{
	IOLockLock(lock_);
	auto *result = tail_;
	if (result)
		switch_gpu_event_waits_retain(result);
	IOLockUnlock(lock_);
	return result;
}

int SwitchGpuEventChannel::prepare(switch_gpu_event_use *const *uses, uint32_t count,
				   switch_gpu_event_waits **waits, Record **record)
{
	*waits = nullptr;
	*record = nullptr;
	int result = switch_gpu_event_waits_prepare(uses, count, waits);
	if (!result)
		result = adopt(*waits, record);
	if (result)
		switch_gpu_event_waits_abort(waits);
	return result;
}

int SwitchGpuEventChannel::adopt(switch_gpu_event_waits *waits, Record **record)
{
	*record = nullptr;
	IOLockLock(lock_);
	Record *retired = nullptr;
	while (records_) {
		const int status = switch_gpu_event_waits_status(records_->waits);
		if (!status)
			break;
		auto *head = records_;
		records_ = head->next;
		head->next = retired;
		retired = head;
		--count_;
		if (status < 0 && !failure_) {
			failedSeq_ = head->seqno;
			__atomic_store_n(&failure_, status, __ATOMIC_RELEASE);
		}
	}
	if (!records_) {
		last_ = &records_;
		__atomic_store_n(&active_, 0u, __ATOMIC_RELEASE);
	}
	switch_gpu_event_waits *previous = nullptr;
	const bool full = count_ >= kRecords;
	if (!full) {
		++count_;
		previous = tail_;
		if (previous)
			switch_gpu_event_waits_retain(previous);
	}
	IOLockUnlock(lock_);
	while (retired) {
		auto *head = retired;
		retired = head->next;
		switch_gpu_event_waits_release(&head->waits);
		IODelete(head, Record, 1);
	}
	if (full)
		return -28;
	auto *made = IONew(Record, 1);
	int result = made ? 0 : -12;
	if (!result && previous)
		result = switch_gpu_event_waits_inherit(waits, previous);
	switch_gpu_event_waits_release(&previous);
	if (result) {
		IODelete(made, Record, 1);
		IOLockLock(lock_);
		--count_;
		IOLockUnlock(lock_);
		return result;
	}
	switch_gpu_event_waits_retain(waits);
	*made = {nullptr, waits, 0, false};
	*record = made;
	return 0;
}

void SwitchGpuEventChannel::bind(Record *record, uint32_t seqno)
{
	IOLockLock(lock_);
	if (!record->linked) {
		record->seqno = seqno;
		record->linked = true;
		*last_ = record;
		last_ = &record->next;
		__atomic_store_n(&active_, 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock_);
}

void SwitchGpuEventChannel::commit(Record **owned)
{
	if (!owned || !*owned)
		return;
	auto *record = *owned;
	*owned = nullptr;
	IOLockLock(lock_);
	auto *previous = tail_;
	tail_ = record->waits;
	switch_gpu_event_waits_retain(tail_);
	IOLockUnlock(lock_);
	switch_gpu_event_waits_release(&previous);
}

void SwitchGpuEventChannel::abort(Record **owned)
{
	if (!owned || !*owned)
		return;
	auto *record = *owned;
	*owned = nullptr;
	IOLockLock(lock_);
	if (record->linked) {
		auto **at = &records_;
		while (*at && *at != record)
			at = &(*at)->next;
		if (*at) {
			*at = record->next;
			if (last_ == &record->next)
				last_ = at;
		}
		if (!records_)
			__atomic_store_n(&active_, 0u, __ATOMIC_RELEASE);
	}
	--count_;
	IOLockUnlock(lock_);
	switch_gpu_event_waits_release(&record->waits);
	IODelete(record, Record, 1);
}

uint64_t SwitchGpuEventChannel::generation()
{
	return __atomic_load_n(&generation_, __ATOMIC_ACQUIRE);
}

int SwitchGpuEventChannel::wait(uint32_t seqno, uint64_t deadline, uint64_t generation)
{
	if (!generation)
		generation = this->generation();
	const uint32_t active = __atomic_load_n(&active_, __ATOMIC_ACQUIRE);
	const int fail = __atomic_load_n(&failure_, __ATOMIC_ACQUIRE);
	if (__atomic_load_n(&generation_, __ATOMIC_ACQUIRE) != generation)
		return -19;
	if (!active && !fail)
		return 1;
	for (;;) {
		IOLockLock(lock_);
		if (generation_ != generation) {
			IOLockUnlock(lock_);
			return -19;
		}
		auto *record = records_;
		const bool eligible = record && int32_t(seqno - record->seqno) >= 0;
		const int failure = failure_ && int32_t(seqno - failedSeq_) >= 0 ? failure_ : 0;
		auto *batch = eligible ? record->waits : nullptr;
		if (batch)
			switch_gpu_event_waits_retain(batch);
		IOLockUnlock(lock_);
		if (!batch)
			return failure ? failure : 1;
		const int result = switch_gpu_event_waits_wait(batch, deadline);
		Record *retired = nullptr;
		IOLockLock(lock_);
		if (result && result != -110 && generation_ == generation && records_ == record &&
		    record->waits == batch) {
			records_ = record->next;
			if (!records_)
				last_ = &records_;
			--count_;
			if (result < 0 && !failure_) {
				failedSeq_ = record->seqno;
				__atomic_store_n(&failure_, result, __ATOMIC_RELEASE);
			}
			if (!records_)
				__atomic_store_n(&active_, 0u, __ATOMIC_RELEASE);
			retired = record;
		}
		IOLockUnlock(lock_);
		if (retired) {
			switch_gpu_event_waits_release(&retired->waits);
			IODelete(retired, Record, 1);
		}
		switch_gpu_event_waits_release(&batch);
		if (result <= 0)
			return result;
	}
}

void SwitchGpuEventChannel::reset()
{
	if (!lock_)
		return;
	IOLockLock(lock_);
	auto *records = records_;
	auto *previous = tail_;
	records_ = nullptr;
	last_ = &records_;
	tail_ = nullptr;
	count_ = 0;
	failedSeq_ = 0;
	__atomic_store_n(&failure_, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&active_, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&generation_, generation_ + 1, __ATOMIC_RELEASE);
	IOLockUnlock(lock_);
	while (records) {
		auto *record = records;
		records = record->next;
		switch_gpu_event_waits_release(&record->waits);
		IODelete(record, Record, 1);
	}
	switch_gpu_event_waits_release(&previous);
}

void SwitchGpuEventChannel::destroy()
{
	reset();
	if (lock_)
		IOLockFree(lock_);
	lock_ = nullptr;
}
