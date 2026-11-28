// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuBridgeProducer.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace
{
// Under the bridge's lock. host1x hands out 32 syncpoints and takes none back, and its engines need theirs, so the GPU's
// channels take at most SWITCH_GPU_TIMELINE_SYNCPTS and pass on those whose timelines have gone.
uint32_t made;
uint32_t kept;
}  // namespace

bool switch_gpu_producer_wait(const switch_gpu_backing_token *tokens, uint32_t count,
			      SwitchGpuProducerWait &wait)
{
	bool covered = false;
	for (uint32_t i = 0; i < count; ++i) {
		const auto &token = tokens[i];
		if (switch_gpu_backing_token_external_status(&token) == 1)
			continue;
		if (token.engine != SWITCH_GPU_ENGINE_GM20B)
			return false;
		uint32_t id = 0;
		uint32_t threshold = 0;
		uint32_t last = 0;
		const int asked = switch_gpu_timeline_syncpoint(&token, &id, &threshold, &last);
		if (asked == -61) {
			wait.unassigned = true;
			wait.ask = token;
		}
		if (asked != 1 || (covered && id != wait.syncpoint))
			return false;
		if (!covered || int32_t(last - uint32_t(wait.covering.value)) > 0) {
			wait.threshold = threshold;
			wait.covering = token;
			wait.covering.value = last;
		}
		wait.syncpoint = id;
		covered = true;
	}
	return covered;
}

void switch_gpu_producer_assign(const SwitchGpuProducerWait &wait)
{
	if (!wait.unassigned)
		return;
	uint32_t id = kept;
	uint32_t promised = 0;
	kept = 0;
	if (!id && switch_gpu_timeline_syncpt_reclaim(&id, &promised, 1)) {
		switch_gpu_producer_release(id, promised);
	} else if (!id) {
		if (made == SWITCH_GPU_TIMELINE_SYNCPTS || tegra_host1x_syncpt_alloc(&id))
			return;
		++made;
	}
	if (switch_gpu_timeline_syncpt_adopt(&wait.ask, id, tegra_host1x_syncpt_read(id)))
		kept = id;
}

int switch_gpu_producer_covered(const switch_gpu_backing_token &covering, int *stopped)
{
	return switch_gpu_timeline_covered(&covering, uint32_t(covering.value), stopped);
}

uint64_t switch_gpu_producer_check(const switch_gpu_backing_token &covering)
{
	return tgm_writer_check_progress(covering.queue, uint32_t(covering.value), "host1x wait");
}

void switch_gpu_producer_release(uint32_t syncpoint, uint32_t threshold)
{
	const int released = tegra_host1x_syncpt_release_through(syncpoint, threshold);
	if (released)
		IOLog("SwitchGpuBridge: syncpoint %u of a stopped producer stays short of %u "
		      "(%d)\n",
		      syncpoint, threshold, released);
}
