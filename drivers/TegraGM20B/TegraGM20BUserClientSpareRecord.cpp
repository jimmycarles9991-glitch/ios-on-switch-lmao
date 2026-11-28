// SPDX-License-Identifier: MIT
// The spare pool's demand record (SwitchGpuSpareRecord.h) in NVRAM: read by the refill worker's first run, set again from
// a thread call at each release (method 43) when it changed. IODTNVRAM only keeps a set variable in memory; SwitchNVRAM
// commits it with the bank at the next sleep or shutdown.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <string.h>
#include "SwitchGpuSpareRecord.h"
#include "TegraGM20BUserClientInternal.h"

namespace
{
const char kRecordKey[] = "switch-gpu-spares";

IORegistryEntry *nvram()
{
	return IORegistryEntry::fromPath("/options", gIODTPlane);
}

// The previous boot's record, or 0 bytes
uint32_t readRecord(uint8_t (&out)[SWITCH_GPU_SPARE_RECORD_BYTES])
{
	IORegistryEntry *options = nvram();
	OSObject *object = options ? options->copyProperty(kRecordKey) : nullptr;
	OSSafeReleaseNULL(options);
	const OSData *data = OSDynamicCast(OSData, object);
	uint32_t length = data ? data->getLength() : 0;
	if (length > sizeof(out))
		length = 0;
	if (length)
		memcpy(out, data->getBytesNoCopy(), length);
	OSSafeReleaseNULL(object);
	return length;
}
}  // namespace

namespace tgm_uc
{

void sparesSeed(SparePool *p)
{
	uint8_t record[SWITCH_GPU_SPARE_RECORD_BYTES];
	const uint32_t length = readRecord(record);
	IOLockLock(p->lock);
	if (switch_gpu_spare_record_seed(p->books, record, length, SWITCH_GPU_SPARE_CAP_BYTES)) {
		memcpy(p->record, record, length);
		p->recordLength = length;
#if TGM_DEBUG_PROBES
		for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; s++)
			if (p->books.used[s] == SWITCH_GPU_SPARE_SEEDED)
				p->seeded[s] = p->books.depth[s];
#endif
	}
	IOLockUnlock(p->lock);
}

void spareRecordWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	uint8_t record[SWITCH_GPU_SPARE_RECORD_BYTES];
	IOLockLock(p->lock);
	const uint32_t length = switch_gpu_spare_record_encode(p->books, record);
	const bool changed = length != p->recordLength || memcmp(record, p->record, length) != 0;
	if (changed) {
		memcpy(p->record, record, length);
		p->recordLength = length;
	}
	IOLockUnlock(p->lock);
	if (!changed)
		return;
	IORegistryEntry *options = nvram();
	OSData *data = OSData::withBytes(record, length);
	const bool set = options && data && options->setProperty(kRecordKey, data);
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(options);
	static bool told;
	if (!set && !__atomic_exchange_n(&told, true, __ATOMIC_RELAXED))
		IOLog("tegra-gm20b: the spare demand record (%u bytes) was not set in NVRAM (a "
		      "full "
		      "partition refuses it; nothing else is removed): the next boot's first asks "
		      "miss; "
		      "further ones not logged\n",
		      length);
}
}  // namespace tgm_uc

#if TGM_DEBUG_PROBES
// " spare_seed_<kind>_<KiB>=<depth>" for each class the previous boot's record seeded
extern "C" void tgm_spare_seeds_line(char *line, unsigned long size)
{
	tgm_uc::SparePool *p = tgm_uc::sparesExisting();
	if (!p)
		return;
	IOLockLock(p->lock);
	size_t used = strlen(line);
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS && used < size; s++) {
		if (!p->seeded[s])
			continue;
		snprintf(line + used, size - used, " spare_seed_%s_%llu=%u",
			 switch_gpu_spare_slot_kind(s) == SWITCH_GPU_SPARE_WC ? "wc" : "cached",
			 static_cast<unsigned long long>(switch_gpu_spare_slot_bytes(s) >> 10),
			 p->seeded[s]);
		used = strlen(line);
	}
	IOLockUnlock(p->lock);
}
#endif
