// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BABI.h"

#if defined(KERNEL) || defined(SWITCH_GPU_PACK_IMAGE_HOST_TEST)
#include <IOKit/IOReturn.h>
#include <mach/mach_types.h>
class IOMemoryDescriptor;
#endif

struct tgm_mem;
struct tgm_vm;
struct switch_gpu_mapping;

inline constexpr char kSwitchGpuPackMagic[8] = {'G', 'M', '2', '0', 'B', 'P', 'K', '1'};
inline constexpr uint32_t kSwitchGpuPackVersion = 33u;
inline constexpr uint64_t kSwitchGpuPackTierAlign = 128ULL << 10;
inline constexpr uint32_t kSwitchGpuPackChunkPages = uint32_t(kSwitchGpuPackTierAlign >> 12);
inline constexpr uint64_t kSwitchGpuPackTransferBytes = 1ULL << 20;
inline constexpr uint64_t kSwitchGpuPackMaxTier0HeapBytes = 512ULL << 20;
inline constexpr uint64_t kSwitchGpuPackMaxCpuWireBytes = 64ULL << 20;

enum SwitchGpuPackSectionId : uint32_t {
	kSwitchGpuPackPrograms = 0,
	kSwitchGpuPackPipelines = 1,
	kSwitchGpuPackSamplers = 2,
	kSwitchGpuPackBaseline = 3,
	kSwitchGpuPackInit = 4,
	kSwitchGpuPackBlob = 5,
	kSwitchGpuPackStrings = 6,
	kSwitchGpuPackHeap = 7,
	kSwitchGpuPackKernels = 8,
	kSwitchGpuPackSplits = 9,
	kSwitchGpuPackVariants = 10,
	kSwitchGpuPackArchives = 11,
	kSwitchGpuPackIterated = 12,
	kSwitchGpuPackComputeStates = 13,
	kSwitchGpuPackReflections = 14,
	kSwitchGpuPackFastKeys = 15,
	kSwitchGpuPackSectionCount = 16,
};

struct __attribute__((packed)) SwitchGpuPackSection {
	uint64_t offset;
	uint64_t bytes;
};

struct __attribute__((packed)) SwitchGpuPackHeader {
	char magic[8];
	uint32_t version;
	uint32_t programs;
	uint32_t pipelines;
	uint32_t samplers;
	SwitchGpuPackSection section[kSwitchGpuPackSectionCount];
	uint64_t stack;
	uint32_t chipFacts;
	uint32_t reserved;
	uint64_t tier0[kSwitchGpuPackSectionCount];
};

static_assert(sizeof(SwitchGpuPackHeader) == 424, "v33 PackHeader size");

enum class SwitchGpuPackState : uint32_t {
	None = 0,
	Building = 1,
	Ready = 2,
	Refused = 3,
};

struct SwitchGpuPackByteRange {
	uint64_t offset;
	uint64_t bytes;
};

inline bool switch_gpu_pack_validate_header(const SwitchGpuPackHeader *hdr, uint64_t fileSize,
					    const char **whyOut)
{
	const auto fail = [&](const char *why) {
		if (whyOut)
			*whyOut = why;
		return false;
	};
	if (!hdr)
		return fail("null header");
	if (fileSize < sizeof(SwitchGpuPackHeader) || fileSize > uint64_t(INT64_MAX))
		return fail("file shorter than header");
	if (memcmp(hdr->magic, kSwitchGpuPackMagic, sizeof(kSwitchGpuPackMagic)) != 0)
		return fail("bad magic");
	if (hdr->version != kSwitchGpuPackVersion)
		return fail("unsupported version");
	for (uint32_t s = 0; s < kSwitchGpuPackSectionCount; ++s) {
		const uint64_t off = hdr->section[s].offset;
		const uint64_t len = hdr->section[s].bytes;
		if (off > fileSize || len > fileSize - off)
			return fail("section past eof");
		if (hdr->tier0[s] > len)
			return fail("tier0 exceeds section");
	}
	const uint64_t heapOff = hdr->section[kSwitchGpuPackHeap].offset;
	const uint64_t heapBytes = hdr->section[kSwitchGpuPackHeap].bytes;
	const uint64_t tier0Bytes = hdr->tier0[kSwitchGpuPackHeap];
	if (heapOff < sizeof(SwitchGpuPackHeader))
		return fail("heap overlaps header");
	if (!heapBytes || !tier0Bytes)
		return fail("empty tier-0 heap");
	if ((heapOff & (kSwitchGpuPackTierAlign - 1)) != 0)
		return fail("heap offset unaligned");
	if ((tier0Bytes & (kSwitchGpuPackTierAlign - 1)) != 0)
		return fail("tier-0 heap unaligned");
	if (heapBytes > TGM_PACK_WINDOW_BYTES)
		return fail("heap exceeds window");
	if (tier0Bytes > kSwitchGpuPackMaxTier0HeapBytes)
		return fail("tier-0 heap exceeds limit");
	if (whyOut)
		*whyOut = nullptr;
	return true;
}

inline bool switch_gpu_pack_validate_cpu_ranges(const TegraGM20BAbiPackCpuRange *ranges,
						uint32_t count, uint64_t vmPageBytes,
						uint64_t *totalBytesOut)
{
	if (!ranges || count > TegraGM20BMaxPackCpuRanges || !vmPageBytes ||
	    (vmPageBytes & (vmPageBytes - 1)) != 0)
		return false;
	const uint64_t pageMask = vmPageBytes - 1;
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const uint64_t addr = ranges[i].address;
		const uint64_t len = ranges[i].bytes;
		if (!addr || !len || ((addr | len) & pageMask) != 0)
			return false;
		if (addr > UINT64_MAX - len)
			return false;
		if (len > kSwitchGpuPackMaxCpuWireBytes - total)
			return false;
		total += len;
	}
	for (uint32_t i = count; i < TegraGM20BMaxPackCpuRanges; ++i) {
		if (ranges[i].address != 0 || ranges[i].bytes != 0)
			return false;
	}
	if (totalBytesOut)
		*totalBytesOut = total;
	return true;
}

inline uint32_t switch_gpu_pack_warm_ranges(const SwitchGpuPackHeader *hdr, uint64_t fileSize,
					    uint64_t vmPageBytes, SwitchGpuPackByteRange *out,
					    uint32_t cap)
{
	if (!hdr || !out || !cap || !vmPageBytes || (vmPageBytes & (vmPageBytes - 1)) != 0)
		return 0;
	static constexpr uint32_t kSections[4] = {
		kSwitchGpuPackHeap,
		kSwitchGpuPackFastKeys,
		kSwitchGpuPackPrograms,
		kSwitchGpuPackVariants,
	};
	const uint64_t pageMask = vmPageBytes - 1;
	const uint64_t fileEndAligned = (fileSize + pageMask) & ~pageMask;
	uint32_t count = 0;
	for (uint32_t s : kSections) {
		if (count >= cap)
			break;
		const uint64_t secOff = hdr->section[s].offset;
		const uint64_t secBytes = hdr->section[s].bytes;
		const uint64_t t0 = hdr->tier0[s];
		if (t0 >= secBytes)
			continue;
		const uint64_t rawStart = secOff + t0;
		const uint64_t rawEnd = secOff + secBytes;
		const uint64_t pageStart = rawStart & ~pageMask;
		uint64_t pageEnd = (rawEnd + pageMask) & ~pageMask;
		if (pageEnd > fileEndAligned)
			pageEnd = fileEndAligned;
		if (pageEnd > pageStart && pageStart < fileSize)
			out[count++] = {pageStart, pageEnd - pageStart};
	}
	return count;
}

#if defined(KERNEL) || defined(SWITCH_GPU_PACK_IMAGE_HOST_TEST)
struct SwitchGpuPackClient {
	switch_gpu_mapping *gpuMapping;
	uint64_t gpuBytes;
	IOMemoryDescriptor *cpuWired[TegraGM20BMaxPackCpuRanges];
	uint32_t cpuWiredCount;
};

struct SwitchGpuPackImageView {
	bool ready;
	tgm_mem *mem;
	IOMemoryDescriptor *cpuMemory;
	uint64_t tier0HeapBytes;
	uint64_t heapBytes;
	uint64_t stack;
};

IOReturn switch_gpu_pack_image_acquire(const char *procName, SwitchGpuPackImageView *out,
				       uint64_t *waitedNs);
IOReturn switch_gpu_pack_client_wire_cpu(task_t task, const TegraGM20BAbiPackCpuRange *ranges,
					 uint32_t count, IOMemoryDescriptor **outDescriptors,
					 uint64_t *outBytes);
void switch_gpu_pack_client_release_cpu(SwitchGpuPackClient *client);
void switch_gpu_pack_client_unmap_gpu(tgm_vm *vm, SwitchGpuPackClient *client);

extern "C" {
void switch_gpu_boot_complete(void);
void switch_gpu_pack_shutdown(void);
}
#endif

#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
void switch_gpu_pack_image_reset_for_test(void);
uint32_t switch_gpu_pack_image_refused_count_for_test(void);
void switch_gpu_pack_image_wait_warm_for_test(void);
#endif
