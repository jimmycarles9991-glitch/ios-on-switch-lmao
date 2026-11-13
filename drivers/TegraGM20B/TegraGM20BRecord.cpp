// SPDX-License-Identifier: MIT
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>
#include <libkern/c++/OSObject.h>
#include <switch/memory_attachment.h>
#include "TegraGM20BRecord.h"
#include "TegraGM20BViews.h"

extern "C" IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *surface);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchGpuSurfaceRecord : public OSObject
{
	OSDeclareDefaultStructors(SwitchGpuSurfaceRecord);

public:
	uint32_t imported;  // atomic
	tgm_coh *deferred;  // the coherency registry's lock

	bool init() override
	{
		imported = 0;
		deferred = nullptr;
		return OSObject::init();
	}

	void free() override
	{
		tgm_coherency_record_gone(reinterpret_cast<tgm_record *>(this));
		OSObject::free();
	}
};

OSDefineMetaClassAndStructors(SwitchGpuSurfaceRecord, OSObject);

namespace
{
SwitchGpuSurfaceRecord *copyRecord(IOMemoryDescriptor *memory)
{
	OSObject *object = SwitchMemoryAttachment::copyGpu(memory);
	auto *record = OSDynamicCast(SwitchGpuSurfaceRecord, object);
	if (object && !record)
		object->release();
	return record;
}

// Unretained: the descriptor's own reference keeps the record while the memory lives
SwitchGpuSurfaceRecord *recordOf(IOMemoryDescriptor *memory)
{
	SwitchGpuSurfaceRecord *record = copyRecord(memory);
	if (!record) {
		auto *made = OSTypeAlloc(SwitchGpuSurfaceRecord);
		if (made && !made->init())
			OSSafeReleaseNULL(made);
		if (made && !SwitchMemoryAttachment::replaceGpu(memory, nullptr, made))
			OSSafeReleaseNULL(made);
		record = made ? made : copyRecord(memory);
	}
	if (record)
		record->release();
	return record;
}
}  // namespace

extern "C" struct tgm_record *tgm_record_of(void *surface)
{
	IOMemoryDescriptor *memory =
		surface ? tegra_vic_ios_memory(static_cast<OSObject *>(surface)) : nullptr;
	return reinterpret_cast<tgm_record *>(memory ? recordOf(memory) : nullptr);
}

extern "C" int tgm_record_first_import(struct tgm_record *record)
{
	auto *r = reinterpret_cast<SwitchGpuSurfaceRecord *>(record);
	return static_cast<int>(__atomic_exchange_n(&r->imported, 1u, __ATOMIC_ACQ_REL) == 0);
}

extern "C" void tgm_record_park(struct tgm_record *record, struct tgm_coh *c)
{
	reinterpret_cast<SwitchGpuSurfaceRecord *>(record)->deferred = c;
}

extern "C" struct tgm_coh *tgm_record_take(struct tgm_record *record)
{
	auto *r = reinterpret_cast<SwitchGpuSurfaceRecord *>(record);
	tgm_coh *c = r->deferred;
	r->deferred = nullptr;
	return c;
}

// TegraVIC's surface acquisition (VIC's jobs, NVJPG's, NVENC's): none of them reads a deferred surface's texels
extern "C" void tegra_gm20b_surface_engine_rows(OSObject *surface)
{
	IOMemoryDescriptor *memory = surface ? tegra_vic_ios_memory(surface) : nullptr;
	SwitchGpuSurfaceRecord *record = memory ? copyRecord(memory) : nullptr;
	if (!record)
		return;
	tgm_coherency_engine_rows(reinterpret_cast<tgm_record *>(record));
	record->release();
}

extern "C" int tgm_record_is_surface(const struct tgm_record *record, uint32_t id)
{
	OSObject *surface = tgm_surface_lookup(id, current_task());
	IOMemoryDescriptor *memory = surface ? tegra_vic_ios_memory(surface) : nullptr;
	SwitchGpuSurfaceRecord *found = memory ? copyRecord(memory) : nullptr;
	const int same = static_cast<int>(found != nullptr &&
					  reinterpret_cast<const tgm_record *>(found) == record);
	if (found)
		found->release();
	if (surface)
		surface->release();
	return same;
}
