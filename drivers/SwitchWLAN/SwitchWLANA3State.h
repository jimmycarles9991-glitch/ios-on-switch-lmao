// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_A3_STATE_H
#define SWITCH_WLAN_A3_STATE_H

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include "SwitchWLANA3Radio.h"
#include "SwitchWLANA3Wire.h"
#include "SwitchWLANA3Clock.h"
#include "SwitchWLANA3Diagnostic.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace switch_a3_host {
#if DEVELOPMENT || DEBUG
extern SwitchWLANA3Diagnostics gDiagnostics;
#endif
constexpr unsigned kJobs = 16;
constexpr uint32_t kRadioCredits = 8;
constexpr uint32_t kLegacy6Mbps = 12;
constexpr uint32_t kDataRates[] = {12, 24, 48, 72, 108};
constexpr uint8_t kDataRateMax = 4;
constexpr unsigned kDataRateStepUpAcks = 8;
constexpr uint64_t kDrainExpired = UINT64_MAX;

struct TxJob {
    uint32_t session, id, lifetime;
    uint16_t bytes;
    uint8_t frame[14 + sizeof(switch_a3_tx_header) + SWITCH_A3_MAX_FRAME];
};

struct PendingTx {
    uint32_t id;
    bool ack, transport, notified, unicastData;
    uint8_t rateIndex;
    uint64_t expires_ns, submit_deadline_ns;
    uint32_t epoch, schedule;
#if DEVELOPMENT || DEBUG
    uint16_t echoId = 0, echoSequence = 0;
    uint8_t echoType = 0;
#endif
};

using Job = SwitchWLANA3Job;
extern IOLock *gLock;
extern thread_call_t gWorker, gTxExpiry;
extern Job gJobs[kJobs];
extern TxJob gTx[kJobs];
extern PendingTx gPending[kJobs];
extern swlan_awdl_event gCancelled[kJobs];
extern unsigned gCancelCount, gHead, gCount, gTxHead, gTxCount, gDataAckStreak;
extern uint8_t gDataRateIndex;
extern bool gBusy, gAttached, gClockQueued, gPowered, gRecoveryRequired, gDmaBlocked;
extern bool gDrainDmaWait, gDrainPending, gDrainWaitEvent, gClosing, gHaveConfiguration, gHaveSchedule, gStopped;
extern bool gStationPaused, gPauseReady;
extern bool gScheduleUpdatePending, gScheduleWaitEvent, gScheduleRejected;
extern uint32_t gScheduleRequest, gScheduleEvent;
extern bool gVerifyState;
extern uint32_t gOwnershipEvent, gDrainEvent;
extern uint64_t gDrainDeadlineNs;
extern switch_a3_configuration gDesiredConfiguration;
extern switch_a3_schedule gDesiredSchedule, gAppliedSchedule;
extern uint32_t gSession, gRequest, gConfiguration, gApplied, gFeatures, gSchedule, gAddressGeneration;
extern uint32_t gFirmwareGeneration, gLifetime, gKnownEpoch, gBound, gCreditLimit;
extern uint32_t gFirmwareOwnerGeneration;
extern uint32_t gStateRevision, gObservedEpoch;
extern switch_a3_ring_binding gRings;
extern bool gConfigured;
extern int gClockError, gRetireError, gD11n;
extern uint8_t gMac[6];
extern swlan_awdl_state gState;
extern swlan_rx_fn gRx;
extern void *gRxContext;
uint64_t hostNs();
unsigned pendingCount();
uint32_t creditRoom();
bool observeReply(const switch_a3_reply &reply);
bool stoppedForOwner(const switch_a3_reply &reply, bool directStop = false);
bool applySnapshot(const switch_a3_reply &reply);
void updateCredits(uint32_t credits);
void armTxExpiry();
void expireTx(thread_call_param_t, thread_call_param_t);
void cancelTx(bool retainSubmitted = false);
void resetScheduleUpdate();
void requestScheduleUpdate();
bool queueScheduleUpdate();
bool scheduleGenerationCurrent(uint32_t generation);
bool observeSchedule(const switch_a3_reply &reply);
void finishScheduleUpdate(const Job &job, const switch_a3_reply &reply, int error, uint32_t observedEvent);
void deferDesired(bool waitForOwnership, uint32_t observedEvent);
void emitRadioEvent(uint32_t session, uint32_t type, uint32_t id = 0, uint16_t status = 0, uint64_t tsf = 0);
void work(thread_call_param_t, thread_call_param_t);
int enqueueLocked(uint32_t operation, const void *payload, size_t length, bool changes);
int queueControl(uint32_t operation, const void *payload, size_t length, bool changes);
}
#endif
