// SPDX-License-Identifier: MIT
// The present watch's producer, read in the GPU interrupts' primary filter (shim/xnu_glue.cpp): when its
// writes are complete and visible the display's callout (TegraScanoutCopy.h tgm_present_filter_callout) runs there,
// ahead of the work loop's interrupt pass, whose tgm_present_pass delivers the present as before. The view is attached
// just before the watch is armed and detached at its claim (gpu_present.c), before its tokens drop and done runs.
//
// The filter takes no lock. Attach and detach alternate (one present is outstanding, and the next is offered only after
// this one's done), so the view has one writer at a time, which publishes it under a sequence, odd while it changes; the
// filter acts only on a view it read between two equal even sequences. The view holds its own reference on the token,
// which a detach releases once no filter can still be reading it: each CPU's filter marks its runs (odd while it runs),
// and the dropped view's reference goes when every CPU's mark is even or has moved since the drop.
#include <IOKit/IOLib.h>
#include <kern/cpu_number.h>
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

namespace
{
constexpr unsigned kCpus = 8;

struct Published {
	uint32_t seq;
	const switch_gpu_timeline *timeline;
	uint32_t value;
	// the sequence of the view whose callout ran (odd, so none, when published): an exchange, as two CPUs run the filter
	// at once while the GIC moves the frame lines between cores; once a present, at its producer's visibility
	uint32_t called;
};

struct Callout {
	uint32_t seq;
	void (*fn)(void *, int);
	void *context;
};

struct Dropped {
	switch_gpu_backing_token token;
	uint32_t runs[kCpus];
	bool held;
};

Published gView;
Callout gCallout;
uint32_t gRuns[kCpus];  // each written by its own CPU's filter alone, with interrupts off

// the writers' own: the attached view's reference, and a dropped one still waiting for the filters to leave it
switch_gpu_backing_token gHeld;
bool gHolding;
Dropped gDropped;

template <typename T> T load(const T &word)
{
	return __atomic_load_n(&word, __ATOMIC_RELAXED);
}

template <typename T> void store(T &word, T value)
{
	__atomic_store_n(&word, value, __ATOMIC_RELAXED);
}

void publish(const switch_gpu_timeline *timeline, uint32_t value)
{
	const uint32_t seq = load(gView.seq);
	__atomic_store_n(&gView.seq, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	store(gView.timeline, timeline);
	store(gView.value, value);
	store(gView.called, seq + 1);
	__atomic_store_n(&gView.seq, seq + 2, __ATOMIC_RELEASE);
}

bool leftBy(const uint32_t *marked)
{
	for (unsigned cpu = 0; cpu < kCpus; cpu++) {
		if ((marked[cpu] & 1) &&
		    __atomic_load_n(&gRuns[cpu], __ATOMIC_ACQUIRE) == marked[cpu])
			return false;
	}
	return true;
}

void releaseDropped()
{
	if (gDropped.held && leftBy(gDropped.runs)) {
		switch_gpu_backing_token_release(&gDropped.token);
		gDropped.held = false;
	}
}

void drop()
{
	releaseDropped();
	if (!gHolding)
		return;
	publish(nullptr, 0);
	gHolding = false;
	// against the filter's mark-then-read: either its mark is seen here or it reads the view gone
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (gDropped.held) {
		// a filter run the length of a present: never seen; its view's timeline is kept rather than freed under it
		IOLog("tegra-gm20b: present filter: a CPU is still in the filter run that read the "
		      "last present's view\n");
		return;
	}
	for (unsigned cpu = 0; cpu < kCpus; cpu++)
		gDropped.runs[cpu] = __atomic_load_n(&gRuns[cpu], __ATOMIC_ACQUIRE);
	gDropped.token = gHeld;
	gDropped.held = true;
	releaseDropped();
}

// The view read whole, or nothing: false while a writer changes it or after it changed since `seq`
bool readView(uint32_t seq, switch_gpu_timeline_view *view)
{
	view->timeline = load(gView.timeline);
	view->value = load(gView.value);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return view->timeline && load(gView.seq) == seq;
}

bool readCallout(void (**fn)(void *, int), void **context)
{
	const uint32_t seq = __atomic_load_n(&gCallout.seq, __ATOMIC_ACQUIRE);
	*fn = load(gCallout.fn);
	*context = load(gCallout.context);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return !(seq & 1) && *fn && load(gCallout.seq) == seq;
}
}  // namespace

// Once, at the display's start: it has one writer.
extern "C" void tgm_present_filter_callout(void (*fn)(void *, int), void *ctx)
{
	const uint32_t seq = load(gCallout.seq);
	__atomic_store_n(&gCallout.seq, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	store(gCallout.fn, fn);
	store(gCallout.context, ctx);
	__atomic_store_n(&gCallout.seq, seq + 2, __ATOMIC_RELEASE);
}

// A token whose status needs the timeline's lock (a semantic wait at or below it) is not watched here: the work
// loop's pass delivers it.
extern "C" void tgm_present_filter_attach(const struct switch_gpu_backing_token *token)
{
	switch_gpu_timeline_view view = {};
	drop();
	if (switch_gpu_backing_token_retain(token))
		return;
	if (switch_gpu_timeline_view_open(token, &view)) {
		switch_gpu_backing_token_release(token);
		return;
	}
	gHeld = *token;
	gHolding = true;
	publish(view.timeline, view.value);
}

extern "C" void tgm_present_filter_detach(void)
{
	drop();
}

extern "C" void tgm_present_filter(void)
{
	const uint32_t seq = __atomic_load_n(&gView.seq, __ATOMIC_ACQUIRE);
	switch_gpu_timeline_view view = {};
	void (*fn)(void *, int) = nullptr;
	void *context = nullptr;

	if ((seq & 1) || !readView(seq, &view))
		return;
	const unsigned cpu = static_cast<unsigned>(cpu_number()) % kCpus;
	const uint32_t runs = __atomic_load_n(&gRuns[cpu], __ATOMIC_RELAXED);
	__atomic_store_n(&gRuns[cpu], runs + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (readView(seq, &view) && switch_gpu_timeline_view_visible(&view) == 1 &&
	    readCallout(&fn, &context) &&
	    __atomic_exchange_n(&gView.called, seq, __ATOMIC_RELAXED) != seq &&
	    load(gView.seq) == seq)
		fn(context, 0);
	__atomic_store_n(&gRuns[cpu], runs + 2, __ATOMIC_RELEASE);
}
