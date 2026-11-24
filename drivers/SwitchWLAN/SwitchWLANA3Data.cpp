// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchWLANA3State.h"
#if DEVELOPMENT || DEBUG
#include "SwitchWLANTrace.h"
#endif

namespace switch_a3_host {
void armTxExpiry()
{
    uint64_t first = gDrainPending && gDrainDeadlineNs ? gDrainDeadlineNs : UINT64_MAX;
    if (gDrainDeadlineNs != kDrainExpired)
        for (const auto &pending : gPending)
            if (pending.id && pending.expires_ns < first)
                first = pending.expires_ns;
    if (first == UINT64_MAX) {
        thread_call_cancel(gTxExpiry);
    } else {
        uint64_t deadline = 0;
        nanoseconds_to_absolutetime(first, &deadline);
        thread_call_enter_delayed(gTxExpiry, deadline);
    }
}

void expireTx(thread_call_param_t, thread_call_param_t)
{
    swlan_awdl_event expired[kJobs];
    unsigned count = 0;
    IOLockLock(gLock);
    const uint64_t now = hostNs();
    const bool active = gAttached && gPowered && !gRecoveryRequired && gFirmwareOwnerGeneration;
    bool drain = gDrainPending && gDrainDeadlineNs && gDrainDeadlineNs <= now;
    for (const auto &pending : gPending)
        drain |= pending.id && pending.transport && pending.expires_ns <= now;
    for (auto &pending : gPending) {
        if (!pending.id || pending.expires_ns > now)
            continue;
        if (!pending.notified) {
            // The chip took it and may have sent it: ETIME, unlike ETIMEDOUT, tells the sender not to send it again.
            const uint16_t status = pending.transport ? ETIME : ETIMEDOUT;
            expired[count++] = {SWLAN_AWDL_TXDONE, pending.id, status, 0, gSession, pending.epoch};
        }
        pending.notified = true;
        if (pending.transport && active)
            pending.expires_ns = UINT64_MAX;
        else
            pending = {};
    }
    const bool reconcile = drain && active && gDrainDeadlineNs != kDrainExpired;
    if (reconcile) {
        if (gDrainPending)
            gDrainDeadlineNs = kDrainExpired;
        else
            deferDesired(false, gOwnershipEvent);
    } else if (gState.ready) {
        gState.tx_credits = creditRoom();
    }
    armTxExpiry();
    IOLockUnlock(gLock);
    for (unsigned i = 0; i < count; ++i)
        switch_wlan_a3_notify(&expired[i]);
    if (reconcile || count)
        thread_call_enter(gWorker);
}

void cancelTx(bool retainSubmitted)
{
    for (auto &pending : gPending) {
        if (!pending.id || (retainSubmitted && pending.transport))
            continue;
        if (!pending.notified && gCancelCount < kJobs) {
            const uint16_t status = pending.transport ? ETIME : ECANCELED;
            gCancelled[gCancelCount++] = {SWLAN_AWDL_TXDONE, pending.id, status, 0, gSession, pending.epoch};
        }
        pending.notified = true;
        if (!pending.transport)
            pending = {};
    }
    gTxCount = gTxHead = 0;
    gState.tx_credits = 0;
    armTxExpiry();
    __atomic_store_n(&gDmaBlocked, false, __ATOMIC_RELEASE);
}
}

using namespace switch_a3_host;

#if DEVELOPMENT || DEBUG
static void traceEchoTx(PendingTx &pending, const swlan_tx_req &request)
{
    static uint32_t session;
    static unsigned count;
    if ((request.frame[0] & 0x0c) != 8 || (request.frame[4] & 1))
        return;
    if (session != gSession) {
        session = gSession;
        count = 0;
    }
    if (count >= 16)
        return;
    awdl_msdu msdu = {};
    uint16_t sequence = 0;
    int retry = 0;
    if (awdl_data_msdus(request.frame, request.len, &msdu, 1, &sequence, &retry) != 1 || msdu.ethertype != 0x86dd)
        return;
    const auto echo = switch_wlan_echo6_ip(msdu.payload, msdu.len, msdu.len);
    if (!echo.type)
        return;
    ++count;
    pending.echoId = echo.id;
    pending.echoSequence = echo.sequence;
    pending.echoType = echo.type;
    IOLog("SwitchWLAN: A3 echo queued type %u id %u seq %u request %u session %u epoch %u schedule %u\n",
          echo.type, echo.id, echo.sequence, pending.id, session, pending.epoch, pending.schedule);
}
#endif

int switch_wlan_a3_radio_tx(const swlan_tx_req *request)
{
    if (!request || !request->frame || request->len < 24 || request->len > SWITCH_A3_MAX_FRAME)
        return -EMSGSIZE;
    if (!gLock)
        return -ENXIO;
    if (request->rate_mcs != 0xff || ((request->flags & SWLAN_TX_NO_ACK) && !(request->frame[4] & 1)))
        return -EOPNOTSUPP;
    if (request->flags & ~(SWLAN_TX_NO_ACK | SWLAN_TX_ON_CHANNEL_ONLY))
        return -EINVAL;
    uint8_t envelope[14 + sizeof(switch_a3_tx_header) + SWITCH_A3_MAX_FRAME] = {};
    switch_a3_tx_header header = {};
    IOLockLock(gLock);
    const uint64_t now = hostNs();
    const uint16_t chanspec = bcm_chanspec20(awdl_chan_of_freq(request->freq_mhz).number, gD11n);
    if (!chanspec) {
        IOLockUnlock(gLock);
        return -EINVAL;
    }
    if (!gAttached || !gPowered || !gState.ready || gScheduleUpdatePending || gScheduleRejected || gStationPaused ||
        !switch_wlan_a3_clock_valid(gState.clock, now) ||
        request->generation != gSession || request->clock_epoch != gState.clock.epoch) {
        IOLockUnlock(gLock);
        return -EAGAIN;
    }
    unsigned pendingIndex = 0;
    while (pendingIndex < kJobs && gPending[pendingIndex].id)
        ++pendingIndex;
    if (!gState.tx_credits || gTxCount == kJobs || pendingIndex == kJobs) {
        IOLockUnlock(gLock);
        return -ENOBUFS;
    }
    header.magic = SWITCH_A3_TX_MAGIC;
    header.version = SWITCH_A3_WIRE_VERSION;
    header.header_bytes = sizeof header;
    header.total_bytes = static_cast<uint32_t>(sizeof header + request->len);
    header.session = gSession;
    header.clock_epoch = gState.clock.epoch;
    header.schedule_generation = gSchedule;
    header.request_id = (++gRequest & INT32_MAX);
    if (!header.request_id)
        header.request_id = (++gRequest & INT32_MAX);
    header.firmware_generation = gFirmwareGeneration;
    const bool unicastData = (request->frame[0] & 0x0c) == 8 && !(request->frame[4] & 1);
    const uint8_t rateIndex = unicastData ? gDataRateIndex : 0;
    header.frame_bytes = static_cast<uint16_t>(request->len);
    header.chanspec = chanspec;
    header.flags = request->flags | SWITCH_A3_TX_ON_CHANNEL_ONLY;
    header.rate = unicastData ? kDataRates[rateIndex] : kLegacy6Mbps;
    const uint64_t nowTsf = gState.clock.tsf_us + (now - gState.clock.host_ns) / 1000;
    header.expiry_tsf = nowTsf + 1000000;
    if (request->deadline_tsf && request->deadline_tsf < header.expiry_tsf)
        header.expiry_tsf = request->deadline_tsf;
    if (header.expiry_tsf <= nowTsf) {
        IOLockUnlock(gLock);
        return -ETIMEDOUT;
    }
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    memcpy(envelope, marker, sizeof marker);
    envelope[6] = 0x02;
    envelope[7] = 0x57;
    memcpy(envelope + 8, &header.request_id, sizeof header.request_id);
    envelope[12] = 0x88;
    envelope[13] = 0xb5;
    --gState.tx_credits;
    memcpy(envelope + 14, &header, sizeof header);
    memcpy(envelope + 14 + sizeof header, request->frame, request->len);
    TxJob &job = gTx[(gTxHead + gTxCount++) % kJobs];
    job.session = gSession;
    job.lifetime = gLifetime;
    job.id = header.request_id;
    job.bytes = static_cast<uint16_t>(14 + header.total_bytes);
    memcpy(job.frame, envelope, job.bytes);
    const uint64_t expires = now + (header.expiry_tsf - nowTsf) * 1000;
    gPending[pendingIndex] = {header.request_id,
                              !(request->flags & SWLAN_TX_NO_ACK) && !(request->frame[4] & 1),
                              false,
                              false,
                              unicastData,
                              rateIndex,
                              expires,
                              expires,
                              gState.clock.epoch,
                              gSchedule};
#if DEVELOPMENT || DEBUG
    traceEchoTx(gPending[pendingIndex], *request);
#endif
    armTxExpiry();
    IOLockUnlock(gLock);
    thread_call_enter(gWorker);
    return static_cast<int>(header.request_id);
}

bool switch_wlan_a3_radio_tx_allowed(const uint8_t *envelope, size_t length)
{
    if (!gLock)
        return false;
    IOLockLock(gLock);
    switch_a3_tx_header header = {};
    bool valid = gAttached && gPowered && !gRecoveryRequired && gState.ready && !gScheduleUpdatePending &&
                 !gScheduleRejected && !gStationPaused &&
                 switch_wlan_a3_clock_valid(gState.clock, hostNs()) &&
                 switch_a3_tx_decode(envelope, length, gSession, gState.clock.epoch, &header) &&
                 header.schedule_generation == gSchedule;
    if (valid) {
        valid = false;
        for (const auto &pending : gPending)
            valid |= pending.id == header.request_id && pending.transport && !pending.notified &&
                     pending.epoch == header.clock_epoch && pending.schedule == header.schedule_generation;
    }
    IOLockUnlock(gLock);
    return valid;
}

static void switch_wlan_a3_state_event(const uint8_t *frame, size_t length)
{
    switch_a3_reply state = {};
    if (length != sizeof state)
        return;
    memcpy(&state, frame, sizeof state);
    if (!switch_a3_state_valid(state, length) || state.request_id || !state.session || state.status)
        return;
    IOLockLock(gLock);
    if (!gAttached || !gPowered || gRecoveryRequired || state.session != gSession ||
        (gKnownEpoch && static_cast<int32_t>(state.clock_epoch - gKnownEpoch) < 0) ||
        (state.state == SWITCH_A3_RUNNING && state.applied_generation != gConfiguration &&
         !scheduleGenerationCurrent(state.applied_generation)) || !observeReply(state)) {
        IOLockUnlock(gLock);
        return;
    }
    ++gOwnershipEvent;
    observeSchedule(state);
    if (gDrainPending && !state.fault_status && stoppedForOwner(state)) {
        applySnapshot(state);
        gVerifyState = false;
        IOLockUnlock(gLock);
        thread_call_enter(gWorker);
        return;
    }
    const bool running = state.state == SWITCH_A3_RUNNING && !state.fault_status && state.legal_social_channels &&
                         state.clock_epoch == gKnownEpoch &&
                         (state.features & SWITCH_A3_FEATURE_REQUIRED) == SWITCH_A3_FEATURE_REQUIRED;
    const bool verify = !running || !gState.ready;
    const bool scheduleWake = gScheduleUpdatePending;
    if (verify) {
#if DEVELOPMENT || DEBUG
        if (!gClosing && (state.fault_status || state.state == SWITCH_A3_FAULTED ||
                          state.clock_epoch != gKnownEpoch ||
                          (state.features & SWITCH_A3_FEATURE_REQUIRED) != SWITCH_A3_FEATURE_REQUIRED))
            gDiagnostics.fault_pending = true;
#endif
        if (gApplied && (state.state_generation == gApplied ||
                         (gScheduleUpdatePending && state.state_generation == gScheduleRequest)))
            gState.ready = gState.tx_credits = 0;
        gVerifyState = true;
    } else {
        updateCredits(state.tx_credits);
    }
    IOLockUnlock(gLock);
    if (verify || scheduleWake)
        thread_call_enter(gWorker);
    else
        emitRadioEvent(state.session, SWLAN_AWDL_CONFIG_CHANGED, 0, 0, state.tsf);
}

bool switch_wlan_a3_radio_frame(const uint8_t *frame, size_t length)
{
    if (!frame || length < 4 || !gLock)
        return false;
    uint32_t magic = 0;
    memcpy(&magic, frame, sizeof magic);
    if (magic == SWITCH_A3_STATE_MAGIC) {
        switch_wlan_a3_state_event(frame, length);
        return true;
    }
    if (magic != SWITCH_A3_RAW_RX_MAGIC && magic != SWITCH_A3_TX_STATUS_MAGIC)
        return false;
    IOLockLock(gLock);
    switch_a3_raw_rx header = {};
    size_t frameLength = 0;
    const bool receiveReady = gAttached && gPowered && gState.ready && !gClosing && !gDrainPending &&
                              !gRecoveryRequired && gKnownEpoch && gKnownEpoch == gObservedEpoch;
#if DEVELOPMENT || DEBUG
    if (magic == SWITCH_A3_RAW_RX_MAGIC) {
        ++gDiagnostics.rx_records;
        if (!receiveReady)
            ++gDiagnostics.rx_unready;
    } else {
        ++gDiagnostics.tx_records;
    }
#endif
    if (receiveReady &&
        magic == SWITCH_A3_RAW_RX_MAGIC &&
        switch_a3_rx_decode(frame, length, gSession, gKnownEpoch, gApplied, &header, &frameLength)) {
        swlan_rx_meta meta = {};
        meta.tsf = header.tsf;
        const uint8_t channel = bcm_chanspec_primary(header.chanspec);
        meta.freq_mhz = awdl_chan_freq(awdl_chan{channel, awdl_opclass_20mhz(channel)});
        meta.rssi_dbm = header.rssi;
        meta.flags = SWLAN_RX_FCS_OK;
        meta.generation = gSession;
        meta.clock_epoch = header.clock_epoch;
        meta.applied_generation = header.config_generation;
        const swlan_rx_fn receive = gAttached && meta.freq_mhz ? gRx : nullptr;
        void *context = gRxContext;
#if DEVELOPMENT || DEBUG
        if (receive)
            ++gDiagnostics.rx_delivered;
        else
            ++gDiagnostics.rx_unbound;
#endif
        IOLockUnlock(gLock);
        if (receive)
            receive(context, frame + sizeof header, frameLength, &meta);
        return true;
    }
#if DEVELOPMENT || DEBUG
    if (magic == SWITCH_A3_RAW_RX_MAGIC && receiveReady) {
        if (switch_a3_rx_decode(frame, length, header.session, header.clock_epoch, header.config_generation,
                               &header, &frameLength))
            ++gDiagnostics.rx_stale;
        else
            ++gDiagnostics.rx_invalid;
    }
#endif
    switch_a3_tx_status status = {};
    bool valid = false, report = false;
    uint16_t result = 0;
    if (gAttached && magic == SWITCH_A3_TX_STATUS_MAGIC) {
        for (auto &pending : gPending) {
            if (!pending.id || !pending.transport ||
                !switch_a3_tx_status_decode(frame, length, gSession, pending.epoch, pending.schedule, &status) ||
                pending.id != status.request_id)
                continue;
            const bool acked = (status.flags & (SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED)) ==
                               (SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED);
            result = status.status == SWITCH_A3_TX_SENT        ? (pending.ack && !acked ? EIO : 0)
                     : status.status == SWITCH_A3_TX_EXPIRED   ? ETIMEDOUT
                     : status.status == SWITCH_A3_TX_CANCELLED ? ECANCELED
                                                               : EPROTO;
            if (pending.unicastData && (status.status == SWITCH_A3_TX_SENT || status.status == SWITCH_A3_TX_FAILED)) {
                if (status.status == SWITCH_A3_TX_SENT && acked) {
                    if (gDataRateIndex < kDataRateMax && ++gDataAckStreak >= kDataRateStepUpAcks) {
                        ++gDataRateIndex;
                        gDataAckStreak = 0;
                    }
                } else if (pending.ack) {
                    if (pending.rateIndex == gDataRateIndex && gDataRateIndex > 0)
                        --gDataRateIndex;
                    gDataAckStreak = 0;
                }
            }
#if DEVELOPMENT || DEBUG
            gDiagnostics.tx_last_id = status.request_id;
            gDiagnostics.tx_last_status = status.status;
            gDiagnostics.tx_last_flags = status.flags;
            gDiagnostics.tx_last_tsf = status.tsf;
            gDiagnostics.tx_sent += status.status == SWITCH_A3_TX_SENT;
            gDiagnostics.tx_expired += status.status == SWITCH_A3_TX_EXPIRED;
            gDiagnostics.tx_cancelled += status.status == SWITCH_A3_TX_CANCELLED;
            gDiagnostics.tx_failed += status.status == SWITCH_A3_TX_FAILED;
            gDiagnostics.tx_ack_missing += status.status == SWITCH_A3_TX_SENT && result != 0;
            if (pending.echoType)
                IOLog("SwitchWLAN: A3 echo RF type %u id %u seq %u request %u session %u status %u flags %#x tsf %llu\n",
                      pending.echoType, pending.echoId, pending.echoSequence, pending.id, status.session, status.status,
                      status.flags, status.tsf);
#endif
            report = !pending.notified;
            pending = {};
            ++gOwnershipEvent;
            if (gState.ready)
                gState.tx_credits = creditRoom();
            armTxExpiry();
            valid = true;
            break;
        }
    }
#if DEVELOPMENT || DEBUG
    if (magic == SWITCH_A3_TX_STATUS_MAGIC && !valid)
        ++gDiagnostics.tx_rejected;
#endif
    IOLockUnlock(gLock);
    if (valid) {
        if (report)
            emitRadioEvent(status.session, SWLAN_AWDL_TXDONE, status.request_id, result,
                           (status.flags & SWITCH_A3_TX_TSF_VALID) ? status.tsf : 0);
        emitRadioEvent(status.session, SWLAN_AWDL_TX_CREDIT);
        thread_call_enter(gWorker);
    }
    return true;
}

void switch_wlan_a3_radio_dma_ready(void)
{
    const bool transmit = __atomic_exchange_n(&gDmaBlocked, false, __ATOMIC_ACQ_REL);
    const bool drain = __atomic_exchange_n(&gDrainDmaWait, false, __ATOMIC_ACQ_REL);
    if (transmit || drain)
        thread_call_enter(gWorker);
}
