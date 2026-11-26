// SPDX-License-Identifier: GPL-2.0-only
#include "SwitchDisplayPortService.h"
#include <kern/clock.h>

void SwitchDisplayPort::cecReset()
{
    cecTimer->cancelTimeout();
    cec = {};
    cecRestartPending = false;
    if (cecReleasePending) {
        cecReleasePending = false;
        gate->commandWakeup(&cecReleasePending, false);
    }
}

void SwitchDisplayPort::cecRelease()
{
    cecRestartPending = false;
    cecReleasePending = true;
    stepTimer->cancelTimeout();
    scheduleAux();
    uint64_t deadline = 0;
    clock_interval_to_deadline(200, kMillisecondScale, &deadline);
    // Only the suspend or alt-mode caller sleeps; the event thread must finish AUX_DONE with the gate open.
    while (cecReleasePending) {
        if (gate->commandSleep(&cecReleasePending, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            IOLog("SwitchDisplayPort: timed out waiting for dock CEC release\n");
            cecReleasePending = false;
            break;
        }
    }
}

void SwitchDisplayPort::cecEdid()
{
    if (!nintendoDock)
        return;
    const uint16_t physical = switch_dp_cec_physical(edid, validEdidBlocks);
    if (cec.phase == SWITCH_DP_CEC_OFF && cec.retry_on_hpd && physical != SWITCH_DP_CEC_INVALID_PHYSICAL)
        switch_dp_cec_reconnect(&cec);
    cec.physical = physical;
}

void SwitchDisplayPort::cecComplete(bool ok)
{
    if (!ok && cec.phase == SWITCH_DP_CEC_STOP)
        IOLog("SwitchDisplayPort: dock CEC disable was not acknowledged\n");
    switch_dp_cec_complete(&cec, request.data, ok);
    if (cec.phase == SWITCH_DP_CEC_WAIT && cec.wait_pending) {
        const uint32_t wire = cec.length * 24u;
        cecTimer->setTimeoutMS((wire > 50u ? wire : 50u) + 1010u);
        cec.wait_pending = false;
    } else if (cec.phase != SWITCH_DP_CEC_WAIT && cec.phase != SWITCH_DP_CEC_STATUS) {
        cecTimer->cancelTimeout();
    }
}

void SwitchDisplayPort::cecTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->cec.phase != SWITCH_DP_CEC_WAIT && self->cec.phase != SWITCH_DP_CEC_STATUS)
        return;
    self->cec.phase = SWITCH_DP_CEC_STATUS;
    self->cec.deadline = true;
    self->scheduleAux();
}

void SwitchDisplayPort::scheduleAux()
{
    if (auxPending || auxRetrying)
        return;
    if (cecReleasePending) {
        if (cec.phase != SWITCH_DP_CEC_STOP)
            switch_dp_cec_release(&cec);
        if (cec.phase == SWITCH_DP_CEC_OFF || stage == Stage::Off) {
            cecReleasePending = false;
            gate->commandWakeup(&cecReleasePending, false);
        } else {
            switch_dp_aux_request transaction = {};
            uint32_t wait = 0;
            if (switch_dp_cec_next(&cec, &transaction, &wait))
                auxStart(transaction, Next::Cec);
            return;
        }
    }
    if (sleeping || stage == Stage::Off)
        return;
    if (cecRestartPending) {
        cecRestartPending = false;
        switch_dp_cec_reconnect(&cec);
    }
    if (probePending && stage == Stage::Waiting) {
        probePending = false;
        stage = Stage::Probing;
        auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_REV, nullptr, sizeof(caps), Next::Caps);
        return;
    }
    if (stage == Stage::Probing || stage == Stage::Training)
        return;
    if (modePending) {
        modePending = false;
        chooseAndTrain();
        return;
    }
    if (irqPending) {
        irqPending = false;
        if (stage == Stage::Running) {
            auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_SINK_COUNT, nullptr, 6, Next::LinkStatus);
            return;
        }
        cec.service = true;
    }
    cec.active = stage == Stage::Running && screen->requested();
    switch_dp_aux_request transaction = {};
    uint32_t wait = 0;
    if (switch_dp_cec_next(&cec, &transaction, &wait))
        auxStart(transaction, Next::Cec);
    else if (wait)
        cecTimer->setTimeoutMS(wait);
}
