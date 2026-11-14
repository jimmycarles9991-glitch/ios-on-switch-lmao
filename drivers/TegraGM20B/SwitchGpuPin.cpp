// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <string.h>
#include "SwitchGpuPin.h"
#include "TegraGM20BGpu.h"
void switch_gpu_cached_retire(SwitchGpuCachedBuffer buffer, uint64_t *pages, uint32_t count,
			      switch_gpu_cached_mapping *mapping);

namespace
{
struct Pin {
	IOMemoryDescriptor *memory;
	bool prepared;
};

void release_pin(void *owner)
{
	auto *pin = static_cast<Pin *>(owner);
	if (pin->prepared)
		pin->memory->complete();
	pin->memory->release();
	IODelete(pin, Pin, 1);
}

struct Chunks {
	uint64_t *pages;
	uint32_t count;
};

struct Cached {
	switch_gpu_cached_mapping *mapping;
	SwitchGpuCachedBuffer buffer;
	uint64_t *pages;
	uint32_t count;
};

void release_cached(void *owner)
{
	auto *cached = static_cast<Cached *>(owner);
	switch_gpu_cached_retire(cached->buffer, cached->pages, cached->count, cached->mapping);
	IODelete(cached, Cached, 1);
}

void release_chunks(void *owner)
{
	auto *chunks = static_cast<Chunks *>(owner);
	tgm_pool_free_chunks(chunks->pages, chunks->count);
	IODelete(chunks->pages, uint64_t, chunks->count);
	IODelete(chunks, Chunks, 1);
}
}  // namespace

switch_gpu_backing *switch_gpu_pin_memory(const uint64_t *pages, uint32_t count,
					  uint32_t surface_id, IOMemoryDescriptor *memory,
					  bool prepare)
{
	if (!memory)
		return nullptr;
	auto *pin = IONew(Pin, 1);
	if (!pin)
		return nullptr;
	if (prepare && memory->prepare() != kIOReturnSuccess) {
		IODelete(pin, Pin, 1);
		return nullptr;
	}
	memory->retain();
	*pin = {memory, prepare};
	auto *backing = switch_gpu_backing_register(pages, count, surface_id, pin, release_pin);
	if (!backing)
		release_pin(pin);
	return backing;
}

switch_gpu_backing *switch_gpu_pin_chunks(const uint64_t *pages, uint32_t count)
{
	auto *chunks = IONew(Chunks, 1);
	if (!chunks)
		return nullptr;
	chunks->pages = IONew(uint64_t, count);
	chunks->count = count;
	if (!chunks->pages) {
		IODelete(chunks, Chunks, 1);
		return nullptr;
	}
	memcpy(chunks->pages, pages, sizeof(*pages) * count);
	auto *backing = switch_gpu_backing_register(pages, count, 0, chunks, release_chunks);
	if (!backing) {
		IODelete(chunks->pages, uint64_t, count);
		IODelete(chunks, Chunks, 1);
	}
	return backing;
}

switch_gpu_backing *switch_gpu_pin_cached(const uint64_t *pages, uint32_t count,
					  uint32_t visible_count,
					  const SwitchGpuCachedBuffer &buffer,
					  switch_gpu_cached_mapping *mapping)
{
	auto *cached = IONew(Cached, 1);
	if (!cached)
		return nullptr;
	IOMemoryDescriptor *memory = buffer.memory;
	cached->pages = IONew(uint64_t, count);
	if (!cached->pages || memory->prepare() != kIOReturnSuccess) {
		IODelete(cached->pages, uint64_t, count);
		IODelete(cached, Cached, 1);
		return nullptr;
	}
	memcpy(cached->pages, pages, sizeof(*pages) * count);
	cached->mapping = mapping;
	cached->count = count;
	cached->buffer = buffer;
	memory->retain();
	auto *backing =
		switch_gpu_backing_register(pages, visible_count, 0, cached, release_cached);
	if (!backing) {
		memory->complete();
		memory->release();
		IODelete(cached->pages, uint64_t, count);
		IODelete(cached, Cached, 1);
	}
	return backing;
}
