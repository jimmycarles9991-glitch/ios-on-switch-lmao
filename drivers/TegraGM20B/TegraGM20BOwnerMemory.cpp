// SPDX-License-Identifier: MIT
// Debug kernels' "memory by process" line (SwitchGpuOwnerMemory.h): every GPU connection of the accelerator, read through
// IOKit's client iterator, which copies the client set under IOKit's registry lock and keeps each client while it is
// read; no lock of the GPU driver's is taken.
#include <IOKit/IOLib.h>
#include "TegraGM20BUserClientInternal.h"

#if TGM_STATS
namespace
{
IOService *gProvider;              // the accelerator, which lives as long as the driver
constexpr uint32_t kEntries = 64;  // TegraGM20BUserClient.cpp kMaxConnectionsAll
constexpr uint32_t kShown = 8;     // a line of them fits the stats tick's 1 KiB
}  // namespace

void tgm_uc::tgm_owner_memory_provider(IOService *provider)
{
	__atomic_store_n(&gProvider, provider, __ATOMIC_RELEASE);
}

extern "C" unsigned long tgm_owner_memory(char *out, unsigned long size, uint64_t *imported)
{
	*imported = 0;
	IOService *provider = __atomic_load_n(&gProvider, __ATOMIC_ACQUIRE);
	OSIterator *clients = provider && size ? provider->getClientIterator() : nullptr;
	SwitchGpuOwnerEntry *entries = clients ? IONew(SwitchGpuOwnerEntry, kEntries) : nullptr;
	uint32_t n = 0;
	while (entries && n < kEntries) {
		OSObject *object = clients->getNextObject();
		if (!object)
			break;
		if (const auto *client = OSDynamicCast(AGXDeviceUserClient, object)) {
			entries[n] = client->ownerEntry();
			*imported += entries[n++].memory.bytes[SWITCH_GPU_OWNER_IMPORTED];
		}
	}
	const unsigned long written = n ? switch_gpu_owner_line(entries, n, kShown, out, size) : 0;
	if (entries)
		IODelete(entries, SwitchGpuOwnerEntry, kEntries);
	if (clients)
		clients->release();
	return written;
}
#else
void tgm_uc::tgm_owner_memory_provider(IOService *)
{
}
#endif
