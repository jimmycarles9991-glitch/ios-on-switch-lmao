// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include "SwitchWLANA3State.h"
#include "SwitchWLANA3Wire.h"
#include "SwitchWLANA3Clock.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace switch_a3_host {
IOLock *gLock;
thread_call_t gWorker, gTxExpiry;
Job gJobs[kJobs];
TxJob gTx[kJobs];
PendingTx gPending[kJobs];
swlan_awdl_event gCancelled[kJobs];
unsigned gCancelCount;
unsigned gHead, gCount;
unsigned gTxHead, gTxCount, gDataAckStreak;
uint8_t gDataRateIndex = kDataRateMax;
bool gBusy, gAttached, gClockQueued, gPowered, gRecoveryRequired;
bool gDmaBlocked;
bool gDrainDmaWait;
bool gDrainPending, gDrainWaitEvent, gClosing, gHaveConfiguration, gHaveSchedule, gStopped, gStationPaused, gPauseReady;
bool gVerifyState;
uint32_t gOwnershipEvent, gDrainEvent;
uint64_t gDrainDeadlineNs;
switch_a3_configuration gDesiredConfiguration;
switch_a3_schedule gDesiredSchedule;
uint32_t gSession, gRequest, gConfiguration, gApplied, gFeatures, gSchedule, gAddressGeneration, gFirmwareGeneration;
uint32_t gLifetime;
uint32_t gKnownEpoch;
uint32_t gBound, gCreditLimit;
uint32_t gFirmwareOwnerGeneration;
uint32_t gStateRevision, gObservedEpoch;
switch_a3_ring_binding gRings;
bool gConfigured;
int gClockError;
int gRetireError;
int gD11n;
uint8_t gMac[6];
swlan_awdl_state gState;
swlan_rx_fn gRx;
void *gRxContext;

unsigned pendingCount()
{
    unsigned count = 0;
    for (const auto &pending : gPending)
        count += pending.id != 0;
    return count;
}

uint32_t creditRoom()
{
    if (gScheduleUpdatePending || gScheduleRejected || gStationPaused)
        return 0;
    const unsigned pending = pendingCount();
    return pending < gCreditLimit ? gCreditLimit - pending : 0;
}

bool observeReply(const switch_a3_reply &reply)
{
    if (gStateRevision && static_cast<int32_t>(reply.state_revision - gStateRevision) <= 0)
        return false;
    gStateRevision = reply.state_revision;
    gObservedEpoch = reply.clock_epoch;
    gFeatures = reply.features;
    gFirmwareGeneration = reply.firmware_generation;
    gState.social_channels = reply.legal_social_channels;
    gState.home_chanspec = reply.home_chanspec;
    gState.social_channels_valid = 1;
    return true;
}

void updateCredits(uint32_t credits)
{
    if (!gState.ready) {
        gState.tx_credits = 0;
        return;
    }
    const uint32_t limit = credits + pendingCount();
    gCreditLimit = limit < gRings.credits ? limit : gRings.credits;
    const uint32_t room = creditRoom();
    gState.tx_credits = credits < room ? credits : room;
}

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

void emitRadioEvent(uint32_t session, uint32_t type, uint32_t id, uint16_t status, uint64_t tsf)
{
    swlan_awdl_event event = {};
    IOLockLock(gLock);
    const bool current = gAttached && session == gSession;
    event = {type, id, status, tsf, session, gState.clock.epoch};
#if DEVELOPMENT || DEBUG
    const bool changed = current && gDiagnostics.transitions < 32 &&
                         (!gDiagnostics.transitions || gDiagnostics.last_ready != gState.ready ||
                          gDiagnostics.last_epoch != gState.clock.epoch || gDiagnostics.last_error != gState.error);
    const swlan_awdl_state observed = gState;
    const uint32_t revision = gStateRevision, applied = gApplied;
    const bool updating = gScheduleUpdatePending, paused = gStationPaused;
    if (changed) {
        ++gDiagnostics.transitions;
        gDiagnostics.last_ready = gState.ready;
        gDiagnostics.last_epoch = gState.clock.epoch;
        gDiagnostics.last_error = gState.error;
    }
#endif
    IOLockUnlock(gLock);
#if DEVELOPMENT || DEBUG
    if (changed)
        IOLog("SwitchWLAN: A3 resource transition: event %u status %u ready %u error %d owner %u/%u/%u "
              "revision %u clock-valid %u schedule-pending %u station-parked %u\n",
              type, status, observed.ready, observed.error, session, observed.clock.epoch, applied, revision,
              switch_wlan_a3_clock_valid(observed.clock, hostNs()), updating, paused);
#endif
    if (current)
        switch_wlan_a3_notify(&event);
}

void deferDesired(bool waitForOwnership, uint32_t observedEvent)
{
    resetScheduleUpdate();
    const bool owned = gFirmwareOwnerGeneration != 0;
    const bool retired = owned && gStopped && (!waitForOwnership || gOwnershipEvent != observedEvent);
    const bool waiting = owned && !retired && (waitForOwnership || gDrainWaitEvent);
    ++gLifetime;
    gCount = gHead = 0;
    gClockQueued = false;
    gDrainPending = owned || (gHaveConfiguration && !gClosing && !gStationPaused);
    // The ioctl owns its timeout; radio retirement begins only after STOP answers.
    if (!owned)
        gDrainDeadlineNs = 0;
    else if (waiting && !gDrainDeadlineNs)
        gDrainDeadlineNs = hostNs() + 1500000000ULL;
    if (waitForOwnership && !gDrainWaitEvent)
        gDrainEvent = observedEvent;
    gDrainWaitEvent = waiting;
    gState.ready = 0;
    gState.error = 0;
    gClockError = 0;
    gConfigured = false;
    gApplied = gSchedule = gBound = 0;
    gStopped = retired;
    gPauseReady = gStationPaused && !owned;
    cancelTx();
    if (gPauseReady)
        IOLockWakeup(gLock, &gStationPaused, false);
    if (owned && gAttached && gPowered && !gRecoveryRequired && !gDrainWaitEvent && !gStopped)
        enqueueLocked(SWITCH_A3_STOP, nullptr, 0, false);
}

void replayDesired()
{
    gDrainPending = gDrainWaitEvent = false;
    armTxExpiry();
    if (gClosing || gStationPaused) {
        gPauseReady = gStationPaused;
        IOLockWakeup(gLock, &gStationPaused, false);
        return;
    }
    if (gRetireError) {
        gState.error = gRetireError;
        return;
    }
    if (!gHaveConfiguration)
        return;
    enqueueLocked(SWITCH_A3_CONFIGURE, &gDesiredConfiguration, sizeof gDesiredConfiguration, true);
    if (gHaveSchedule && gState.social_channels_valid && gState.social_channels) {
        enqueueLocked(SWITCH_A3_INSTALL_SCHEDULE, &gDesiredSchedule, sizeof gDesiredSchedule, true);
        enqueueLocked(SWITCH_A3_BIND_RINGS, &gRings, sizeof gRings, false);
        enqueueLocked(SWITCH_A3_START, nullptr, 0, false);
    }
}

bool isCurrentCalibration(uint32_t session, uint32_t lifetime, uint32_t revision)
{
    IOLockLock(gLock);
    const bool active = gAttached && gPowered && session == gSession && lifetime == gLifetime &&
                        (!revision || !gStateRevision || static_cast<int32_t>(revision - gStateRevision) >= 0);
    IOLockUnlock(gLock);
    return active;
}

bool stoppedForOwner(const switch_a3_reply &reply, bool directStop)
{
    return reply.state == SWITCH_A3_STOPPED && !reply.applied_generation &&
           ((directStop && !reply.state_generation) ||
            (reply.state_generation >= gFirmwareOwnerGeneration && reply.state_generation <= gConfiguration));
}

bool applySnapshot(const switch_a3_reply &reply)
{
    const bool changedEpoch = gKnownEpoch && gKnownEpoch != reply.clock_epoch;
    gKnownEpoch = reply.clock_epoch;
    if (changedEpoch)
        gState.clock = {};
    const int error = switch_wlan_a3_firmware_error(reply.fault_status);
    if (gClockError && !error) {
        if (gState.error == gClockError)
            gState.error = 0;
        gClockError = 0;
    }
    if (gDrainPending && stoppedForOwner(reply)) {
        gStopped = true;
        gDrainWaitEvent = false;
        gApplied = 0;
    } else if (!gDrainPending && gConfigured && gSchedule == gApplied && gBound == gApplied &&
               !error && reply.legal_social_channels && reply.state == SWITCH_A3_RUNNING &&
               reply.applied_generation == gApplied &&
               (reply.features & SWITCH_A3_FEATURE_REQUIRED) == SWITCH_A3_FEATURE_REQUIRED &&
               gState.clock.epoch == reply.clock_epoch) {
        gApplied = reply.applied_generation;
        gState.ready = 1;
        gState.error = 0;
    } else if (!gDrainPending && gFirmwareOwnerGeneration && gHaveConfiguration && !gClosing && !gStationPaused &&
               !gRetireError) {
        const bool terminal = error && error != -EBUSY && error != -EAGAIN && error != -ESTALE;
        gRetireError = terminal ? error : 0;
        deferDesired(false, gOwnershipEvent);
        gState.error = gRetireError;
        if (terminal)
            IOLog("SwitchWLAN: A3 firmware fault %d while retiring session %u configuration %u\n",
                  reply.fault_status, gSession, reply.state_generation);
    }
    updateCredits(reply.tx_credits);
    return changedEpoch;
}

void calibrate(const Job &job)
{
    IOLockLock(gLock);
    const swlan_awdl_clock_sample previous = gState.clock;
    const uint32_t observedEvent = gOwnershipEvent;
    IOLockUnlock(gLock);
    swlan_awdl_clock_sample sample = {};
    switch_a3_reply reply = {};
    int error = switch_wlan_a3_calibrate(job, previous, sample, reply, isCurrentCalibration);
    IOLockLock(gLock);
    const bool current = gAttached && gPowered && !gRecoveryRequired && job.control.session == gSession &&
                         job.lifetime == gLifetime;
    if (current) {
        gClockQueued = false;
        const bool fresh = reply.state_revision && observeReply(reply);
        const bool epochChanged = gKnownEpoch && gKnownEpoch != gObservedEpoch;
        const bool superseded = reply.state_revision && !fresh;
        const bool firmwareChanged = fresh && gState.ready &&
                                     (reply.state != SWITCH_A3_RUNNING || !scheduleGenerationCurrent(reply.applied_generation) ||
                                      !reply.legal_social_channels || reply.fault_status ||
                                      (reply.features & SWITCH_A3_FEATURE_REQUIRED) != SWITCH_A3_FEATURE_REQUIRED);
        if ((!error && (sample.epoch != gObservedEpoch || (superseded && epochChanged))) ||
            (error && reply.state_revision && (superseded || epochChanged))) {
            if (gState.clock.epoch != gObservedEpoch) {
                gState.clock = {};
                gState.ready = gState.tx_credits = 0;
            }
            gVerifyState = true;
            error = -ESTALE;
        } else if (!error) {
            gKnownEpoch = sample.epoch;
            gState.clock = sample;
            if (fresh && !gFirmwareOwnerGeneration && reply.applied_generation) {
                gFirmwareOwnerGeneration = reply.applied_generation;
                gStopped = false;
            }
            if (epochChanged || firmwareChanged) {
                if (fresh && reply.fault_status) {
                    gState.ready = gState.tx_credits = 0;
                    gVerifyState = true;
                } else {
                    deferDesired(false, observedEvent);
                }
            }
            if (gFirmwareOwnerGeneration && gDrainPending && !gStopped && !gCount && !gDrainWaitEvent)
                enqueueLocked(SWITCH_A3_STOP, nullptr, 0, false);
            if (gClockError && gState.error == gClockError)
                gState.error = 0;
            gClockError = 0;
            if (!gFirmwareOwnerGeneration && !gDrainPending && gHaveConfiguration && !gClosing &&
                !gStationPaused && !gRetireError)
                gDrainPending = true;
        } else if (firmwareChanged) {
            gState.ready = gState.tx_credits = 0;
            gVerifyState = true;
        } else if (!gState.ready || previous.epoch != gObservedEpoch ||
                   !switch_wlan_a3_clock_valid(previous, hostNs())) {
            gClockError = error;
            gState.tx_credits = 0;
            if (!gFirmwareOwnerGeneration)
                gDrainPending = false;
        }
    }
    IOLockUnlock(gLock);
    if (current)
        emitRadioEvent(job.control.session, SWLAN_AWDL_CLOCK_CHANGED, job.control.request_id,
                       static_cast<uint16_t>(-error), sample.tsf_us);
}

static Job makeJob(uint32_t operation, const void *payload, size_t length)
{
    Job job = {};
    job.lifetime = gLifetime;
    job.control.magic = SWITCH_A3_CONTROL_MAGIC;
    job.control.version = SWITCH_A3_WIRE_VERSION;
    job.control.header_bytes = sizeof(job.control);
    job.control.total_bytes = static_cast<uint32_t>(sizeof(job.control) + length);
    if (++gRequest == 0)
        ++gRequest;
    job.control.request_id = gRequest;
    job.control.session = gSession;
    job.control.clock_epoch = operation == SWITCH_A3_GET_CLOCK || operation == SWITCH_A3_GET_STATE ||
                                      operation == SWITCH_A3_STOP
                                  ? 0 : gState.clock.epoch;
    job.control.config_generation = gConfiguration;
    job.control.operation = operation;
    if (length)
        memcpy(job.payload, payload, length);
    return job;
}

void work(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    if (gBusy) {
        IOLockUnlock(gLock);
        return;
    }
    gBusy = true;
    while (gCount || gTxCount || gCancelCount || gDrainPending || gVerifyState || gScheduleUpdatePending) {
        if (gCancelCount) {
            const swlan_awdl_event event = gCancelled[--gCancelCount];
            const bool current = gAttached && event.generation == gSession;
            IOLockUnlock(gLock);
            if (current)
                switch_wlan_a3_notify(&event);
            IOLockLock(gLock);
            continue;
        }
        if (gVerifyState && !gCount && (!gDrainPending || gDrainDeadlineNs != kDrainExpired)) {
            gVerifyState = false;
            if (gAttached && gPowered && !gRecoveryRequired)
                enqueueLocked(SWITCH_A3_GET_STATE, nullptr, 0, false);
            else
                continue;
        }
        if (gDrainPending && !gCount && gFirmwareOwnerGeneration) {
            const bool final = gDrainDeadlineNs == kDrainExpired;
            const uint32_t session = gSession;
            if (final && !gStopped) {
                const uint32_t owner = gFirmwareOwnerGeneration;
                const Job check = makeJob(SWITCH_A3_GET_STATE, nullptr, 0);
                gVerifyState = false;
                IOLockUnlock(gLock);
                switch_a3_reply reply = {};
                uint64_t before = 0, after = 0;
                const int error = switch_wlan_a3_exchange(check, reply, before, after);
                IOLockLock(gLock);
                // Desired lifetime changes leave this native owner in place until the worker drains it.
                if (!gAttached || !gPowered || gRecoveryRequired || session != gSession ||
                    owner != gFirmwareOwnerGeneration || !gDrainPending || gDrainDeadlineNs != kDrainExpired)
                    continue;
                if (!error && observeReply(reply) && stoppedForOwner(reply))
                    applySnapshot(reply);
            }
            if (!final && !gStopped) {
                gBusy = false;
                IOLockUnlock(gLock);
                return;
            }
            int idle = 0;
            if (gStopped) {
                const uint32_t lifetime = gLifetime;
                __atomic_store_n(&gDrainDmaWait, true, __ATOMIC_RELEASE);
                IOLockUnlock(gLock);
                idle = switch_wlan_a3_transport_idle();
                IOLockLock(gLock);
                if (!gAttached || !gPowered || gRecoveryRequired || session != gSession || lifetime != gLifetime)
                    continue;
            }
            if (!idle && gDrainDeadlineNs != kDrainExpired) {
                if (!gDrainDeadlineNs) {
                    gDrainDeadlineNs = hostNs() + 1500000000ULL;
                    armTxExpiry();
                }
                if (!__atomic_load_n(&gDrainDmaWait, __ATOMIC_ACQUIRE))
                    continue;
                gBusy = false;
                IOLockUnlock(gLock);
                return;
            }
            __atomic_store_n(&gDrainDmaWait, false, __ATOMIC_RELEASE);
            if (idle <= 0) {
                const int error = idle < 0 ? idle : -ETIMEDOUT;
#if DEVELOPMENT || DEBUG
                PendingTx debt = {};
                for (const auto &pending : gPending)
                    if (pending.id && pending.transport) {
                        debt = pending;
                        break;
                    }
                IOLog("SwitchWLAN: A3 retirement unresolved: %s session %u owner %u request %u epoch %u schedule %u "
                      "error %d\n",
                      gStopped ? "DMA" : "firmware", session, gFirmwareOwnerGeneration, debt.id,
                      debt.id ? debt.epoch : gKnownEpoch, debt.id ? debt.schedule : gConfiguration, error);
#endif
                gDrainPending = false;
                gRecoveryRequired = true;
                gVerifyState = false;
                gState.error = error;
                gState.clock = {};
                armTxExpiry();
                IOLockWakeup(gLock, &gStationPaused, false);
                IOLockUnlock(gLock);
                emitRadioEvent(session, SWLAN_AWDL_RECOVERY_REQUIRED, 0, static_cast<uint16_t>(-error));
                IOLockLock(gLock);
                continue;
            }
            // A STOPPED snapshot and an empty Msgbuf retire the canceled requests even if status delivery failed.
            for (auto &pending : gPending)
                if (pending.transport && pending.notified)
                    pending = {};
            gFirmwareOwnerGeneration = 0;
            gDrainDeadlineNs = 0;
            armTxExpiry();
        }
        if (gDrainPending && !gCount) {
            if (gHaveConfiguration && !gClosing && !gStationPaused &&
                !switch_wlan_a3_clock_valid(gState.clock, hostNs())) {
                enqueueLocked(SWITCH_A3_GET_CLOCK, nullptr, 0, false);
                continue;
            }
            replayDesired();
            continue;
        }
        if (!gCount && gScheduleUpdatePending && !queueScheduleUpdate()) {
            gBusy = false;
            IOLockUnlock(gLock);
            return;
        }
        if (!gCount) {
            if (!gTxCount)
                continue;
            const TxJob tx = gTx[gTxHead];
            bool pending = false;
            for (auto &entry : gPending)
                if (entry.id == tx.id && !entry.notified) {
                    if (entry.submit_deadline_ns <= hostNs()) {
                        thread_call_enter(gTxExpiry);
                        continue;
                    }
                    entry.transport = true;
                    entry.expires_ns = hostNs() + 1100000000ULL;
                    pending = true;
                }
            if (!pending) {
                gTxHead = (gTxHead + 1) % kJobs;
                --gTxCount;
                continue;
            }
            armTxExpiry();
            __atomic_store_n(&gDmaBlocked, true, __ATOMIC_RELEASE);
            IOLockUnlock(gLock);
            const int error = switch_wlan_a3_dma_tx(tx.frame, tx.bytes);
            IOLockLock(gLock);
            for (auto &entry : gPending)
                if (entry.id == tx.id && error) {
                    entry.transport = false;
                    entry.expires_ns = entry.submit_deadline_ns;
                    if ((gScheduleUpdatePending || gScheduleRejected) && !entry.notified && gCancelCount < kJobs) {
                        gCancelled[gCancelCount++] = {SWLAN_AWDL_TXDONE, entry.id, ECANCELED, 0, gSession, entry.epoch};
                        entry.notified = true;
                    }
                    if (entry.notified) {
                        entry = {};
                        ++gOwnershipEvent;
                    }
                }
            armTxExpiry();
            if (!gAttached || !gPowered || tx.session != gSession || tx.lifetime != gLifetime || !gTxCount ||
                gTx[gTxHead].id != tx.id)
                continue;
            if (error == -ENOBUFS || error == -EAGAIN) {
                if (!__atomic_load_n(&gDmaBlocked, __ATOMIC_ACQUIRE))
                    continue;
                gBusy = false;
                IOLockUnlock(gLock);
                return;
            }
            __atomic_store_n(&gDmaBlocked, false, __ATOMIC_RELEASE);
            gTxHead = (gTxHead + 1) % kJobs;
            --gTxCount;
            if (error) {
                for (auto &pending : gPending) {
                    if (pending.id == tx.id) {
                        pending = {};
                        gState.tx_credits = creditRoom();
                        armTxExpiry();
                        break;
                    }
                }
                IOLockUnlock(gLock);
                emitRadioEvent(tx.session, SWLAN_AWDL_TXDONE, tx.id, static_cast<uint16_t>(-error));
                emitRadioEvent(tx.session, SWLAN_AWDL_TX_CREDIT);
                IOLockLock(gLock);
            }
            continue;
        }
        const Job job = gJobs[gHead];
        gHead = (gHead + 1) % kJobs;
        --gCount;
        const bool address = job.control.operation == SWITCH_A3_CONFIGURE;
        const bool updating = job.control.operation == SWITCH_A3_INSTALL_SCHEDULE && gScheduleUpdatePending &&
                              job.control.config_generation == gScheduleRequest;
        const bool mutates = address || job.control.operation == SWITCH_A3_INSTALL_SCHEDULE ||
                             job.control.operation == SWITCH_A3_BIND_RINGS || job.control.operation == SWITCH_A3_START;
        const bool sequenced = job.control.operation == SWITCH_A3_INSTALL_SCHEDULE ||
                               job.control.operation == SWITCH_A3_BIND_RINGS ||
                               job.control.operation == SWITCH_A3_START || job.control.operation == SWITCH_A3_STOP;
        if (!gAttached || !gPowered || gRecoveryRequired || (gStationPaused && mutates && !updating) ||
            job.control.session != gSession || job.lifetime != gLifetime ||
            (address && job.control.config_generation != gAddressGeneration) ||
            (sequenced && !updating && job.control.config_generation != gConfiguration))
            continue;
        if (address) {
            // An ioctl failure or superseded reply cannot prove CONFIGURE never acquired native resources.
            if (!gFirmwareOwnerGeneration)
                gFirmwareOwnerGeneration = job.control.config_generation;
            gStopped = false;
        }
        const bool configured = gConfigured;
        const uint32_t schedule = gSchedule;
        const uint32_t bound = gBound;
        const uint32_t observedEvent = gOwnershipEvent;
        IOLockUnlock(gLock);
        if (job.control.operation == SWITCH_A3_GET_CLOCK) {
            calibrate(job);
        } else {
            switch_a3_reply reply = {};
            uint64_t before = 0, after = 0;
            int error = 0;
            if ((job.control.operation == SWITCH_A3_INSTALL_SCHEDULE && !configured) ||
                ((job.control.operation == SWITCH_A3_BIND_RINGS || job.control.operation == SWITCH_A3_START) &&
                 (!configured || schedule != job.control.config_generation)) ||
                (job.control.operation == SWITCH_A3_START && bound != job.control.config_generation))
                error = -EAGAIN;
            else
                error = switch_wlan_a3_exchange(job, reply, before, after);
            if (!error && job.control.operation != SWITCH_A3_STOP && job.control.operation != SWITCH_A3_GET_STATE &&
                reply.applied_generation != job.control.config_generation)
                error = -EPROTO;
            IOLockLock(gLock);
            const bool current = gAttached && gPowered && !gRecoveryRequired && job.control.session == gSession &&
                                 job.lifetime == gLifetime &&
                                 (!address || job.control.config_generation == gAddressGeneration) &&
                                 (!sequenced || updating || job.control.config_generation == gConfiguration);
            bool needsRecovery = false;
            bool changedEpoch = false;
            int snapshotFault = 0;
            if (current) {
                const bool observed = reply.state_revision && observeReply(reply);
                if (updating) {
                    if (observed)
                        observeSchedule(reply);
                    finishScheduleUpdate(job, reply, error, observedEvent);
                } else if (job.control.operation == SWITCH_A3_GET_STATE) {
                    if (!error && observed) {
#if DEVELOPMENT || DEBUG
                        if ((!gHaveConfiguration || gClosing) && !gFirmwareOwnerGeneration &&
                            reply.applied_generation > gConfiguration) {
                            gConfiguration = reply.applied_generation;
                            gFirmwareOwnerGeneration = reply.applied_generation;
                        }
#endif
                        changedEpoch = applySnapshot(reply);
                        snapshotFault = gRetireError;
                    } else if (!error) {
                        gVerifyState = true;
                    } else if (!gDrainPending && !gState.ready && gFirmwareOwnerGeneration && !gRetireError) {
                        gRetireError = error;
                        deferDesired(false, gOwnershipEvent);
                        gState.error = error;
                    }
                } else if (error) {
                    gState.ready = 0;
                    gBound = 0;
                    if (error == -EBUSY) {
                        deferDesired(true, observedEvent);
                        error = 0;
                    } else if (job.control.operation == SWITCH_A3_STOP) {
                        gRecoveryRequired = true;
                        gCount = gHead = 0;
                        gState.tx_credits = 0;
                        needsRecovery = true;
                        gDrainPending = false;
                        IOLockWakeup(gLock, &gStationPaused, false);
                    }
                    if (needsRecovery) {
                        gState.error = error;
                    } else if (error == -ESTALE) {
                        gState.clock = {};
                        gState.error = 0;
                        gApplied = gSchedule = 0;
                        gConfigured = false;
                        cancelTx();
                    } else {
                        if (!gState.error)
                            gState.error = error;
                    }
                } else {
                    gApplied = reply.applied_generation;
                    if (gApplied) {
                        gFirmwareOwnerGeneration = gApplied;
                        gStopped = false;
                    }
                    if (job.control.operation == SWITCH_A3_CONFIGURE &&
                        job.control.config_generation == gAddressGeneration)
                        gConfigured = true;
                    if (job.control.operation == SWITCH_A3_INSTALL_SCHEDULE) {
                        gSchedule = job.control.config_generation;
                        memcpy(&gAppliedSchedule, job.payload, sizeof gAppliedSchedule);
                    }
                    if (job.control.operation == SWITCH_A3_BIND_RINGS)
                        gBound = job.control.config_generation;
                    if (job.control.operation == SWITCH_A3_STOP) {
                        gApplied = gSchedule = gBound = 0;
                        gStopped = stoppedForOwner(reply, true);
                        if (!gStopped) {
                            gState.error = -EPROTO;
                            gRecoveryRequired = true;
                            gDrainPending = false;
                            needsRecovery = true;
                        }
                    }
                    if (job.control.config_generation == gConfiguration && !needsRecovery) {
                        gState.error = gRetireError;
                        gState.ready = observed && !reply.fault_status && reply.legal_social_channels &&
                                       job.control.operation == SWITCH_A3_START &&
                                       gBound == gConfiguration && gSchedule == gConfiguration &&
                                       reply.state == SWITCH_A3_RUNNING && reply.applied_generation == gConfiguration &&
                                       (reply.features & SWITCH_A3_FEATURE_REQUIRED) == SWITCH_A3_FEATURE_REQUIRED &&
                                       gState.clock.epoch == reply.clock_epoch;
                    }
                    updateCredits(reply.tx_credits);
                }
            }
            IOLockUnlock(gLock);
            if (needsRecovery)
                emitRadioEvent(job.control.session, SWLAN_AWDL_RECOVERY_REQUIRED, job.control.request_id,
                               static_cast<uint16_t>(error ? -error : EPROTO), reply.tsf);
            else if (current)
                emitRadioEvent(job.control.session,
                               changedEpoch ? SWLAN_AWDL_CLOCK_CHANGED : SWLAN_AWDL_CONFIG_CHANGED,
                               job.control.request_id, static_cast<uint16_t>(-(error ? error : snapshotFault)), reply.tsf);
        }
        IOLockLock(gLock);
    }
    gBusy = false;
    IOLockWakeup(gLock, &gStationPaused, false);
    IOLockUnlock(gLock);
}

int enqueueLocked(uint32_t operation, const void *payload, size_t length, bool changes)
{
    if (changes) {
        if (++gConfiguration == 0)
            ++gConfiguration;
        gState.ready = 0;
        gBound = 0;
        cancelTx();
        if (operation == SWITCH_A3_CONFIGURE) {
            gAddressGeneration = gConfiguration;
            gConfigured = false;
            gState.error = 0;
        } else if (operation == SWITCH_A3_STOP) {
            gConfigured = false;
            gState.error = 0;
        }
    }
    gJobs[(gHead + gCount++) % kJobs] = makeJob(operation, payload, length);
    if (operation == SWITCH_A3_GET_CLOCK)
        gClockQueued = true;
    return 0;
}

int queueControl(uint32_t operation, const void *payload, size_t length, bool changes)
{
    if (!gLock || length > sizeof(Job::payload))
        return -ENXIO;
    IOLockLock(gLock);
    int error = 0;
    if (!gAttached || !gPowered || gRecoveryRequired)
        error = -ENXIO;
    else if (operation == SWITCH_A3_STOP) {
        gCount = gHead = 0;
        gClockQueued = false;
        ++gLifetime;
        gAddressGeneration = 0;
        enqueueLocked(operation, payload, length, changes);
    } else if (operation == SWITCH_A3_GET_CLOCK && gClockQueued) {
    } else if (operation != SWITCH_A3_GET_CLOCK && operation != SWITCH_A3_GET_STATE && !gState.clock.epoch)
        error = -EAGAIN;
    else if (gCount == kJobs)
        error = -ENOBUFS;
    else
        enqueueLocked(operation, payload, length, changes);
    IOLockUnlock(gLock);
    if (!error)
        thread_call_enter(gWorker);
    return error;
}
}  // namespace switch_a3_host

using namespace switch_a3_host;

int switch_wlan_a3_radio_clock(void)
{
    return queueControl(SWITCH_A3_GET_CLOCK, nullptr, 0, false);
}

int switch_wlan_a3_radio_refresh(void)
{
    return queueControl(SWITCH_A3_GET_STATE, nullptr, 0, false);
}

int switch_wlan_a3_radio_rx(swlan_rx_fn fn, void *context)
{
    if (!gLock)
        return -ENXIO;
    IOLockLock(gLock);
    gRx = fn;
    gRxContext = context;
    IOLockUnlock(gLock);
    return 0;
}

int switch_wlan_a3_radio_filter(const uint8_t address[6], const uint8_t bssid[6], uint16_t ethertype)
{
    if (!address || !bssid || (address[0] & 1))
        return -EINVAL;
    switch_a3_configuration config = {};
    memcpy(config.address, address, 6);
    memcpy(config.bssid, bssid, 6);
    config.ethertype = ethertype;
    const auto infra = awdl_chan_of_freq(swlan_awdl_infra_freq());
    config.flags = SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK;
    if (!gLock)
        return -ENXIO;
    IOLockLock(gLock);
    if (!gAttached || !gPowered || gRecoveryRequired) {
        IOLockUnlock(gLock);
        return -ENXIO;
    }
    config.chanspec = bcm_chanspec20(infra.number, gD11n);
    if (!config.chanspec)
        config.chanspec = bcm_chanspec20(6, gD11n);
    if (!gHaveConfiguration || memcmp(&config, &gDesiredConfiguration, sizeof config) || gClosing || gState.error) {
        gDesiredConfiguration = config;
        gHaveConfiguration = true;
        gClosing = false;
        gRetireError = 0;
        deferDesired(false, gOwnershipEvent);
    }
    IOLockUnlock(gLock);
    thread_call_enter(gWorker);
    return 0;
}

static int encodeSchedule(const swlan_chan_seq *schedule, int d11n, switch_a3_schedule &wire)
{
    if (!schedule->slots || !schedule->n_slots || schedule->n_slots > SWITCH_A3_MAX_SLOTS || !schedule->period_us)
        return -EINVAL;
    wire.anchor_tsf = schedule->anchor_tsf;
    wire.period_us = schedule->period_us;
    wire.infra_chanspec = bcm_chanspec20(awdl_chan_of_freq(schedule->infra_freq_mhz).number, d11n);
    wire.slot_count = schedule->n_slots;
    uint64_t period = 0;
    for (unsigned i = 0; i < schedule->n_slots; ++i) {
        wire.slots[i].chanspec = schedule->slots[i].freq_mhz
                                     ? bcm_chanspec20(awdl_chan_of_freq(schedule->slots[i].freq_mhz).number, d11n)
                                     : 0;
        wire.slots[i].dwell_us = schedule->slots[i].dwell_us;
        if ((schedule->slots[i].freq_mhz && !wire.slots[i].chanspec) || !wire.slots[i].dwell_us)
            return -EINVAL;
        period += wire.slots[i].dwell_us;
    }
    return period == wire.period_us ? 0 : -EINVAL;
}

int switch_wlan_a3_radio_schedule(const swlan_chan_seq *schedule)
{
    if (!schedule)
        return switch_wlan_a3_radio_close();
    if (!gLock)
        return -ENXIO;
    IOLockLock(gLock);
    switch_a3_schedule wire = {};
    int error = encodeSchedule(schedule, gD11n, wire);
    if (error) {
        gScheduleRejected = true;
        cancelTx(true);
    } else if (!gAttached || !gPowered || gRecoveryRequired)
        error = -ENXIO;
    else if (!gRings.ring_count)
        error = -EOPNOTSUPP;
    else if (gScheduleRejected || !gHaveSchedule || memcmp(&wire, &gDesiredSchedule, sizeof wire) ||
             gClosing || gState.error) {
        const bool replace = gState.ready && gConfigured && gApplied && !gDrainPending && !gClosing;
        gScheduleRejected = false;
        gDesiredSchedule = wire;
        gHaveSchedule = true;
        gClosing = false;
        gRetireError = 0;
        if (replace) {
            if (!gStationPaused || !gPauseReady)
                requestScheduleUpdate();
        } else {
            deferDesired(false, gOwnershipEvent);
        }
    }
    IOLockUnlock(gLock);
    if (!error || error == -EINVAL)
        thread_call_enter(gWorker);
    return error;
}

int switch_wlan_a3_radio_close(void)
{
    if (!gLock)
        return -ENXIO;
    IOLockLock(gLock);
    gClosing = true;
    gHaveSchedule = false;
    gRetireError = 0;
    const bool current = gAttached && gPowered && !gRecoveryRequired;
    if (current)
        deferDesired(false, gOwnershipEvent);
    IOLockUnlock(gLock);
    if (current)
        thread_call_enter(gWorker);
    return current ? 0 : -ENXIO;
}
