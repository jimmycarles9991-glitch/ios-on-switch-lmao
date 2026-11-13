// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"
#include "SwitchGpuEventWord.h"

namespace
{
SwitchGpuEventRecord *disposals;
}

void switch_gpu_event_dispose(SwitchGpuEventRecord *record)
{
	auto *head = __atomic_load_n(&disposals, __ATOMIC_RELAXED);
	do {
		record->disposeNext = head;
	} while (!__atomic_compare_exchange_n(&disposals, &head, record, false, __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED));
	switch_gpu_events_schedule();
}

void switch_gpu_events_dispose()
{
	while (auto *head = __atomic_exchange_n(&disposals, nullptr, __ATOMIC_ACQUIRE)) {
		while (head) {
			auto *record = head;
			head = record->disposeNext;
			if (record->kind == SwitchEventSignal) {
				auto *signal = static_cast<switch_gpu_event_signal *>(record);
				if (signal->bound)
					switch_gpu_backing_token_release(&signal->token);
				if (signal->dependencies)
					switch_gpu_event_waits_drop(signal->dependencies);
				switch_gpu_event_word_release(&signal->word);
				IODelete(signal, switch_gpu_event_signal, 1);
			} else if (record->kind == SwitchEventUse) {
				auto *use = static_cast<switch_gpu_event_use *>(record);
				switch_gpu_backing_token_release(&use->token);
				if (use->authority)
					switch_gpu_event_signal_drop(use->authority);
				else if (use->batchAuthority)
					switch_gpu_event_waits_drop(use->batchAuthority);
				IODelete(use, switch_gpu_event_use, 1);
			} else {
				auto *batch = static_cast<switch_gpu_event_waits *>(record);
				for (uint32_t i = 0; i < batch->count; ++i)
					switch_gpu_event_use_release(&batch->uses[i]);
				if (batch->bound)
					switch_gpu_backing_token_release(&batch->consumer);
				if (batch->previous)
					switch_gpu_event_waits_drop(batch->previous);
				if (batch->uses && batch->uses != batch->inlineUses)
					IODelete(batch->uses, switch_gpu_event_use *,
						 batch->capacity);
				IODelete(batch, switch_gpu_event_waits, 1);
			}
			switch_gpu_events_unreserve();
		}
	}
}
