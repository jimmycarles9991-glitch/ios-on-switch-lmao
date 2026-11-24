// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchWLANA3State.h"

namespace switch_a3_host {
bool gScheduleUpdatePending, gScheduleWaitEvent, gScheduleRejected;
uint32_t gScheduleRequest, gScheduleEvent;
switch_a3_schedule gAppliedSchedule;

void resetScheduleUpdate()
{
    gScheduleUpdatePending = gScheduleWaitEvent = false;
    gScheduleRequest = gScheduleEvent = 0;
}

void requestScheduleUpdate()
{
    if (++gConfiguration == 0)
        ++gConfiguration;
    gScheduleUpdatePending = true;
    gScheduleWaitEvent = false;
    gState.tx_credits = 0;
    cancelTx(true);
}

bool queueScheduleUpdate()
{
    if (!gScheduleUpdatePending || gScheduleRequest || gCount ||
        (gScheduleWaitEvent && gScheduleEvent == gOwnershipEvent))
        return false;
    if (!gAttached || !gPowered || gRecoveryRequired || gDrainPending || !gState.ready) {
        resetScheduleUpdate();
        return false;
    }
    switch_a3_schedule schedule = gDesiredSchedule;
    if (gStationPaused) {
        for (auto &slot : schedule.slots)
            slot.chanspec = 0;
    }
    gScheduleRequest = gConfiguration;
    gScheduleWaitEvent = false;
    enqueueLocked(SWITCH_A3_INSTALL_SCHEDULE, &schedule, sizeof schedule, false);
    return true;
}

bool scheduleGenerationCurrent(uint32_t generation)
{
    return generation == gApplied || (gScheduleUpdatePending && generation == gScheduleRequest);
}

bool observeSchedule(const switch_a3_reply &reply)
{
    if (!gScheduleUpdatePending || !gScheduleRequest || reply.applied_generation != gScheduleRequest ||
        reply.state_revision != gStateRevision ||
        reply.state != SWITCH_A3_RUNNING || reply.fault_status || reply.clock_epoch != gKnownEpoch ||
        (reply.features & SWITCH_A3_FEATURE_REQUIRED) != SWITCH_A3_FEATURE_REQUIRED)
        return false;
    gApplied = gSchedule = gBound = reply.applied_generation;
    gFirmwareOwnerGeneration = gApplied;
    return true;
}

void finishScheduleUpdate(const Job &job, const switch_a3_reply &reply, int error, uint32_t observedEvent)
{
    if (!gScheduleUpdatePending || gScheduleRequest != job.control.config_generation)
        return;
    if (!gState.ready || reply.clock_epoch != gKnownEpoch || gObservedEpoch != gKnownEpoch)
        error = -ESTALE;
    if (!error && reply.applied_generation == job.control.config_generation)
        observeSchedule(reply);
    if (!error && gApplied != job.control.config_generation)
        error = -EPROTO;
    gScheduleRequest = 0;
    if (error == -EBUSY && !reply.fault_status && reply.state == SWITCH_A3_RUNNING &&
        reply.applied_generation == gApplied && reply.clock_epoch == gKnownEpoch) {
        gScheduleWaitEvent = gOwnershipEvent == observedEvent;
        gScheduleEvent = observedEvent;
        return;
    }
    if ((error == -EINVAL || error == -ERANGE || error == -EOPNOTSUPP) && !gStationPaused &&
        reply.state == SWITCH_A3_RUNNING && !reply.fault_status && reply.applied_generation == gApplied &&
        reply.clock_epoch == gKnownEpoch) {
        if (job.control.config_generation == gConfiguration) {
            gDesiredSchedule = gAppliedSchedule;
            gScheduleUpdatePending = false;
            gScheduleRejected = true;
        }
        gScheduleWaitEvent = false;
        updateCredits(reply.tx_credits);
        return;
    }
    if (error) {
        gRetireError = error == -ESTALE || error == -EAGAIN ? 0 : error;
        deferDesired(false, gOwnershipEvent);
        gState.error = gRetireError;
        return;
    }
    memcpy(&gAppliedSchedule, job.payload, sizeof gAppliedSchedule);
    gScheduleUpdatePending = job.control.config_generation != gConfiguration;
    gScheduleWaitEvent = false;
    gState.error = 0;
    if (!gScheduleUpdatePending && gStationPaused) {
        gPauseReady = true;
        IOLockWakeup(gLock, &gStationPaused, false);
    }
    updateCredits(reply.tx_credits);
}
}
