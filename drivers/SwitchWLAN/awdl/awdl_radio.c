/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "awdl_radio.h"

#include <string.h>

int switch_awdl_radio_clock_request_begin(struct switch_awdl_radio_clock_request *request, int independent)
{
    if (independent)
        request->blocked = 0;
    if (request->pending || request->blocked)
        return 0;
    request->pending = 1;
    return 1;
}

void switch_awdl_radio_clock_request_complete(struct switch_awdl_radio_clock_request *request, int error)
{
    request->pending = 0;
    request->blocked = error != 0;
}

uint64_t switch_awdl_radio_clock_tolerance_us(uint32_t previous_uncertainty_ns, uint32_t uncertainty_ns,
                                             uint64_t elapsed_ns)
{
    const uint64_t drift_ns = elapsed_ns / 1000000ULL * SWITCH_AWDL_CLOCK_DRIFT_PPM +
                              (elapsed_ns % 1000000ULL * SWITCH_AWDL_CLOCK_DRIFT_PPM + 999999ULL) / 1000000ULL;
    return (previous_uncertainty_ns + (uint64_t)uncertainty_ns + drift_ns + 999ULL) / 1000ULL;
}

uint64_t switch_awdl_radio_clock_expires(const struct switch_awdl_radio_clock *clock)
{
    if (!clock->affine.valid || clock->uncertainty_ns > SWITCH_AWDL_CLOCK_UNCERTAINTY_NS)
        return 0;
    /* Assume at most 500 ppm oscillator error between samples; age consumes the remaining slot guard. */
    const uint64_t budget =
        (SWITCH_AWDL_CLOCK_UNCERTAINTY_NS - clock->uncertainty_ns) * 1000000ULL / SWITCH_AWDL_CLOCK_DRIFT_PPM;
    return UINT64_MAX - clock->affine.host0_ns < budget ? UINT64_MAX : clock->affine.host0_ns + budget;
}

int switch_awdl_radio_clock_valid(const struct switch_awdl_radio_clock *clock, uint64_t now_ns)
{
    return clock->affine.valid && now_ns >= clock->affine.host0_ns && now_ns < switch_awdl_radio_clock_expires(clock);
}

uint64_t switch_awdl_radio_clock_deadline(const struct switch_awdl_radio_clock *clock)
{
    const uint64_t expires = switch_awdl_radio_clock_expires(clock);
    if (!expires || clock->renewal_attempted)
        return expires;
    const uint64_t lead = (expires - clock->affine.host0_ns) / 4;
    return expires - (lead < 100000000ULL ? lead : 100000000ULL);
}

int switch_awdl_radio_clock_try_renew(struct switch_awdl_radio_clock *clock, uint64_t now_ns)
{
    if (clock->renewal_attempted || !switch_awdl_radio_clock_valid(clock, now_ns) ||
        now_ns < switch_awdl_radio_clock_deadline(clock))
        return 0;
    clock->renewal_attempted = 1;
    return 1;
}

void switch_awdl_radio_clock_clear(struct switch_awdl_radio_clock *clock)
{
    memset(clock, 0, sizeof *clock);
}

int switch_awdl_radio_clock_update(struct switch_awdl_radio_clock *clock, uint64_t tsf, uint64_t host_ns,
                                   uint32_t uncertainty_ns, uint32_t epoch, uint64_t now_ns)
{
    if (epoch && clock->affine.valid && epoch != clock->epoch)
        switch_awdl_radio_clock_clear(clock);
    if (!epoch || epoch == clock->invalid_epoch || !host_ns || host_ns > now_ns ||
        uncertainty_ns >= SWITCH_AWDL_CLOCK_UNCERTAINTY_NS ||
        now_ns - host_ns >= (SWITCH_AWDL_CLOCK_UNCERTAINTY_NS - uncertainty_ns) * 2000ULL || tsf > INT64_MAX ||
        host_ns > INT64_MAX)
        return -1;
    const int had_clock = clock->affine.valid;
    const int epoch_changed = had_clock && epoch != clock->epoch;
    if (had_clock && !epoch_changed && host_ns <= clock->affine.host0_ns)
        return host_ns == clock->affine.host0_ns && tsf == clock->affine.tsf0 ? 0 : -1;
    int changed = !had_clock || epoch_changed;
    if (had_clock && !epoch_changed) {
        const uint64_t predicted = awdl_tsf_clock_now(&clock->affine, host_ns);
        const uint64_t error = tsf > predicted ? tsf - predicted : predicted - tsf;
        const uint64_t tolerance = switch_awdl_radio_clock_tolerance_us(
            clock->uncertainty_ns, uncertainty_ns, host_ns - clock->affine.host0_ns);
        if (error > SWITCH_AWDL_CLOCK_JUMP_US && error > tolerance)
            return -1;
    }
    if (changed)
        memset(&clock->affine, 0, sizeof clock->affine);
    awdl_tsf_clock_anchor(&clock->affine, tsf, host_ns);
    clock->epoch = epoch;
    clock->invalid_epoch = 0;
    clock->uncertainty_ns = uncertainty_ns;
    clock->renewal_attempted = 0;
    return changed;
}
