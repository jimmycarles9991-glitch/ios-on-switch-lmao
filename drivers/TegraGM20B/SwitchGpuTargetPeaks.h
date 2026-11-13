/* SPDX-License-Identifier: MIT */
/* The largest colour-target image each process drew or cleared into since boot (debug kernels): the push scan's record
 * (TegraGM20BUserClientChannel.cpp scanPushes, one submission in kScanEvery), which TegraGM20BMethodPaths' target-peaks op
 * answers, so a HW Test sees a lock screen poster's targets after the poster stopped drawing. An image is the target's
 * pixels, a multisampled target's sample grid over its grid (TegraGM20BPushScan.h targetImage). Plain C++ for the host
 * test (test/gm20b_target_peaks_test.cpp). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace switch_gpu_peaks
{
constexpr uint32_t kOwners = 24;

struct Peak {
	char owner[17];
	uint32_t width, height, samples;
};

inline uint64_t pixels(const Peak &p)
{
	return static_cast<uint64_t>(p.width) * p.height;
}

// A process seen before keeps its larger image; a new one takes a free slot, else the smallest peak's when larger than
// it, so the processes with the largest images stay however many draw small ones
inline void note(Peak (&table)[kOwners], const char *owner, uint32_t width, uint32_t height,
		 uint32_t samples)
{
	const uint64_t area = static_cast<uint64_t>(width) * height;
	if (!area || !owner[0])
		return;
	Peak *smallest = &table[0];
	for (Peak &p : table) {
		if (p.owner[0] && !strncmp(p.owner, owner, sizeof(p.owner) - 1)) {
			if (area > pixels(p)) {
				p.width = width;
				p.height = height;
				p.samples = samples;
			}
			return;
		}
		if (pixels(p) < pixels(*smallest))
			smallest = &p;
	}
	if (area <= pixels(*smallest))
		return;
	strlcpy(smallest->owner, owner, sizeof(smallest->owner));
	smallest->width = width;
	smallest->height = height;
	smallest->samples = samples;
}

// "owner=WxHxS;..." largest first, whole entries only: what does not fit in `size` is the smallest
inline void line(const Peak (&table)[kOwners], char *out, size_t size)
{
	if (!size)
		return;
	out[0] = 0;
	bool taken[kOwners] = {};
	size_t used = 0;
	for (uint32_t n = 0; n < kOwners; n++) {
		const Peak *best = nullptr;
		uint32_t at = 0;
		for (uint32_t i = 0; i < kOwners; i++)
			if (!taken[i] && pixels(table[i]) &&
			    (!best || pixels(table[i]) > pixels(*best))) {
				best = &table[i];
				at = i;
			}
		if (!best)
			return;
		taken[at] = true;
		char entry[48];
		const int wrote = snprintf(entry, sizeof(entry), "%s=%ux%ux%u;", best->owner,
					   best->width, best->height, best->samples);
		if (wrote <= 0 || used + static_cast<size_t>(wrote) >= size)
			return;
		memcpy(out + used, entry, static_cast<size_t>(wrote) + 1);
		used += static_cast<size_t>(wrote);
	}
}
}  // namespace switch_gpu_peaks
