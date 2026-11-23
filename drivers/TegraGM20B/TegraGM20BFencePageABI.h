/* SPDX-License-Identifier: MIT */
/* A connection's fence page (GetInfo's and ChannelCreate's fence_token): every region the kernel, the GPU or the client
 * reads or writes in it. Each feature's offsets come from its region here, and no two regions share a byte. The kernel
 * zeroes the page when it hands it out (gpu_chan.c tgm_fence_page_get) and maps it write-combined; the client maps it
 * read-only. The GPU writes it (semaphore releases) through the connection's own mapping of its first
 * TegraGM20BFencePageGpuBytes (TegraGM20BUserClient.cpp setUp: one 4 KiB GPU page), and reads it there and through every
 * address space's read-only view of the whole fence range: every region lies in those bytes. */
#ifndef TEGRA_GM20B_FENCE_PAGE_ABI_H
#define TEGRA_GM20B_FENCE_PAGE_ABI_H

/* name, first byte, bytes; in address order:
 *   Channels        per channel slot: TegraGM20BAbiFence, AbiFenceSemantic, AbiFenceIdentity
 *   Visible         per channel slot: the seqno its External jobs release, read by the kernel alone
 *   ChannelEpoch    TegraGM20BABI.h TegraGM20BChannelEpochWord
 *   BindEpoch       the connection's bind epoch, which each of its submissions acquires
 *   Events          TegraGM20BJobABI.h method 40's imported event words
 *   ClockAnchors    TegraGM20BClockABI.h TegraGM20BAbiClockAnchors
 *   SetGenerations  TegraGM20BJobABI.h method 31's set generations
 *   Promises        TegraGM20BJobABI.h method 34's writer promise words */
#define TEGRA_GM20B_FENCE_PAGE_REGIONS(X) \
	X(Channels, 0, 8 * 64)            \
	X(Visible, 512, 8 * 4)            \
	X(ChannelEpoch, 1020, 4)          \
	X(BindEpoch, 1024, 4)             \
	X(Events, 1028, 255 * 4)          \
	X(ClockAnchors, 2048, 1024)       \
	X(SetGenerations, 3072, 16 * 4)   \
	X(Promises, 3136, 32 * 4)

enum {
	TegraGM20BFencePageGpuBytes = 4096,
	TegraGM20BFencePageRecordBytes = 64, /* a channel slot of Channels */
#define TEGRA_GM20B_FENCE_PAGE_ENUM(name, first, bytes) \
	TegraGM20BFencePage##name##First = (first), TegraGM20BFencePage##name##Bytes = (bytes),
	TEGRA_GM20B_FENCE_PAGE_REGIONS(TEGRA_GM20B_FENCE_PAGE_ENUM)
#undef TEGRA_GM20B_FENCE_PAGE_ENUM
};

#ifdef __cplusplus
#define TEGRA_GM20B_FENCE_PAGE_ASSERT(x, why) static_assert(x, why)
#else
#define TEGRA_GM20B_FENCE_PAGE_ASSERT(x, why) _Static_assert(x, why)
#endif
#define TEGRA_GM20B_FENCE_PAGE_FITS(name, first, bytes)                                         \
	TEGRA_GM20B_FENCE_PAGE_ASSERT((first) % 4 == 0 && (bytes) > 0 &&                        \
					      (first) + (bytes) <= TegraGM20BFencePageGpuBytes, \
				      #name " is whole words in the GPU's mapping of the page");
TEGRA_GM20B_FENCE_PAGE_REGIONS(TEGRA_GM20B_FENCE_PAGE_FITS)
#undef TEGRA_GM20B_FENCE_PAGE_FITS
TEGRA_GM20B_FENCE_PAGE_ASSERT(TegraGM20BFencePageVisibleBytes / 4 ==
				      TegraGM20BFencePageChannelsBytes /
					      TegraGM20BFencePageRecordBytes,
			      "a visibility word for each channel slot");

#ifdef __cplusplus
struct tgm_fence_page_region {
	unsigned first, bytes;
};

static constexpr tgm_fence_page_region tgm_fence_page_regions[] = {
#define TEGRA_GM20B_FENCE_PAGE_ENTRY(name, first, bytes) {first, bytes},
	TEGRA_GM20B_FENCE_PAGE_REGIONS(TEGRA_GM20B_FENCE_PAGE_ENTRY)
#undef TEGRA_GM20B_FENCE_PAGE_ENTRY
};

static constexpr bool tgm_fence_page_disjoint()
{
	for (const tgm_fence_page_region &a : tgm_fence_page_regions)
		for (const tgm_fence_page_region &b : tgm_fence_page_regions)
			if (&a < &b && a.first < b.first + b.bytes && b.first < a.first + a.bytes)
				return false;
	return true;
}

static_assert(tgm_fence_page_disjoint(), "two regions of the fence page share a byte");
#endif

#endif
