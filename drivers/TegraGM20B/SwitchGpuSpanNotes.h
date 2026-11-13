/* SPDX-License-Identifier: MIT */
/* The render server's long GPU spans (debug kernels): its raw path names each stretch of kLongSpanNs or more of one state
 * (drivers/TegraGM20BMetal/raw/runtime/raw_span_marks.h) and notes it here (TegraGM20BMethodPaths' note-span op), beside
 * the compositor's longest job, so a HW Test that made the compositor work (ui.launch_hitch) reads what it cost and which
 * states drew it. The newest notes are kept. Plain C++ for the host test (test/gm20b_span_notes_test.cpp). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace switch_gpu_spans
{
constexpr uint32_t kNotes = 16;

struct Note {
	char owner[17];
	char text[112];
};

struct Notes {
	Note note[kNotes];
	uint32_t next, count;
};

inline void add(Notes &n, const char *owner, const char *text)
{
	Note &at = n.note[n.next % kNotes];
	strlcpy(at.owner, owner, sizeof(at.owner));
	strlcpy(at.text, text, sizeof(at.text));
	n.next++;
	if (n.count < kNotes)
		n.count++;
}

/* "job_max_us=N <sync> spans=K; <owner>: <text>; ..." with the compositor's longest job since the last take and its sync
 * totals (SwitchGpuCompositorSync.h), the notes oldest first, whole notes only; then the notes start again */
inline void take(Notes &n, uint64_t job_max_ns, const char *sync, char *out, size_t size)
{
	if (!size)
		return;
	int put = snprintf(out, size, "job_max_us=%llu %s spans=%u",
			   (unsigned long long)(job_max_ns / 1000), sync, n.count);
	size_t used = put > 0 ? static_cast<size_t>(put) : 0;
	for (uint32_t i = 0; i < n.count && used < size; i++) {
		const Note &at = n.note[(n.next - n.count + i) % kNotes];
		char entry[sizeof(Note) + 4];
		put = snprintf(entry, sizeof(entry), "; %s: %s", at.owner, at.text);
		if (put <= 0 || used + static_cast<size_t>(put) >= size)
			break;
		memcpy(out + used, entry, static_cast<size_t>(put) + 1);
		used += static_cast<size_t>(put);
	}
	n.count = 0;
}
}  // namespace switch_gpu_spans
