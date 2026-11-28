// SPDX-License-Identifier: BSD-2-Clause-Patent
// The illegal-opcode probe (SwitchHost1xErrorProbe.h), debug kernels only.
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#include <IOKit/IOLib.h>
#include "SwitchHost1xErrorProbe.h"
#include "TegraHost1x.h"

namespace {
constexpr uint32_t kChannel = SWITCH_HOST1X_ERROR_PROBE_CHANNEL, kDeliveryMs = 100, kRunUs = 100000;

// After the restart: one syncpoint increment on the channel, and the wait for it.
bool runsAgain()
{
    uint32_t id = 0;
    if (tegra_host1x_syncpt_alloc(&id))
        return false;
    const uint32_t base = tegra_host1x_syncpt_read(id);
    const uint32_t words[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                               tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id)};
    const bool ran = !tegra_host1x_submit_on(kChannel, words, 2) && !tegra_host1x_syncpt_wait(id, base + 1, kRunUs);
    if (!ran) {
        tegra_host1x_channel_teardown(kChannel);
        tegra_host1x_channel_restart(kChannel);
    }
    tegra_host1x_syncpt_free(id);
    return ran;
}
}  // namespace

extern "C" int switch_host1x_error_probe_run(void *out)
{
    auto *reply = static_cast<SwitchHost1xErrorProbeReply *>(out);
    *reply = {};
    reply->magic = SWITCH_HOST1X_ERROR_PROBE_MAGIC;
    reply->channel = kChannel;
    reply->opcode = SWITCH_HOST1X_ERROR_PROBE_OPCODE;
    reply->attached = switch_host1x_errors_attached();
    if (reply->attached) {
        reply->statBefore = switch_host1x_errors_stat();
        const uint32_t illegal = SWITCH_HOST1X_ERROR_PROBE_OPCODE << 28;
        switch_host1x_errors_expect(kChannel);
        reply->status = tegra_host1x_submit_on(kChannel, &illegal, 1);
        reply->taken = switch_host1x_errors_expect_end(kDeliveryMs);
        reply->statStopped = switch_host1x_errors_stat();
        reply->stopped = switch_host1x_errors_line();
        tegra_host1x_channel_teardown(kChannel);
        tegra_host1x_channel_restart(kChannel);
        reply->statRestarted = switch_host1x_errors_stat();
        reply->recovered = !reply->status && runsAgain();
        reply->after = switch_host1x_errors_line();
    }
    IOLog("TegraHost1x: illegal-opcode probe on channel %u: attached %u, opcode %u delivered %u times, CMDPROC_STAT "
          "%#x, %#x stopped, %#x restarted, %s after the restart (status %d)\n",
          kChannel, reply->attached, reply->opcode, reply->taken, reply->statBefore, reply->statStopped,
          reply->statRestarted, reply->recovered ? "a stream ran" : "no stream ran", reply->status);
    return 0;
}
#endif
