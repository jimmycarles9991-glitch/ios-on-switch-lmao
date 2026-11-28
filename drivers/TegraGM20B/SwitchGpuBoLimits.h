// SPDX-License-Identifier: MIT
// Size and flag rules for BoAlloc and BoImport: plain cached allocations and host imports admit up to 2 GiB (enough
// for a 16384x16384x6 R8Unorm cube), while pool, coherent, contiguous, compressible and control allocations stay within
// the 256 MiB maxBufferLength limit.
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"

constexpr uint64_t SWITCH_GPU_MAX_BUFFER_BYTES = 256ULL << 20;
constexpr uint64_t SWITCH_GPU_MAX_BO_BYTES = 2ULL << 30;
constexpr uint64_t SWITCH_GPU_BO_CHUNK_BYTES = 64ULL << 10;
#if DEVELOPMENT || DEBUG
constexpr bool SWITCH_GPU_BO_PROBES = true;
#else
constexpr bool SWITCH_GPU_BO_PROBES = false;
#endif

constexpr uint32_t switch_gpu_bo_flags_mask(bool probes)
{
	return static_cast<uint32_t>(TegraGM20BBoCoherent) |
	       static_cast<uint32_t>(TegraGM20BBoContiguous) |
	       static_cast<uint32_t>(TegraGM20BBoCompressible) |
	       static_cast<uint32_t>(TegraGM20BBoControl) |
	       static_cast<uint32_t>(TegraGM20BBoSystem) |
	       (probes ? static_cast<uint32_t>(TegraGM20BBoProbeDirect) : 0u);
}

constexpr uint32_t SWITCH_GPU_BO_FLAGS_MASK = switch_gpu_bo_flags_mask(SWITCH_GPU_BO_PROBES);

constexpr uint64_t switch_gpu_bo_alloc_limit(uint32_t flags)
{
	return flags == 0u ? SWITCH_GPU_MAX_BO_BYTES : SWITCH_GPU_MAX_BUFFER_BYTES;
}

constexpr bool switch_gpu_bo_alloc_args_ok(uint64_t bytes, uint32_t align_log2, uint32_t flags,
					   bool probes = SWITCH_GPU_BO_PROBES)
{
	const bool compressible = (flags & TegraGM20BBoCompressible) != 0u;
	const bool system = (flags & TegraGM20BBoSystem) != 0u;
	const uint32_t systemForbidden =
		TegraGM20BBoCompressible | TegraGM20BBoControl | TegraGM20BBoContiguous;
	const bool systemBad =
		system && (!(flags & TegraGM20BBoCoherent) || (flags & systemForbidden) != 0u);
	return bytes != 0u && (bytes & 0xfffULL) == 0u &&
	       bytes <= switch_gpu_bo_alloc_limit(flags) && align_log2 >= 12u &&
	       align_log2 <= 21u && (flags & ~switch_gpu_bo_flags_mask(probes)) == 0u &&
	       !((flags & TegraGM20BBoControl) && !(flags & TegraGM20BBoCoherent)) && !systemBad &&
	       !(compressible && (flags != TegraGM20BBoCompressible ||
				  (bytes & (SWITCH_GPU_BO_CHUNK_BYTES - 1u)) != 0u));
}

/* An uncompressed BO of its own pages, 64 KiB or more and not physically contiguous, is compacted into the Tegra SMMU's
 * 64 KiB big pages where the SMMU is up (72a0e9b01), unless it asks for direct physical addresses */
constexpr bool switch_gpu_bo_big_smmu(uint32_t flags, bool pool, uint32_t npages,
				      uint32_t granule_pages, bool smmu_ready)
{
	return !(flags &
		 (TegraGM20BBoCompressible | TegraGM20BBoContiguous | TegraGM20BBoProbeDirect)) &&
	       !pool && npages >= granule_pages && smmu_ready;
}

constexpr bool switch_gpu_bo_import_args_ok(uint64_t address, uint64_t bytes)
{
	return bytes != 0u && ((address | bytes) & 0xfffULL) == 0u &&
	       bytes <= SWITCH_GPU_MAX_BO_BYTES && address <= UINT64_MAX - bytes;
}
