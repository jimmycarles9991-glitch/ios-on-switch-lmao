// SPDX-License-Identifier: BSD-2-Clause-Patent
// Two-slot prepared ADMA2 pipeline state machine for primary-IRQ command chaining (NativeSwitchBlock.cpp).
// Kept header-only so host tests (test/sd_pipeline_test.cpp) exercise every transition, race, and error path.
#pragma once

#include <stdint.h>
#include "SwitchSdBus.h"
#include "SwitchSdCard.h"

struct SwitchSdPipeSlot {
    uint64_t lba = 0;
    uint32_t sectors = 0;
    uint32_t descriptors = 0;
    bool write = false;
    bool direct = false;
    bool staged = false;
    bool cmd23 = false;
    bool preparedCache = false;
    bool inUse = false;
    bool irqDone = false;
    bool timedOut = false;
    bool fastIssued = false;
    int issueStep = 0;
    uint32_t irqStatus = 0;
    uint8_t irqAdmaState = 0;
    uint64_t irqAdmaAddress = 0;
    uint64_t issuedAt = 0;
    uint64_t irqAt = 0;
    SwitchSdFrame frame = {};
};

enum class SwitchSdIrqAction : uint8_t {
    kLegacyWake,  // non-pipelined command (waitSignal / awaitStatus)
    kFastIssued,  // active finished cleanly and prepared was issued on the wire
    kDoneIdle,    // active finished cleanly; no prepared ready, bus now idle
    kSlowWake,    // error, DAT0 still busy, or done != nullptr; worker finishes active
};

struct SwitchSdIrqSample {
    uint32_t status;
    uint32_t response;
    uint16_t autoCmdError;
    uint32_t stopResponse;
    uint32_t present;
    uint8_t admaError;
    uint64_t admaAddress;
    uint64_t nowNs;
};

template <typename Slot> struct SwitchSdPipeline {
    // Four mutually exclusive slot owners: active/prepared/done guarded by hwLock; stashed is worker-only.
    Slot *active = nullptr;
    Slot *prepared = nullptr;
    Slot *done = nullptr;
    Slot *stashed = nullptr;

    static bool lbaOverlaps(const SwitchSdPipeSlot &a, const SwitchSdPipeSlot &b)
    {
        return a.sectors != 0 && b.sectors != 0 && a.lba < b.lba + b.sectors && b.lba < a.lba + a.sectors;
    }

    static bool canOverlap(const SwitchSdPipeSlot &cand, const SwitchSdPipeSlot &other, bool paOverlaps)
    {
        return !lbaOverlaps(cand, other) && !(cand.staged && other.staged) && !paOverlaps;
    }

    // Primary IRQ context, hwLock held, SIGNAL_ENABLE already masked to 0.
    // On kFastIssued, emitCmd(prepared) writes the SDHCI registers and re-enables SIGNAL_ENABLE.
    // On kSlowWake, bus and STATUS are left untouched for worker recovery.
    template <typename EmitCmd>
    SwitchSdIrqAction onIrq(SwitchSdBus &bus, const SwitchSdIrqSample &s, uint32_t errorMask, uint32_t r1Errors,
                            uint32_t r1StopErrors, uint32_t admaErrorBit, EmitCmd &&emitCmd)
    {
        if (!active)
            return SwitchSdIrqAction::kLegacyWake;
        active->irqAt = s.nowNs;
        const bool clean =
            (done == nullptr) && active->direct && !(s.status & errorMask) &&
            ((s.status & (kSdCommandComplete | kSdTransferComplete)) == (kSdCommandComplete | kSdTransferComplete)) &&
            !(s.response & r1Errors) && !(active->sectors > 1 && (s.autoCmdError & 0x9fu)) &&
            !(active->sectors > 1 && !active->cmd23 && (s.stopResponse & r1StopErrors));
        const bool released = !bus.card || ((s.present & kSdPresentDat0) != 0u);
        if (clean && released) {
            if (prepared) {
                SwitchSdBus nextBus = bus;
                nextBus.ended(s.status, s.present);
                const SwitchSdUse nextUse = prepared->write ? SwitchSdUse::kWrite : SwitchSdUse::kRead;
                if (nextBus.state(s.present, nextUse) == SwitchSdBusState::kFree) {
                    bus.ended(s.status, s.present);
                    bus.issued(nextUse);
                    prepared->issuedAt = s.nowNs;
                    prepared->fastIssued = true;
                    emitCmd(*prepared);
                    done = active;
                    active = prepared;
                    prepared = nullptr;
                    return SwitchSdIrqAction::kFastIssued;
                }
            }
            bus.ended(s.status, s.present);
            done = active;
            active = nullptr;
            return SwitchSdIrqAction::kDoneIdle;
        }
        active->irqStatus = s.status;
        if (s.status & admaErrorBit) {
            active->irqAdmaState = s.admaError;
            active->irqAdmaAddress = s.admaAddress;
        }
        active->irqDone = true;
        return SwitchSdIrqAction::kSlowWake;
    }

    Slot *takeDone()
    {
        Slot *s = done;
        done = nullptr;
        return s;
    }

    Slot *retractPrepared()
    {
        Slot *s = prepared;
        prepared = nullptr;
        return s;
    }

    bool shouldSleep() const
    {
        return done == nullptr && active != nullptr && !active->irqDone;
    }

    bool idle() const
    {
        return active == nullptr && prepared == nullptr && done == nullptr && stashed == nullptr;
    }

    // Worker timeout check under hwLock: ignores stale timeouts if an IRQ already rotated or finished `expected`.
    bool noteTimeout(const Slot *expected, uint32_t status, uint64_t nowNs, uint64_t limitNs)
    {
        if (active != expected || active->irqDone)
            return false;
        if (nowNs >= active->issuedAt && nowNs - active->issuedAt >= limitNs) {
            active->irqStatus = status;
            active->timedOut = true;
            active->irqDone = true;
            return true;
        }
        return false;
    }
};
