// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "SwitchWLANA3Clock.h"
#include "SwitchWLANA3Probe.h"
#include "SwitchWLANA3TxProbe.h"
#include "SwitchWLANA3Wire.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace {
thread_call_t gReadDeadline;
uint8_t gAddress[6];
int gD11n;
bool gStarted;
uint32_t gSession, gEpoch, gRxCount;

int command(uint32_t operation, uint32_t epoch, const void *payload, size_t length, switch_a3_reply &reply)
{
    SwitchWLANA3Job job = {};
    job.control.magic = SWITCH_A3_CONTROL_MAGIC;
    job.control.version = SWITCH_A3_WIRE_VERSION;
    job.control.header_bytes = sizeof job.control;
    job.control.total_bytes = static_cast<uint32_t>(sizeof job.control + length);
    job.control.request_id = 0x80000000u | operation;
    job.control.session = gSession;
    job.control.clock_epoch = epoch;
    job.control.config_generation = 1;
    job.control.operation = operation;
    if (length)
        memcpy(job.payload, payload, length);
    uint64_t before = 0, after = 0;
    const int result = switch_wlan_a3_exchange(job, reply, before, after);
    IOLog("SwitchWLAN: A3 qualification: operation %u answered %d, session %u epoch %u applied %u TSF %llu\n",
          operation, result, reply.session, reply.clock_epoch, reply.applied_generation,
          static_cast<unsigned long long>(reply.tsf));
    return result;
}

void deadlineResult(thread_call_param_t, thread_call_param_t)
{
    switch_a3_deadline_snapshot snapshot = {};
    size_t returned = 0;
    const int result =
        swlan_awdl_var(0, 0, SWITCH_A3_DEADLINE_IOVAR, nullptr, 0, &snapshot, sizeof snapshot, &returned);
    if (result || returned != sizeof snapshot || snapshot.reserved || snapshot.session_generation != gSession ||
        snapshot.clock_generation != __atomic_load_n(&gEpoch, __ATOMIC_ACQUIRE)) {
        IOLog("SwitchWLAN: A3 qualification: deadline snapshot rejected (%d, %u bytes)\n", result,
              static_cast<unsigned>(returned));
        return;
    }
    const int64_t delta = static_cast<int64_t>(snapshot.actual_tsf) - static_cast<int64_t>(snapshot.requested_tsf);
    IOLog("SwitchWLAN: A3 qualification: deadline fired %u armed %u status %d, requested %llu actual %llu delta %lld "
          "us\n",
          snapshot.fired, snapshot.armed, snapshot.status, static_cast<unsigned long long>(snapshot.requested_tsf),
          static_cast<unsigned long long>(snapshot.actual_tsf), static_cast<long long>(delta));
    if (snapshot.fired == 1 && !snapshot.armed && !snapshot.status && delta >= 0)
        switch_wlan_a3_tx_probe_start();
}

void run(thread_call_param_t, thread_call_param_t)
{
    swlan_awdl_state state = {};
    if (switch_wlan_a3_radio_state(&state) || !state.generation)
        return;
    gSession = state.generation;
    switch_a3_reply reply = {};
    if (command(SWITCH_A3_GET_CLOCK, 0, nullptr, 0, reply))
        return;
    const uint32_t epoch = reply.clock_epoch;
    switch_a3_configuration config = {};
    memcpy(config.address, gAddress, 6);
    memcpy(config.bssid, awdl_bssid, 6);
    config.chanspec = bcm_chanspec20(6, gD11n);
    config.flags = SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK;
    if (command(SWITCH_A3_CONFIGURE, epoch, &config, sizeof config, reply))
        return;
    const uint32_t enable = 1;
    if (command(SWITCH_A3_TEST_RECEIVE, epoch, &enable, sizeof enable, reply)) {
        command(SWITCH_A3_STOP, epoch, nullptr, 0, reply);
        return;
    }
    __atomic_store_n(&gEpoch, epoch, __ATOMIC_RELEASE);
    IOLog("SwitchWLAN: A3 qualification: receive/ACK address %02x:%02x:%02x:%02x:%02x:%02x; operational features "
          "remain zero\n",
          gAddress[0], gAddress[1], gAddress[2], gAddress[3], gAddress[4], gAddress[5]);
    switch_wlan_a3_radio_refresh();
    if (command(SWITCH_A3_GET_CLOCK, 0, nullptr, 0, reply))
        return;
    const uint64_t deadline = reply.tsf + 50000;
    if (command(SWITCH_A3_TEST_DEADLINE, epoch, &deadline, sizeof deadline, reply))
        return;
    uint64_t hostDeadline = 0;
    clock_interval_to_deadline(100, kMillisecondScale, &hostDeadline);
    thread_call_enter_delayed(gReadDeadline, hostDeadline);
}
}

void switch_wlan_a3_probe(const uint8_t mac[6], int d11n)
{
    if (__atomic_exchange_n(&gStarted, true, __ATOMIC_ACQ_REL))
        return;
    thread_call_t worker = thread_call_allocate(run, nullptr);
    gReadDeadline = thread_call_allocate(deadlineResult, nullptr);
    if (!worker || !gReadDeadline) {
        if (worker)
            thread_call_free(worker);
        if (gReadDeadline)
            thread_call_free(gReadDeadline);
        return;
    }
    memcpy(gAddress, mac, 6);
    gAddress[0] = static_cast<uint8_t>((gAddress[0] | 2u) & ~1u);
    gAddress[5] ^= 0x80;
    gD11n = d11n;
    thread_call_enter(worker);
}

void switch_wlan_a3_probe_rx(const uint8_t *data, size_t length)
{
    const uint32_t epoch = __atomic_load_n(&gEpoch, __ATOMIC_ACQUIRE);
    if (!epoch || gRxCount >= 4)
        return;
    switch_a3_raw_rx header = {};
    size_t frameLength = 0;
    if (!switch_a3_rx_decode(data, length, gSession, epoch, 1, &header, &frameLength))
        return;
    ++gRxCount;
    IOLog("SwitchWLAN: A3 qualification: raw RX %u bytes channel %#x RSSI %d TSF %llu\n",
          static_cast<unsigned>(frameLength), header.chanspec, header.rssi,
          static_cast<unsigned long long>(header.tsf));
}
#endif
