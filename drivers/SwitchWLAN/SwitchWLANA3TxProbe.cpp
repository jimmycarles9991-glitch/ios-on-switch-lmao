// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include "SwitchWLANA3Clock.h"
#include "SwitchWLANA3TxProbe.h"
#include "SwitchWLANA3Wire.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace {
constexpr uint32_t kGeneration = 2, kRequest = 0x7f000001, kDeadlineMs = 500;
constexpr uint64_t kLifetimeUs = static_cast<uint64_t>(kDeadlineMs) * 1000;
enum class Phase {
    Prepared,
    Starting,
    Waiting,
    Finishing,
    Draining,
    Done
};
IOLock *gLock;
thread_call_t gWorker, gTimer;
Phase gPhase = Phase::Done;
uint8_t gAddress[6], gEnvelope[14 + sizeof(switch_a3_tx_header) + AWDL_MAX_FRAME];
size_t gLength;
switch_a3_ring_binding gRings;
switch_a3_tx_status gStatus;
uint32_t gSession, gEpoch;
uint64_t gDeadlineNs;
int gFailure;
bool gPrepared, gUsed, gAuthorized, gSubmitted, gCompleted, gTimedOut, gStopFailed, gLeased;

uint64_t hostNow()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

bool current()
{
    swlan_awdl_state state = {};
    return !switch_wlan_a3_radio_state(&state) && state.generation == gSession &&
           (!gEpoch || state.clock.epoch == gEpoch);
}

int command(uint32_t operation, uint32_t generation, const void *payload, size_t length, switch_a3_reply &reply)
{
    if (!current())
        return -ESTALE;
    SwitchWLANA3Job job = {};
    job.control = {SWITCH_A3_CONTROL_MAGIC,
                   SWITCH_A3_WIRE_VERSION,
                   sizeof job.control,
                   static_cast<uint32_t>(sizeof job.control + length),
                   0x90000000u | operation,
                   gSession,
                   operation == SWITCH_A3_GET_CLOCK ? 0u : gEpoch,
                   generation,
                   operation};
    if (length)
        memcpy(job.payload, payload, length);
    uint64_t before = 0, after = 0;
    int result = switch_wlan_a3_exchange(job, reply, before, after);
    if (!result &&
        (reply.firmware_generation != 1 || !current() || (gEpoch && reply.clock_epoch != gEpoch) ||
         (operation != SWITCH_A3_GET_CLOCK && operation != SWITCH_A3_STOP && reply.applied_generation != generation)))
        result = -ESTALE;
    IOLog("SwitchWLAN: A3 TX qualification: operation %u result %d session %u epoch %u applied %u\n", operation, result,
          reply.session, reply.clock_epoch, reply.applied_generation);
    return result;
}

int refuseTx(void *, const uint8_t *, size_t, uint16_t, uint64_t, int)
{
    return -ENXIO;
}

size_t buildFrame(uint64_t tsf, uint16_t chanspec, uint8_t *frame, size_t capacity)
{
    const uint8_t channel = bcm_chanspec_primary(chanspec);
    const uint8_t opclass = awdl_opclass_20mhz(channel);
    if (!opclass)
        return 0;
    auto *core = static_cast<awdl_core *>(IOMallocZero(awdl_core_size()));
    if (!core)
        return 0;
    awdl_config config = {};
    awdl_config_default(&config, gAddress);
    config.backend = AWDL_BACKEND_SOFT;
    awdl_host host = {};
    host.tx = refuseTx;
    size_t length = 0;
    if (!awdl_core_init(core, &host, &config)) {
        awdl_core_start(core, tsf);
        awdl_chan sequence[AWDL_SEQ_SLOTS];
        for (auto &slot : sequence)
            slot = {channel, opclass};
        awdl_core_set_sequence(core, sequence, tsf);
        length = awdl_core_build_af(core, AWDL_SUBTYPE_PSF, tsf, frame, capacity);
    }
    IOFree(core, awdl_core_size());
    return length;
}

void armDeadline()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kDeadlineMs, kMillisecondScale, &deadline);
    gDeadlineNs = hostNow() + kLifetimeUs * 1000;
    thread_call_enter_delayed(gTimer, deadline);
}

void fail(int error)
{
    IOLockLock(gLock);
    gFailure = error;
    gAuthorized = false;
    gPhase = Phase::Finishing;
    IOLockUnlock(gLock);
}

void begin()
{
    swlan_awdl_state state = {};
    if (switch_wlan_a3_radio_state(&state) || !state.generation || state.ready) {
        fail(-EAGAIN);
        return;
    }
    gSession = state.generation;
    switch_a3_reply reply = {};
    int result = command(SWITCH_A3_GET_CLOCK, 0, nullptr, 0, reply);
    if (result) {
        fail(result);
        return;
    }
    gEpoch = reply.clock_epoch;
    result = switch_wlan_a3_tx_probe_claim(gSession, gEpoch);
    if (result) {
        fail(result);
        return;
    }
    gLeased = true;
    result = command(SWITCH_A3_STOP, 1, nullptr, 0, reply);
    if (result) {
        gStopFailed = true;
        fail(result);
        return;
    }
    uint8_t channelBytes[4] = {};
    size_t returned = 0;
    result = swlan_awdl_var(0, 0, "chanspec", nullptr, 0, channelBytes, sizeof channelBytes, &returned);
    const uint32_t channel = static_cast<uint32_t>(channelBytes[0]) | (static_cast<uint32_t>(channelBytes[1]) << 8) |
                             (static_cast<uint32_t>(channelBytes[2]) << 16) |
                             (static_cast<uint32_t>(channelBytes[3]) << 24);
    if (result || returned != sizeof channelBytes || !channel || channel > UINT16_MAX) {
        fail(result ? result : -EINVAL);
        return;
    }
    const auto chanspec = static_cast<uint16_t>(channel);
    switch_a3_configuration config = {};
    memcpy(config.address, gAddress, sizeof config.address);
    memcpy(config.bssid, awdl_bssid, sizeof config.bssid);
    config.chanspec = chanspec;
    config.flags = SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK;
    result = command(SWITCH_A3_CONFIGURE, 1, &config, sizeof config, reply);
    switch_a3_schedule schedule = {};
    schedule.anchor_tsf = reply.tsf;
    schedule.period_us = kLifetimeUs;
    schedule.infra_chanspec = chanspec;
    schedule.slot_count = 1;
    schedule.slots[0] = {chanspec, 0, static_cast<uint32_t>(kLifetimeUs)};
    if (!result)
        result = command(SWITCH_A3_INSTALL_SCHEDULE, kGeneration, &schedule, sizeof schedule, reply);
    if (!result)
        result = command(SWITCH_A3_BIND_RINGS, kGeneration, &gRings, sizeof gRings, reply);
    if (!result)
        result = command(SWITCH_A3_START, kGeneration, nullptr, 0, reply);
    if (!result && (reply.state != SWITCH_A3_RUNNING || !reply.tx_credits || reply.features))
        result = -EPROTO;
    if (!result)
        result = command(SWITCH_A3_GET_CLOCK, kGeneration, nullptr, 0, reply);
    if (result || reply.tsf > UINT64_MAX - kLifetimeUs) {
        fail(result ? result : -ERANGE);
        return;
    }
    uint8_t *frame = gEnvelope + 14 + sizeof(switch_a3_tx_header);
    const size_t frameLength = buildFrame(reply.tsf, chanspec, frame, AWDL_MAX_FRAME);
    if (!frameLength) {
        fail(-EINVAL);
        return;
    }
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    memcpy(gEnvelope, marker, sizeof marker);
    gEnvelope[6] = 2;
    gEnvelope[7] = 0x57;
    for (unsigned i = 0; i < 4; ++i)
        gEnvelope[8 + i] = static_cast<uint8_t>(kRequest >> (i * 8));
    gEnvelope[12] = 0x88;
    gEnvelope[13] = 0xb5;
    switch_a3_tx_header header = {};
    header.magic = SWITCH_A3_TX_MAGIC;
    header.version = SWITCH_A3_WIRE_VERSION;
    header.header_bytes = sizeof header;
    header.total_bytes = static_cast<uint32_t>(sizeof header + frameLength);
    header.session = gSession;
    header.clock_epoch = gEpoch;
    header.schedule_generation = kGeneration;
    header.request_id = kRequest;
    header.frame_bytes = static_cast<uint16_t>(frameLength);
    header.chanspec = chanspec;
    header.flags = SWITCH_A3_TX_NO_ACK | SWITCH_A3_TX_ON_CHANNEL_ONLY;
    header.rate = 12;
    header.earliest_tsf = reply.tsf;
    header.expiry_tsf = reply.tsf + kLifetimeUs;
    header.firmware_generation = 1;
    memcpy(gEnvelope + 14, &header, sizeof header);
    IOLockLock(gLock);
    gLength = 14 + sizeof header + frameLength;
    gAuthorized = true;
    gSubmitted = true;
    gPhase = Phase::Waiting;
    armDeadline();
    IOLockUnlock(gLock);
    result = switch_wlan_a3_tx_probe_transmit(gEnvelope, gLength);
    if (result) {
        IOLockLock(gLock);
        gSubmitted = false;
        IOLockUnlock(gLock);
        fail(result);
    } else {
        IOLog("SwitchWLAN: A3 TX qualification: one PSF submitted, %u bytes channel %#x request %u; features zero\n",
              static_cast<unsigned>(frameLength), chanspec, kRequest);
    }
}

void finish(bool expired)
{
    const bool lost = !gLeased || !current();
    if (lost) {
        IOLog("SwitchWLAN: A3 TX qualification: lease absent or lifetime changed; old cleanup discarded\n");
    } else {
        switch_wlan_a3_probe_ownership ownership = {};
        const int ownershipResult =
            switch_wlan_a3_tx_probe_cleanup(gSession, gEpoch, SWITCH_A3_PROBE_CLOSE, &ownership);
        switch_a3_reply reply = {};
        const int result = gStopFailed ? gFailure : command(SWITCH_A3_STOP, kGeneration, nullptr, 0, reply);
        gStopFailed = result || reply.state != SWITCH_A3_STOPPED;
        IOLog("SwitchWLAN: A3 TX qualification: stopped %d, private DMA outstanding %u rings closed %u; RF status %s\n",
              result, ownership.dma_pending, ownership.rings_closed, gCompleted ? "received" : "unproven");
        IOLockLock(gLock);
        gPhase = Phase::Draining;
        armDeadline();
        IOLockUnlock(gLock);
        if ((!ownership.dma_pending && ownership.rings_closed) || ownershipResult || expired || gStopFailed)
            thread_call_enter(gWorker);
        return;
    }
    IOLockLock(gLock);
    gPhase = Phase::Done;
    IOLockUnlock(gLock);
}

void work(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const Phase phase = gPhase;
    IOLockUnlock(gLock);
    if (phase == Phase::Starting)
        begin();
    IOLockLock(gLock);
    const bool finishing = gPhase == Phase::Finishing;
    const bool timedOut = gTimedOut;
    IOLockUnlock(gLock);
    if (finishing)
        finish(timedOut);
    else if (phase == Phase::Draining) {
        switch_wlan_a3_probe_ownership ownership = {};
        const int result = switch_wlan_a3_tx_probe_cleanup(gSession, gEpoch, 0, &ownership);
        const bool expired = hostNow() >= gDeadlineNs;
        const bool retired = !result && !ownership.dma_pending && ownership.rings_closed;
        if (!retired && !expired && !gStopFailed)
            return;
        const bool recovery = gStopFailed || !retired || (gSubmitted && !gCompleted);
        int cleanupResult = -ESTALE;
        if (current())
            cleanupResult = switch_wlan_a3_tx_probe_cleanup(
                gSession, gEpoch, recovery ? SWITCH_A3_PROBE_RECOVER : SWITCH_A3_PROBE_RELEASE, &ownership);
        IOLog("SwitchWLAN: A3 TX qualification: result %d status %d flags %#x TSF %s, DMA retired %u rings closed %u, "
              "recovery %u\n",
              gTimedOut ? -ETIMEDOUT : gFailure, gCompleted ? gStatus.status : -1, gCompleted ? gStatus.flags : 0,
              gCompleted && (gStatus.flags & SWITCH_A3_TX_TSF_VALID) ? "valid" : "unavailable",
              !result && !ownership.dma_pending, !result && ownership.rings_closed, recovery && !cleanupResult);
        if (cleanupResult)
            IOLog("SwitchWLAN: A3 TX qualification: final ownership changed (%d)\n", cleanupResult);
        IOLockLock(gLock);
        gPhase = Phase::Done;
        thread_call_cancel(gTimer);
        IOLockUnlock(gLock);
    }
}

void deadline(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    if (gPhase == Phase::Waiting) {
        gAuthorized = false;
        gTimedOut = true;
        gPhase = Phase::Finishing;
    }
    const bool wake = gPhase == Phase::Finishing || gPhase == Phase::Draining;
    IOLockUnlock(gLock);
    if (wake)
        thread_call_enter(gWorker);
}
}

void switch_wlan_a3_tx_probe_prepare(const uint8_t mac[6], int d11n, uint16_t firstRing, unsigned ringCount)
{
    if (gPrepared || !mac || d11n || firstRing < 2 || !ringCount || ringCount > UINT16_MAX ||
        static_cast<unsigned>(firstRing) + ringCount > 65536u)
        return;
    IOLock *lock = IOLockAlloc();
    thread_call_t worker = thread_call_allocate(work, nullptr);
    thread_call_t timer = thread_call_allocate(deadline, nullptr);
    if (!lock || !worker || !timer) {
        if (lock)
            IOLockFree(lock);
        if (worker)
            thread_call_free(worker);
        if (timer)
            thread_call_free(timer);
        return;
    }
    gLock = lock;
    gWorker = worker;
    gTimer = timer;
    gRings = {firstRing, static_cast<uint16_t>(ringCount), SWITCH_A3_MAX_FRAME, 0, 1, 0};
    memcpy(gAddress, mac, sizeof gAddress);
    gAddress[0] = static_cast<uint8_t>((gAddress[0] | 2u) & ~1u);
    gAddress[5] ^= 0x80;
    gPhase = Phase::Prepared;
    gPrepared = true;
}

void switch_wlan_a3_tx_probe_start()
{
    if (!gPrepared)
        return;
    IOLockLock(gLock);
    if (!gUsed && gPhase == Phase::Prepared) {
        gUsed = true;
        gPhase = Phase::Starting;
        thread_call_enter(gWorker);
    }
    IOLockUnlock(gLock);
}

bool switch_wlan_a3_tx_probe_authorize(const uint8_t *envelope, size_t length)
{
    if (!gPrepared || !envelope)
        return false;
    IOLockLock(gLock);
    const bool valid = gPhase == Phase::Waiting && gAuthorized && hostNow() < gDeadlineNs && length == gLength &&
                       !memcmp(envelope, gEnvelope, gLength);
    if (valid)
        gAuthorized = false;
    IOLockUnlock(gLock);
    return valid;
}

void switch_wlan_a3_tx_probe_observe(const uint8_t *frame, size_t length)
{
    if (!gPrepared)
        return;
    IOLockLock(gLock);
    switch_a3_tx_status status = {};
    const bool valid = gPhase == Phase::Waiting && gSubmitted &&
                       switch_a3_tx_status_decode(frame, length, gSession, gEpoch, kGeneration, &status) &&
                       status.request_id == kRequest;
    if (valid) {
        gStatus = status;
        gCompleted = true;
        gAuthorized = false;
        gFailure = status.status == SWITCH_A3_TX_SENT ? 0 : -EIO;
        gPhase = Phase::Finishing;
    }
    IOLockUnlock(gLock);
    if (valid)
        thread_call_enter(gWorker);
}

void switch_wlan_a3_tx_probe_dma(unsigned outstanding, bool closed)
{
    if (!gPrepared || outstanding || !closed)
        return;
    IOLockLock(gLock);
    const bool wake = gPhase == Phase::Draining;
    IOLockUnlock(gLock);
    if (wake)
        thread_call_enter(gWorker);
}
#endif
