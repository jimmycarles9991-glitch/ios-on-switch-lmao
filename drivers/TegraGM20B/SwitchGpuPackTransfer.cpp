// SPDX-License-Identifier: MIT
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/memory_object_types.h>
#include <mach/vm_param.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/vnode.h>
#include "SwitchGpuCachedZero.h"
#include "SwitchGpuPackTransfer.h"
#include "TegraGM20BGpu.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

#ifndef O_NOFOLLOW_ANY
#define O_NOFOLLOW_ANY 0x20000000
#endif

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not declared in every kernel config's headers
int vnode_pagein(vnode_t vp, upl_t upl, upl_offset_t upl_offset, uint64_t f_offset, upl_size_t size,
		 int flags, int *errorp);
// NOLINTNEXTLINE(readability-redundant-declaration)
ppnum_t upl_phys_page(upl_page_info_t *upl, int index);
// NOLINTNEXTLINE(readability-redundant-declaration)
boolean_t upl_page_present(upl_page_info_t *upl, int index);
// NOLINTNEXTLINE(readability-redundant-declaration)
boolean_t upl_valid_page(upl_page_info_t *upl, int index);
// NOLINTNEXTLINE(readability-redundant-declaration)
vfs_context_t vfs_context_kernel(void);
}

namespace
{
constexpr uint64_t kGpuPageBytes = 4096;
constexpr uint64_t kDmaMask34 = 0x3ffffffffULL;
constexpr uint64_t kContigMask = kDmaMask34 & ~(kSwitchGpuPackTierAlign - 1);
constexpr uint32_t kMaxUplAttempts = 4;

bool collectDescriptorPages(IOMemoryDescriptor *md, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		const addr64_t pa = md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		len &= ~0xfffULL;
		if (!len)
			return false;
		if (len > bytes - off)
			len = bytes - off;
		if (!pa || (pa & 0xfffULL) || pa + len > (1ULL << 34))
			return false;
		for (uint64_t at = 0; at < len; at += kGpuPageBytes)
			pas[(off + at) >> 12] = pa + at;
		off += len;
	}
	return true;
}
}  // namespace

#if TGM_STATS
uint64_t switch_gpu_pack_now_ns(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}
#endif

int switch_gpu_pack_read_exact(vnode_t vp, void *buf, size_t len, off_t offset, vfs_context_t ctx)
{
	auto *dst = static_cast<uint8_t *>(buf);
	size_t done = 0;
	while (done < len) {
		const size_t remain = len - done;
		const int chunk = remain > (1u << 20) ? int(1u << 20) : int(remain);
		int resid = chunk;
		const int err = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(dst + done), chunk,
					offset + off_t(done), UIO_SYSSPACE, IO_NOAUTH,
					vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (err != 0)
			return err;
		if (resid != 0)
			return EIO;
		done += size_t(chunk);
	}
	return 0;
}

bool switch_gpu_pack_alloc_tier0(uint64_t tier0Bytes, IOMemoryDescriptor **multiOut,
				 void ***chunkCpuOut, uint64_t **pasOut, uint32_t *chunkCountOut)
{
	const uint32_t chunkCount = uint32_t(tier0Bytes / kSwitchGpuPackTierAlign);
	const uint32_t totalGpuPages = uint32_t(tier0Bytes >> 12);
	auto **parts = IONew(IOMemoryDescriptor *, chunkCount);
	auto **chunkCpu = IONew(void *, chunkCount);
	auto *pas = IONew(uint64_t, totalGpuPages);
	if (!parts || !chunkCpu || !pas) {
		if (parts)
			IODelete(parts, IOMemoryDescriptor *, chunkCount);
		if (chunkCpu)
			IODelete(chunkCpu, void *, chunkCount);
		if (pas)
			IODelete(pas, uint64_t, totalGpuPages);
		return false;
	}
	bzero(parts, size_t(chunkCount) * sizeof(*parts));
	bzero(chunkCpu, size_t(chunkCount) * sizeof(*chunkCpu));
	constexpr IOOptionBits kContigOpts = IOOptionBits(kIODirectionInOut) |
					     IOOptionBits(kIOMemoryMapperNone) |
					     IOOptionBits(kIOMemoryPhysicallyContiguous);
	constexpr IOOptionBits kFallbackOpts =
		IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryMapperNone);
	uint32_t made = 0;
	while (made < chunkCount && !switch_gpu_pack_shutdown_requested()) {
		IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
			kernel_task, kContigOpts, kSwitchGpuPackTierAlign, kContigMask);
		if (!bmd) {
			bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
				kernel_task, kFallbackOpts, kSwitchGpuPackTierAlign, kDmaMask34);
		}
		if (!bmd)
			break;
		void *va = bmd->getBytesNoCopy();
		if (!va) {
			bmd->release();
			break;
		}
		parts[made] = bmd;
		chunkCpu[made] = va;
		++made;
	}
	IOMultiMemoryDescriptor *multi =
		made == chunkCount ? IOMultiMemoryDescriptor::withDescriptors(
					     parts, chunkCount, kIODirectionInOut, false)
				   : nullptr;
	if (!multi || multi->prepare(kIODirectionInOut) != kIOReturnSuccess) {
		if (multi)
			multi->release();
		for (uint32_t i = 0; i < made; ++i)
			parts[i]->release();
		IODelete(parts, IOMemoryDescriptor *, chunkCount);
		IODelete(chunkCpu, void *, chunkCount);
		IODelete(pas, uint64_t, totalGpuPages);
		return false;
	}
	constexpr uint32_t kChunkGpuPages = uint32_t(kSwitchGpuPackTierAlign >> 12);
	bool pagesOk = true;
	for (uint32_t c = 0; c < chunkCount; ++c) {
		if (!collectDescriptorPages(parts[c], kSwitchGpuPackTierAlign,
					    pas + size_t(c) * kChunkGpuPages)) {
			pagesOk = false;
			break;
		}
	}
	for (uint32_t i = 0; i < made; ++i)
		parts[i]->release();
	IODelete(parts, IOMemoryDescriptor *, chunkCount);
	if (!pagesOk) {
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(chunkCpu, void *, chunkCount);
		IODelete(pas, uint64_t, totalGpuPages);
		return false;
	}
	// Cached kernel allocations come zeroed through CPU caches; clean and invalidate once
	// before CE writes so no dirty CPU lines can later evict over CE-written program bytes.
	for (uint32_t c = 0; c < chunkCount; ++c)
		switch_dcache_clean_invalidate(chunkCpu[c], size_t(kSwitchGpuPackTierAlign));
	*multiOut = multi;
	*chunkCpuOut = chunkCpu;
	*pasOut = pas;
	*chunkCountOut = chunkCount;
	return true;
}

bool switch_gpu_pack_stream_tier0(vnode_t vp, uint64_t heapOff, uint64_t tier0Bytes,
				  uint64_t *dstPas, void **chunkCpu, const char **whyOut,
				  int *errOut, SwitchGpuPackStreamStats *stats,
				  volatile uint32_t *frontier)
{
	uint32_t landed = 0;
	constexpr uint32_t kMaxChunkGpuPages = uint32_t(kSwitchGpuPackTransferBytes >> 12);
	auto *srcPas = IONew(uint64_t, kMaxChunkGpuPages);
	if (!srcPas) {
		*whyOut = "no memory for source page list";
		*errOut = ENOMEM;
		return false;
	}
	const uint32_t gpuPerVm = uint32_t(PAGE_SIZE / kGpuPageBytes);
	constexpr int kCommitFlags =
		UPL_COMMIT_FREE_ON_EMPTY | UPL_COMMIT_INACTIVATE | UPL_COMMIT_CLEAR_PRECIOUS;
	bool ok = true;
	for (uint64_t off = 0; ok && off < tier0Bytes;) {
		if (switch_gpu_pack_shutdown_requested()) {
			*whyOut = "shutdown";
			*errOut = ECANCELED;
			ok = false;
			break;
		}
		const uint64_t chunkBytes = tier0Bytes - off < kSwitchGpuPackTransferBytes
						    ? tier0Bytes - off
						    : kSwitchGpuPackTransferBytes;
		const uint32_t vmPages = uint32_t(chunkBytes / PAGE_SIZE);
		const uint32_t gpuPages = uint32_t(chunkBytes >> 12);
		const uint64_t fileOff = heapOff + off;

		upl_t upl = nullptr;
		upl_page_info_t *pl = nullptr;
		bool haveUpl = false;
		for (uint32_t attempt = 0; attempt < kMaxUplAttempts && !haveUpl; ++attempt) {
			if (switch_gpu_pack_shutdown_requested()) {
				*whyOut = "shutdown";
				*errOut = ECANCELED;
				ok = false;
				break;
			}
			// APFS sets VFC_VFSVNOP_PAGEINV2 and must acquire its locks before UPL
			// creation; passing upl == NULL lets vnode_pagein populate the cache first.
			int pagerErr = 0;
			const int pageinRc =
				vnode_pagein(vp, nullptr, 0, fileOff, upl_size_t(chunkBytes),
					     UPL_IOSYNC, &pagerErr);
#if TGM_STATS
			++stats->pageinReqs;
#else
			(void)stats;
#endif
			if (pageinRc != 0 || pagerErr != 0) {
				*whyOut = "pagein failed";
				*errOut = EIO;
				ok = false;
				break;
			}
			if (switch_gpu_pack_shutdown_requested()) {
				*whyOut = "shutdown";
				*errOut = ECANCELED;
				ok = false;
				break;
			}
			// UPL_COPYOUT_FROM pins only resident pages without disconnecting pmaps or
			// allocating absent pages, so a single full-range commit frees the UPL.
			const kern_return_t kr =
				ubc_create_upl_external(vp, off_t(fileOff), int(chunkBytes), &upl,
							&pl, UPL_COPYOUT_FROM | UPL_SET_LITE);
			if (kr != KERN_SUCCESS || !upl || !pl) {
				*whyOut = "ubc_create_upl failed";
				*errOut = int(kr);
				ok = false;
				break;
			}
			bool allValid = true;
			bool badPhys = false;
			for (uint32_t p = 0; p < vmPages; ++p) {
				if (!upl_page_present(pl, int(p)) || !upl_valid_page(pl, int(p))) {
					allValid = false;
					break;
				}
				const ppnum_t ppn = upl_phys_page(pl, int(p));
				const uint64_t vmPa = uint64_t(ppn) << PAGE_SHIFT;
				if (!ppn || vmPa + PAGE_SIZE > (1ULL << 34)) {
					badPhys = true;
					break;
				}
				for (uint32_t sub = 0; sub < gpuPerVm; ++sub)
					srcPas[p * gpuPerVm + sub] = vmPa + sub * kGpuPageBytes;
			}
			if (badPhys) {
				ubc_upl_commit_range(upl, 0, upl_size_t(chunkBytes), kCommitFlags);
				*whyOut = "page physical address out of range";
				*errOut = EIO;
				ok = false;
				break;
			}
			if (!allValid) {
				ubc_upl_commit_range(upl, 0, upl_size_t(chunkBytes), kCommitFlags);
				upl = nullptr;
				pl = nullptr;
				continue;
			}
			haveUpl = true;
		}
		if (!ok)
			break;
		const uint32_t firstChunk = uint32_t(off / kSwitchGpuPackTierAlign);
		const uint32_t chunks = uint32_t(chunkBytes / kSwitchGpuPackTierAlign);
		if (!haveUpl) {
			vfs_context_t ctx = vfs_context_kernel();
			for (uint32_t c = 0; c < chunks; ++c) {
				const int readErr = switch_gpu_pack_read_exact(
					vp, chunkCpu[firstChunk + c],
					size_t(kSwitchGpuPackTierAlign),
					off_t(fileOff + uint64_t(c) * kSwitchGpuPackTierAlign),
					ctx);
				if (readErr != 0) {
					*whyOut = "tier-0 read fallback failed";
					*errOut = readErr;
					ok = false;
					break;
				}
				switch_dcache_clean_invalidate(chunkCpu[firstChunk + c],
							       size_t(kSwitchGpuPackTierAlign));
			}
			if (!ok)
				break;
			off += chunkBytes;
			if (frontier)
				switch_gpu_pack_raise(frontier, ++landed);
			continue;
		}
		// Flush any APFS CPU-decrypted or CPU-decompressed page-cache lines to DRAM.
		tgm_cpu_clean_pages(srcPas, gpuPages);
#if TGM_STATS
		const uint64_t copyStart = switch_gpu_pack_now_ns();
#endif
		const int copyErr = switch_gpu_cached_copy(srcPas, dstPas + (off >> 12), gpuPages);
#if TGM_STATS
		stats->copyNs += switch_gpu_pack_now_ns() - copyStart;
#endif
		ubc_upl_commit_range(upl, 0, upl_size_t(chunkBytes), kCommitFlags);
		if (copyErr != 0) {
			*whyOut = "copy engine failed";
			*errOut = copyErr;
			ok = false;
			break;
		}
		for (uint32_t c = 0; c < chunks; ++c)
			switch_dcache_invalidate(chunkCpu[firstChunk + c],
						 size_t(kSwitchGpuPackTierAlign));
		off += chunkBytes;
		if (frontier)
			switch_gpu_pack_raise(frontier, ++landed);
	}
	IODelete(srcPas, uint64_t, kMaxChunkGpuPages);
	return ok;
}

namespace
{
bool prefetchRange(vnode_t vp, uint64_t offset, uint64_t bytes, uint64_t fileSize)
{
	if (!bytes || offset >= fileSize)
		return true;
	uint64_t at = offset & ~uint64_t(PAGE_SIZE - 1);
	const uint64_t end = (offset + bytes + PAGE_SIZE - 1) & ~uint64_t(PAGE_SIZE - 1);
	for (; at < end && at < fileSize; at += kSwitchGpuPackTransferBytes) {
		if (switch_gpu_pack_shutdown_requested())
			return false;
		const uint64_t length = end - at < kSwitchGpuPackTransferBytes
						? end - at
						: kSwitchGpuPackTransferBytes;
		int pagerErr = 0;
		if (vnode_pagein(vp, nullptr, 0, at, upl_size_t(length), UPL_IOSYNC, &pagerErr) !=
			    0 ||
		    pagerErr != 0)
			return false;
	}
	return true;
}
}  // namespace

bool switch_gpu_pack_prefetch_tables(vnode_t vp, const SwitchGpuPackHeader &hdr, vfs_context_t ctx)
{
	static constexpr uint32_t kTables[] = {
		kSwitchGpuPackFastKeys, kSwitchGpuPackPipelines, kSwitchGpuPackPrograms,
		kSwitchGpuPackSegments, kSwitchGpuPackVariants,  kSwitchGpuPackBlob,
		kSwitchGpuPackStrings,  kSwitchGpuPackSplits,    kSwitchGpuPackIterated,
		kSwitchGpuPackInit,     kSwitchGpuPackBaseline,  kSwitchGpuPackSamplers,
	};
	uint64_t fileSize = 0;
	for (const SwitchGpuPackSection &s : hdr.section)
		if (s.offset <= UINT64_MAX - s.bytes && s.offset + s.bytes > fileSize)
			fileSize = s.offset + s.bytes;
	bool ok = true;
	for (const uint32_t id : kTables) {
		const SwitchGpuPackSection &s = hdr.section[id];
		const uint64_t bytes = id == kSwitchGpuPackFastKeys ? s.bytes
				       : hdr.tier0[id] < s.bytes    ? hdr.tier0[id]
								    : s.bytes;
		ok = prefetchRange(vp, s.offset, bytes, fileSize) && ok;
	}
	// PackSegmentTable: pipelineCount, variantComboCount, wordsOffset, wordsBytes (the words in the heap's tier 0)
	uint32_t table[4] = {};
	const SwitchGpuPackSection &segments = hdr.section[kSwitchGpuPackSegments];
	if (segments.bytes >= sizeof table && !switch_gpu_pack_shutdown_requested() &&
	    switch_gpu_pack_read_exact(vp, table, sizeof table, off_t(segments.offset), ctx) == 0 &&
	    table[3] && uint64_t(table[2]) + table[3] <= hdr.tier0[kSwitchGpuPackHeap])
		ok = prefetchRange(vp, hdr.section[kSwitchGpuPackHeap].offset + table[2], table[3],
				   fileSize) &&
		     ok;
	return ok;
}
