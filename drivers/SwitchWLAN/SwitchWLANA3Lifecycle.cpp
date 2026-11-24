// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <kern/thread.h>
#include "SwitchWLANA3State.h"

using namespace switch_a3_host;

void switch_wlan_a3_radio_attach(const switch_a3_caps *caps, const uint8_t mac[6], int d11n, uint16_t firstRing,
                                 unsigned ringCount)
{
    if (!caps || !mac || caps->magic != SWITCH_A3_CAPS_MAGIC || caps->version != SWITCH_A3_CAPS_VERSION ||
        caps->size != sizeof(*caps) || caps->status != SWITCH_A3_ATTACHED || caps->register_status || firstRing < 2 ||
        ringCount > UINT16_MAX || static_cast<uint32_t>(firstRing) + ringCount > 65536u)
        return;
    if (!gLock) {
        IOLock *lock = IOLockAlloc();
        thread_call_t worker = thread_call_allocate(work, nullptr);
        thread_call_t expiry = thread_call_allocate(expireTx, nullptr);
        if (!lock || !worker || !expiry) {
            if (lock)
                IOLockFree(lock);
            if (worker)
                thread_call_free(worker);
            if (expiry)
                thread_call_free(expiry);
            return;
        }
        gWorker = worker;
        gTxExpiry = expiry;
        gLock = lock;
    }
    IOLockLock(gLock);
#if DEVELOPMENT || DEBUG
    gDiagnostics = {};
#endif
    gStationPaused = gPauseReady = false;
    gScheduleRejected = false;
    resetScheduleUpdate();
    gAttached = true;
    gRecoveryRequired = false;
    gPowered = true;
    ++gLifetime;
    if (++gSession == 0)
        ++gSession;
    gCount = gHead = 0;
    gTxCount = gTxHead = 0;
    gDataRateIndex = kDataRateMax;
    gDataAckStreak = 0;
    memset(gPending, 0, sizeof gPending);
    armTxExpiry();
    gClockQueued = false;
    gVerifyState = false;
    gConfiguration = gApplied = gSchedule = gAddressGeneration = gFirmwareGeneration = 0;
    gCancelCount = 0;
    gConfigured = false;
    gFirmwareOwnerGeneration = 0;
    gStateRevision = gObservedEpoch = 0;
    gBound = 0;
    gCreditLimit = kRadioCredits;
    gRings = {firstRing, static_cast<uint16_t>(ringCount), SWITCH_A3_MAX_FRAME, 0, kRadioCredits, 0};
    const bool replay = gHaveConfiguration && !gClosing;
    gDrainPending = replay;
    gDrainDeadlineNs = 0;
    armTxExpiry();
    gStopped = false;
    gDrainWaitEvent = false;
    gOwnershipEvent = gDrainEvent = 0;
    gKnownEpoch = 0;
    gClockError = 0;
    gRetireError = 0;
    __atomic_store_n(&gDrainDmaWait, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gDmaBlocked, false, __ATOMIC_RELEASE);
    gState = {};
    gState.version = SWLAN_AWDL_STATE_VERSION;
    gState.generation = gSession;
    gFeatures = caps->features;
    gD11n = d11n;
    memcpy(gMac, mac, sizeof gMac);
    const uint32_t session = gSession;
    IOLockUnlock(gLock);
    if (replay)
        thread_call_enter(gWorker);
    emitRadioEvent(session, SWLAN_AWDL_CONFIG_CHANGED);
}

void switch_wlan_a3_radio_lost(void)
{
    if (!gLock)
        return;
    IOLockLock(gLock);
    resetScheduleUpdate();
    gAttached = false;
    gPowered = false;
    ++gLifetime;
    gCancelCount = 0;
    gConfigured = false;
    gDrainPending = gDrainWaitEvent = false;
    __atomic_store_n(&gDrainDmaWait, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gDmaBlocked, false, __ATOMIC_RELEASE);
    gCount = gHead = 0;
    gTxCount = gTxHead = 0;
    gDataRateIndex = kDataRateMax;
    gDataAckStreak = 0;
    memset(gPending, 0, sizeof gPending);
    armTxExpiry();
    gClockQueued = false;
    gVerifyState = false;
    gApplied = gSchedule = gBound = 0;
    gState.ready = 0;
    gState.tx_credits = 0;
    gState.clock = {};
    gState.error = -ENXIO;
    gState.social_channels_valid = 0;
    IOLockWakeup(gLock, &gStationPaused, false);
    IOLockUnlock(gLock);
}

void switch_wlan_a3_radio_power(bool on)
{
    if (!gLock)
        return;
    IOLockLock(gLock);
    if (!gAttached || gRecoveryRequired || gPowered == on) {
        IOLockUnlock(gLock);
        return;
    }
    resetScheduleUpdate();
    gPowered = on;
    ++gLifetime;
    gCount = gHead = 0;
    gClockQueued = false;
    gVerifyState = false;
    gState.ready = 0;
    gState.error = 0;
    gRetireError = 0;
    cancelTx();
    gState.clock = {};
    gState.social_channels_valid = 0;
    gApplied = gSchedule = 0;
    gConfigured = false;
    if (on) {
        gDataRateIndex = kDataRateMax;
        gDataAckStreak = 0;
    }
    const bool replay = on && (gFirmwareOwnerGeneration || (gHaveConfiguration && !gClosing && !gStationPaused));
    gDrainPending = replay;
    gDrainDeadlineNs = 0;
    armTxExpiry();
    gStopped = false;
    gDrainWaitEvent = false;
    if (on && gFirmwareOwnerGeneration)
        enqueueLocked(SWITCH_A3_STOP, nullptr, 0, false);
    const uint32_t session = gSession;
    IOLockWakeup(gLock, &gStationPaused, false);
    IOLockUnlock(gLock);
    if (replay)
        thread_call_enter(gWorker);
    if (on) {
        emitRadioEvent(session, SWLAN_AWDL_RADIO_UP);
    } else {
        emitRadioEvent(session, SWLAN_AWDL_RADIO_DOWN);
    }
}

int switch_wlan_a3_radio_station_prepare(void)
{
    if (!gLock)
        return 0;
    IOLockLock(gLock);
    if (!gAttached || !gPowered) {
        IOLockUnlock(gLock);
        return 0;
    }
    if (gRecoveryRequired) {
        const int error = gState.error ? gState.error : -ENXIO;
        IOLockUnlock(gLock);
        return error;
    }
    const uint32_t session = gSession;
    if (!gStationPaused) {
        gStationPaused = true;
        gPauseReady = !gFirmwareOwnerGeneration;
        if (!gPauseReady) {
            if (gState.ready && gHaveSchedule && !gDrainPending)
                requestScheduleUpdate();
            else
                deferDesired(false, gOwnershipEvent);
            thread_call_enter(gWorker);
        }
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(10000, kMillisecondScale, &deadline);
    bool timedOut = false;
    while (!gPauseReady && gAttached && gPowered && !gRecoveryRequired && gSession == session) {
        if (IOLockSleepDeadline(gLock, &gStationPaused, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            timedOut = true;
            break;
        }
    }
    if (timedOut && !gPauseReady && gAttached && gPowered && !gRecoveryRequired && gSession == session) {
        gStationPaused = gPauseReady = false;
        gDrainPending = gDrainWaitEvent = false;
        resetScheduleUpdate();
        gRecoveryRequired = true;
        gCount = gHead = 0;
        gState.ready = 0;
        gState.tx_credits = 0;
        gState.clock = {};
        gState.error = -ETIMEDOUT;
        armTxExpiry();
        IOLockWakeup(gLock, &gStationPaused, false);
        IOLockUnlock(gLock);
        emitRadioEvent(session, SWLAN_AWDL_RECOVERY_REQUIRED, 0, ETIMEDOUT);
        return -ETIMEDOUT;
    }
    const int error = !gAttached || !gPowered || gSession != session ? -ENXIO : gRecoveryRequired ? gState.error : 0;
    IOLockUnlock(gLock);
    return error;
}

void switch_wlan_a3_radio_station_resume(void)
{
    if (!gLock)
        return;
    const auto home = awdl_chan_of_freq(swlan_awdl_infra_freq());
    IOLockLock(gLock);
    const bool paused = gStationPaused;
    gStationPaused = gPauseReady = false;
    const bool current = paused && gAttached && gPowered && !gRecoveryRequired;
    const bool refresh = current && gState.social_channels_valid && !gState.social_channels;
    const bool replay = current && gHaveConfiguration && !gClosing;
    if (refresh)
        gVerifyState = true;
    if (replay) {
        const uint16_t channel = bcm_chanspec20(home.number, gD11n);
        if (channel)
            gDesiredConfiguration.chanspec = channel;
        gDesiredSchedule.infra_chanspec = channel;
        if (gState.ready && gHaveSchedule && !gDrainPending)
            requestScheduleUpdate();
        else
            deferDesired(false, gOwnershipEvent);
    }
    IOLockUnlock(gLock);
    if (replay || refresh)
        thread_call_enter(gWorker);
}

bool switch_wlan_a3_radio_present(void)
{
    if (!gLock)
        return false;
    IOLockLock(gLock);
    const bool present = gAttached;
    IOLockUnlock(gLock);
    return present;
}

bool switch_wlan_a3_radio_caps(swlan_awdl_caps *caps)
{
    if (!caps || !gLock)
        return false;
    IOLockLock(gLock);
    if (!gAttached) {
        IOLockUnlock(gLock);
        return false;
    }
    caps->has_native_awdl = 0;
    caps->soft_trial = 0;
    caps->requires_radio_ready = caps->has_tsf_clock = 1;
    caps->can_monitor = (gFeatures & SWITCH_A3_FEATURE_RX) != 0;
    caps->can_inject = caps->can_tx_action = (gFeatures & SWITCH_A3_FEATURE_TX) != 0;
    caps->can_scheduled_hops = (gFeatures & SWITCH_A3_FEATURE_SCHEDULE) != 0;
    caps->can_stay_assoc = (gFeatures & SWITCH_A3_FEATURE_COEX) != 0;
    caps->max_inject_len = SWITCH_A3_MAX_FRAME;
    memcpy(caps->self_mac, gMac, sizeof gMac);
    IOLockUnlock(gLock);
    return true;
}

int switch_wlan_a3_radio_state(swlan_awdl_state *state)
{
    if (!state || !gLock)
        return -ENXIO;
    IOLockLock(gLock);
    *state = gState;
    state->applied_generation = gApplied;
    if (!switch_wlan_a3_clock_valid(state->clock, hostNs()))
        state->tx_credits = 0;
    const int error = gAttached ? 0 : -ENXIO;
    IOLockUnlock(gLock);
    return error;
}
