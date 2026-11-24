// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/clock.h>
#include <sys/errno.h>
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include "SwitchWLANA3State.h"

namespace switch_a3_host {
SwitchWLANA3Diagnostics gDiagnostics;
}

namespace {
const char *trialBlockerLocked(uint16_t channel)
{
    using namespace switch_a3_host;
    if (channel != 6 && channel != 44 && channel != 149)
        return "non-social home channel";
    if (!gAttached || !gPowered)
        return "radio detached or unpowered";
    if (gClosing || gRecoveryRequired || gDrainPending)
        return "radio closing, recovering or draining";
    if (gStationPaused)
        return "station owns the radio";
    if (!gState.ready)
        return "firmware resources not ready";
    if (gScheduleUpdatePending)
        return "firmware schedule commit pending";
    if (!gApplied || gApplied != gBound || gApplied != gSchedule)
        return "firmware generations disagree";
    if (!gKnownEpoch || gKnownEpoch != gObservedEpoch)
        return "firmware clock epoch mismatch";
    const uint16_t chanspec = bcm_chanspec20(static_cast<uint8_t>(channel), gD11n);
    if (gAppliedSchedule.infra_chanspec != chanspec)
        return "firmware home channel differs";
    if (gAppliedSchedule.slot_count != SWITCH_A3_MAX_SLOTS)
        return "firmware slot count differs";
    for (const auto &slot : gAppliedSchedule.slots)
        if (slot.chanspec != chanspec || !slot.dwell_us)
            return "firmware has an away or inactive slot";
    return nullptr;
}
}

bool switch_wlan_a3_trial_applied(uint16_t channel, const char **blocked)
{
    using namespace switch_a3_host;
    const char *reason = "radio lock unavailable";
    if (gLock) {
        IOLockLock(gLock);
        reason = trialBlockerLocked(channel);
        IOLockUnlock(gLock);
    }
    if (blocked)
        *blocked = reason;
    return !reason;
}

void switch_wlan_a3_trial_boundary(bool started, uint16_t channel)
{
    using namespace switch_a3_host;
    if (!gLock)
        return;
    IOLockLock(gLock);
    const bool applied = !trialBlockerLocked(channel);
    if (started ? gDiagnostics.trial_generation != 0 : (!gDiagnostics.trial_generation || gDiagnostics.fixed_trial_ended)) {
        IOLockUnlock(gLock);
        return;
    }
    if (started && applied) {
        gDiagnostics.trial_generation = gApplied;
        gDiagnostics.trial_epoch = gKnownEpoch;
        gDiagnostics.trial_lifetime = gLifetime;
        gDiagnostics.trial_channel = channel;
    } else if (!started) {
        gDiagnostics.fixed_trial_ended = true;
    }
    const uint32_t generation = gApplied;
    const bool queued = applied && gKnownEpoch == gDiagnostics.trial_epoch && gLifetime == gDiagnostics.trial_lifetime &&
                        channel == gDiagnostics.trial_channel && gCount < kJobs;
    if (queued) {
        enqueueLocked(SWITCH_A3_GET_STATE, nullptr, 0, false);
        Job &job = gJobs[(gHead + gCount - 1) % kJobs];
        job.control.config_generation = generation;
        job.trial_epoch = gKnownEpoch;
        job.trial_boundary = started ? 1 : 2;
    }
    IOLockUnlock(gLock);
    if (queued)
        thread_call_enter(gWorker);
    else
        IOLog("SwitchWLAN: A3 trial %s snapshot unavailable for config %u\n", started ? "start" : "end", generation);
}

void switch_wlan_a3_echo_snapshot(uint32_t session, uint32_t epoch, uint16_t id, uint16_t sequence)
{
    using namespace switch_a3_host;
    if (!gLock)
        return;
    IOLockLock(gLock);
    if (gAttached && gPowered && gState.ready && session == gSession && epoch && epoch == gKnownEpoch)
        gDiagnostics.echo = {gLifetime, epoch, id, sequence};
    IOLockUnlock(gLock);
}

void switch_wlan_a3_discovery_trial()
{
    using namespace switch_a3_host;
    if (!gLock)
        return;
    IOLockLock(gLock);
    if (gAttached && gPowered && !gDiagnostics.trial_requested)
        gDiagnostics.trial_pending = true;
    IOLockUnlock(gLock);
}
#endif
#include "SwitchWLANA3Clock.h"
#include "SwitchWLANA3Wire.h"
#include "awdl/awdl_radio.h"

namespace {
#if DEVELOPMENT || DEBUG
bool diagnosticRequest(const SwitchWLANA3Job &job, uint64_t now, SwitchWLANA3EchoSnapshot &echo)
{
    using namespace switch_a3_host;
    const bool stateRequest = job.control.operation == SWITCH_A3_GET_STATE;
    if ((!stateRequest && job.control.operation != SWITCH_A3_GET_CLOCK) || !gLock)
        return false;
    IOLockLock(gLock);
    const bool fault = stateRequest && !gState.ready && !gDiagnostics.fault_requested &&
                       (gDiagnostics.fault_pending || gState.error || gRetireError || gRecoveryRequired);
    const bool trial = gState.ready && gDiagnostics.trial_pending && !gDiagnostics.trial_requested;
    const bool boundary = job.trial_boundary != 0;
    const auto &pending = gDiagnostics.echo;
    const bool measured = gState.ready && pending.epoch && pending.epoch == gKnownEpoch && pending.lifetime == gLifetime;
    const bool capture = gAttached && gPowered && job.control.session == gSession && job.lifetime == gLifetime &&
                         (measured || boundary || trial || fault || (gState.ready && switch_a3_diagnostic_due(gDiagnostics, now)));
    if (capture) {
        ++gDiagnostics.requests;
        if (measured) {
            echo = pending;
            gDiagnostics.echo = {};
        }
        if (!boundary) {
            gDiagnostics.trial_requested |= trial;
            if (trial)
                gDiagnostics.trial_pending = false;
            gDiagnostics.fault_requested |= fault;
        }
        gDiagnostics.requested_ns = now;
    }
    IOLockUnlock(gLock);
    return capture;
}

void diagnosticFault(const char *which, const switch_a3_fault_record &fault)
{
    if (!fault.sequence)
        return;
    IOLog("SwitchWLAN: A3 diagnostic cause %s: sequence %u reason %u native %d owner %u/%u/%u "
          "observed/deadline/actual %llu/%llu/%llu us channel %#x/%#x\n",
          which, fault.sequence, fault.reason, fault.native_error, fault.session, fault.epoch, fault.config,
          static_cast<unsigned long long>(fault.observed_tsf), static_cast<unsigned long long>(fault.deadline_tsf),
          static_cast<unsigned long long>(fault.actual_tsf), fault.target_chanspec, fault.actual_chanspec);
}

void diagnosticReply(const SwitchWLANA3Job &job, const switch_a3_reply &reply, const void *data, size_t bytes,
                     const SwitchWLANA3EchoSnapshot &echo)
{
    using namespace switch_a3_host;
    struct switch_a3_diagnostic snapshot = {};
    const bool valid = switch_a3_diagnostic_decode(data, bytes, reply, snapshot);
    IOLockLock(gLock);
    const bool current = gAttached && gPowered && job.control.session == gSession && job.lifetime == gLifetime &&
                         (!gKnownEpoch || static_cast<int32_t>(reply.clock_epoch - gKnownEpoch) >= 0);
    const bool stationActive = gState.ready && !gStationPaused && !gClosing && !gDrainPending;
    const SwitchWLANA3Diagnostics host = gDiagnostics;
    IOLockUnlock(gLock);
    if (echo.epoch) {
        const bool matched = current && valid && !reply.status && echo.epoch == reply.clock_epoch;
        IOLog("SwitchWLAN: A3 echo snapshot id %u seq %u %s request %u session %u epoch %u applied %u TSF %llu\n",
              echo.id, echo.sequence, matched ? "matched" : "unavailable", job.control.request_id, reply.session,
              reply.clock_epoch, reply.applied_generation, static_cast<unsigned long long>(reply.tsf));
    }
    if (job.trial_boundary) {
        const bool matched = current && valid && reply.state == SWITCH_A3_RUNNING && !reply.fault_status &&
                             reply.applied_generation == job.control.config_generation && reply.clock_epoch == job.trial_epoch;
        IOLog("SwitchWLAN: A3 trial %s snapshot %s request %u session %u epoch %u config %u firmware TSF %llu\n",
              job.trial_boundary == 1 ? "start" : "end", matched ? "matched" : "unavailable", job.control.request_id,
              job.control.session, job.trial_epoch, job.control.config_generation, static_cast<unsigned long long>(reply.tsf));
        if (!matched)
            return;
    }
    if (!current)
        return;
    if (!valid) {
        IOLog("SwitchWLAN: A3 diagnostic unavailable: session %u epoch %u generation %u, %u bytes\n",
              reply.session, reply.clock_epoch, reply.applied_generation, static_cast<unsigned>(bytes));
        return;
    }
    const uint32_t *c = snapshot.counters;
    IOLog("SwitchWLAN: A3 diagnostic TX: session %u epoch %u config %u revision %u sample %llu us, "
          "ingress %u native %u physical-status %u export-drop %u\n",
          snapshot.session, snapshot.clock_epoch, reply.applied_generation,
          reply.state_revision, static_cast<unsigned long long>(reply.tsf), c[SWITCH_A3_DIAG_TX_INGRESS],
          c[SWITCH_A3_DIAG_TX_NATIVE], c[SWITCH_A3_DIAG_TX_STATUS], c[SWITCH_A3_DIAG_TX_STATUS_EXPORT_DROP]);
    IOLog("SwitchWLAN: A3 diagnostic channel: session %u epoch %u schedule blocks %u "
          "fault %u, PM request %u packet %u physical-ack %u rejected %u status %#x reason %u gate allow %u deny %u; "
          "excursions start %u enter %u return %u\n",
          snapshot.session, snapshot.clock_epoch, c[SWITCH_A3_DIAG_SCHED_BLOCK], c[SWITCH_A3_DIAG_SCHED_FAULT],
          snapshot.pm_requests, snapshot.pm_packets, snapshot.pm_acks, snapshot.pm_failed,
          snapshot.pm_status, snapshot.pm_reason,
          c[SWITCH_A3_DIAG_GATE_ALLOW], c[SWITCH_A3_DIAG_GATE_DENY], c[SWITCH_A3_DIAG_EXCURSION_START],
          c[SWITCH_A3_DIAG_EXCURSION_ENTER], c[SWITCH_A3_DIAG_EXCURSION_RETURN]);
    const auto &beacon = snapshot.beacon;
    if (stationActive && beacon.chanspec && beacon.radio_tsf && beacon.radio_tsf <= reply.tsf)
        IOLog("SwitchWLAN: A3 AP beacon: session %u epoch %u config %u bssid %02x:%02x:%02x:%02x:%02x:%02x "
              "channel %#x AP %llu RX %llu us\n", snapshot.session, snapshot.clock_epoch, reply.applied_generation,
              beacon.bssid[0], beacon.bssid[1], beacon.bssid[2], beacon.bssid[3], beacon.bssid[4], beacon.bssid[5],
              beacon.chanspec, static_cast<unsigned long long>(beacon.ap_tsf), static_cast<unsigned long long>(beacon.radio_tsf));
    if (snapshot.dwell_owner.session && snapshot.dwell_entered && snapshot.dwell_entered <= snapshot.dwell_returned)
        IOLog("SwitchWLAN: A3 observed dwell: session %u epoch %u config %u channel %#x RX-ready %llu last-before-return %llu us\n",
              snapshot.dwell_owner.session, snapshot.dwell_owner.epoch, snapshot.dwell_owner.generation, snapshot.dwell_channel,
              static_cast<unsigned long long>(snapshot.dwell_entered), static_cast<unsigned long long>(snapshot.dwell_returned));
    IOLog("SwitchWLAN: A3 diagnostic RX: session %u epoch %u hook %u header %u phy %u action %u prefix-PSF %u "
          "prefix-MIF %u bssid %u destination %u timestamp %u split %u export %u "
          "drop %u fetch-failed %u; host records %u unready %u invalid %u stale %u unbound %u delivered %u; "
          "TX records %u rejected %u sent %u expired %u cancelled %u failed %u ack-missing %u\n",
          snapshot.session, snapshot.clock_epoch, c[SWITCH_A3_DIAG_RX_HOOK], c[SWITCH_A3_DIAG_RX_HEADER],
          c[SWITCH_A3_DIAG_RX_PHY], c[SWITCH_A3_DIAG_RX_ACTION], c[SWITCH_A3_DIAG_RX_PSF], c[SWITCH_A3_DIAG_RX_MIF],
          c[SWITCH_A3_DIAG_RX_BSSID], c[SWITCH_A3_DIAG_RX_DEST],
          c[SWITCH_A3_DIAG_RX_MATCH], c[SWITCH_A3_DIAG_RX_SPLIT], c[SWITCH_A3_DIAG_RX_EXPORT],
          c[SWITCH_A3_DIAG_RX_DROP], c[SWITCH_A3_DIAG_RX_FETCH_FAILED], host.rx_records, host.rx_unready,
          host.rx_invalid, host.rx_stale, host.rx_unbound, host.rx_delivered, host.tx_records, host.tx_rejected,
          host.tx_sent, host.tx_expired, host.tx_cancelled, host.tx_failed, host.tx_ack_missing);
    diagnosticFault("first", snapshot.first_fault);
    diagnosticFault("latest", snapshot.latest_fault);
}

#endif

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

int firmwareError(int status)
{
    switch (status) {
    case 0:
        return 0;
    case -2:
        return -EINVAL;
    case -14:
        return -EMSGSIZE;
    case -16:
        return -EBUSY;
    case -22:
        return -ENOBUFS;
    case -23:
        return -EOPNOTSUPP;
    case -25:
        return -EAGAIN;
    case -27:
        return -ENOMEM;
    case -29:
        return -ERANGE;
    case -35:
        return -ESTALE;
    default:
        return -EIO;
    }
}

bool sampleOf(const switch_a3_reply &reply, uint64_t before, uint64_t after, swlan_awdl_clock_sample &sample)
{
    if (after < before || reply.tsf > INT64_MAX || before > INT64_MAX || after > INT64_MAX ||
        !reply.firmware_generation || after - before >= 2 * (SWITCH_AWDL_CLOCK_UNCERTAINTY_NS - 4000ULL))
        return false;
    const uint64_t span = after - before;
    sample = {reply.tsf, before + span / 2, static_cast<uint32_t>(span / 2 + 4000), reply.clock_epoch};
    return true;
}

bool clockOutlier(const swlan_awdl_clock_sample &previous, const swlan_awdl_clock_sample &sample, bool &forward)
{
    if (!previous.host_ns || previous.epoch != sample.epoch || sample.host_ns <= previous.host_ns)
        return false;
    const uint64_t elapsed = sample.host_ns - previous.host_ns;
    const uint64_t predicted = previous.tsf_us + elapsed / 1000;
    forward = sample.tsf_us > predicted;
    const uint64_t residual = forward ? sample.tsf_us - predicted : predicted - sample.tsf_us;
    const uint64_t tolerance =
        switch_awdl_radio_clock_tolerance_us(previous.uncertainty_ns, sample.uncertainty_ns, elapsed);
    return residual > SWITCH_AWDL_CLOCK_JUMP_US && residual > tolerance;
}

bool clocksAgree(const swlan_awdl_clock_sample &first, const swlan_awdl_clock_sample &second)
{
    if (first.epoch != second.epoch || second.host_ns <= first.host_ns)
        return false;
    const uint64_t elapsed = second.host_ns - first.host_ns;
    const uint64_t predicted = first.tsf_us + elapsed / 1000;
    const uint64_t residual = second.tsf_us > predicted ? second.tsf_us - predicted : predicted - second.tsf_us;
    return residual <= switch_awdl_radio_clock_tolerance_us(first.uncertainty_ns, second.uncertainty_ns, elapsed);
}
}

bool switch_wlan_a3_clock_valid(const swlan_awdl_clock_sample &sample, uint64_t now_ns)
{
    return sample.epoch && sample.host_ns && sample.uncertainty_ns < SWITCH_AWDL_CLOCK_UNCERTAINTY_NS &&
           now_ns >= sample.host_ns &&
           now_ns - sample.host_ns <
               (SWITCH_AWDL_CLOCK_UNCERTAINTY_NS - sample.uncertainty_ns) * 1000000ULL / SWITCH_AWDL_CLOCK_DRIFT_PPM;
}

int switch_wlan_a3_firmware_error(int status)
{
    return firmwareError(status);
}

int switch_wlan_a3_exchange(const SwitchWLANA3Job &job, switch_a3_reply &reply, uint64_t &before, uint64_t &after)
{
#if DEVELOPMENT || DEBUG
    uint8_t response[sizeof(reply) + sizeof(struct switch_a3_diagnostic)] = {};
    SwitchWLANA3EchoSnapshot echo = {};
    const bool diagnostic = diagnosticRequest(job, hostNs(), echo);
    const size_t capacity = diagnostic ? sizeof response : sizeof reply;
#else
    uint8_t response[sizeof(reply)] = {};
    const size_t capacity = sizeof response;
#endif
    size_t returned = 0;
    before = hostNs();
    int error = switch_wlan_a3_control(&job, job.control.total_bytes, response, capacity, &returned);
    after = hostNs();
    size_t replyBytes = returned;
#if DEVELOPMENT || DEBUG
    if (diagnostic && returned >= sizeof reply && returned <= capacity)
        replyBytes = sizeof reply;
#endif
    const bool decoded = !error && switch_a3_reply_decode(response, replyBytes, &job.control, &reply);
    if (!error) {
        if (decoded) {
            error = firmwareError(reply.status);
        } else {
            reply = {};
            error = -EPROTO;
        }
    }
#if DEVELOPMENT || DEBUG
    if (diagnostic && decoded)
        diagnosticReply(job, reply, response + sizeof reply, returned - sizeof reply, echo);
    else if (echo.epoch)
        IOLog("SwitchWLAN: A3 echo snapshot id %u seq %u unavailable request %u session %u epoch %u error %d\n",
              echo.id, echo.sequence, job.control.request_id, job.control.session, echo.epoch, error);
    if (error)
        IOLog("SwitchWLAN: A3 control failed: operation %u request %u session %u epoch %u generation %u, result %d "
              "after %llu us (%u reply bytes)\n",
              job.control.operation, job.control.request_id, job.control.session, job.control.clock_epoch,
              job.control.config_generation, error, static_cast<unsigned long long>((after - before) / 1000),
              static_cast<unsigned>(returned));
#endif
    return error;
}

int switch_wlan_a3_calibrate(SwitchWLANA3Job job, const swlan_awdl_clock_sample &previous,
                             swlan_awdl_clock_sample &sample, switch_a3_reply &reply,
                             bool (*current)(uint32_t, uint32_t, uint32_t))
{
    uint64_t bestSpan = UINT64_MAX;
    uint32_t epoch = 0;
    int error = -EAGAIN;
    swlan_awdl_clock_sample observations[3] = {};
    unsigned observed = 0;
    job.control.clock_epoch = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!current(job.control.session, job.lifetime, 0))
            return -ECANCELED;
        switch_a3_reply candidate = {};
        uint64_t before = 0, after = 0;
        const int result = switch_wlan_a3_exchange(job, candidate, before, after);
        if (candidate.state_revision) {
            if (reply.state_revision && static_cast<int32_t>(candidate.state_revision - reply.state_revision) <= 0)
                continue;
            reply = candidate;
            if (epoch != candidate.clock_epoch) {
                epoch = candidate.clock_epoch;
                bestSpan = UINT64_MAX;
                sample = {};
                observed = 0;
                error = -EAGAIN;
            }
        }
        if (result) {
            if (bestSpan == UINT64_MAX)
                error = result;
            continue;
        }
        swlan_awdl_clock_sample measured = {};
        if (sampleOf(candidate, before, after, measured)) {
            observations[observed++] = measured;
            if (after - before < bestSpan) {
                bestSpan = after - before;
                sample = measured;
                error = 0;
            }
        }
    }
    if (error || previous.epoch != sample.epoch || !previous.host_ns || sample.host_ns <= previous.host_ns)
        return error;
    bool confirmed = false;
    swlan_awdl_clock_sample accepted = {};
    for (unsigned i = 0; i < observed; ++i) {
        bool forward = false;
        if (!clockOutlier(previous, observations[i], forward)) {
            if (!accepted.epoch || observations[i].uncertainty_ns < accepted.uncertainty_ns)
                accepted = observations[i];
            continue;
        }
        for (unsigned j = i + 1; j < observed; ++j) {
            bool nextForward = false;
            if (clockOutlier(previous, observations[j], nextForward) && forward == nextForward &&
                clocksAgree(observations[i], observations[j]))
                confirmed = true;
        }
    }
    if (!confirmed) {
        sample = accepted;
        return accepted.epoch ? 0 : -EAGAIN;
    }
    if (!current(job.control.session, job.lifetime, reply.state_revision))
        return -ESTALE;
    SwitchWLANA3Job reset = job;
    reset.control.operation = SWITCH_A3_RESET_CLOCK;
    uint64_t before = 0, after = 0;
    error = switch_wlan_a3_exchange(reset, reply, before, after);
    if (!error && !current(job.control.session, job.lifetime, 0))
        error = -ECANCELED;
    if (!error)
        error = switch_wlan_a3_exchange(job, reply, before, after);
    if (!error && !sampleOf(reply, before, after, sample))
        error = -EAGAIN;
    return error;
}
