// SPDX-License-Identifier: GPL-2.0-only
// The SwitchDisplayPort service: one work loop owns DPAUX1, SOR1, display controller B and the external screen.
// SwitchDisplayPort.cpp has the service, its interrupts and the AUX executor; SwitchDPSession.cpp walks a hot-plugged
// sink from its capabilities to a trained, scanning link.
#ifndef SWITCH_DISPLAY_PORT_SERVICE_H
#define SWITCH_DISPLAY_PORT_SERVICE_H

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#include "SwitchDPAux.h"
#include "SwitchDPCEC.h"
#include "SwitchDPLink.h"
#include "SwitchDPModes.h"
#include "SwitchDPTraining.h"
#include "SwitchExternalScreen.h"
#include "TegraDisplayB.h"
#include "TegraDpaux1.h"
#include "TegraSor1.h"

class SwitchDisplayPort : public IOService {
    OSDeclareDefaultStructors(SwitchDisplayPort);

public:
    bool begin();
    void stop(IOService *provider) override;
    void free() override;

    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    void setAltLanes(unsigned lanes, bool flipped, bool dock, uint32_t attachment);
    IOReturn selectMode(int index, bool enable);
    void systemSleep(bool entering);
    bool gpuComplete(void *display, int status);
    bool audioPreparePresence();
    bool audioFormat(uint32_t rate, uint32_t channels, uint32_t bits);
    void audioEnable(bool enable);
    void audioPresence(const uint8_t *eld, uint32_t bytes, bool present);

private:
    enum class Stage : uint8_t {
        Off,
        Waiting,
        Probing,
        Ready,
        Training,
        Running
    };
    enum class Next : uint8_t {
        Caps,
        Powered,
        Edid,
        LinkSet,
        Coding,
        TrainingSet,
        PostcursorSet,
        TrainingStopped,
        TrainingStatus,
        Trained,
        LinkStatus,
        Cec
    };

    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOInterruptEventSource *auxSource = nullptr, *dcSource = nullptr;
    IOTimerEventSource *auxTimer = nullptr, *stepTimer = nullptr, *cecTimer = nullptr;
    TegraDpaux1 aux;
    TegraSor1 sor;
    TegraDisplayB dcb;
    SwitchExternalScreen *screen = nullptr;

    Stage stage = Stage::Off;
    unsigned altLanes = 0;
    uint32_t activeAttachment = 0;
    bool flipped = false, nintendoDock = false;
    bool dcbUp = false, sorUp = false, sleeping = false;
    int wantedMode = -1;
    uint64_t modeGeneration = 0;
    uint8_t trainingEvent = 0;

    struct switch_dp_aux_request request = {};
    Next next = Next::Caps;
    unsigned attempt = 0;
    bool auxPending = false, auxRetrying = false, auxDiscard = false;

    uint8_t caps[16] = {};
    uint8_t edid[512] = {};
    uint32_t validEdidBlocks = 0;
    unsigned edidBlock = 0, edidBlocks = 0, edidStep = 0, edidSteps = 0, edidRead = 0;
    struct switch_dp_aux_request edidPlan[SWITCH_DP_EDID_REQUESTS] = {};
    struct switch_dp_mode_list modes = {};
    struct switch_dp_mode mode = {};
    unsigned modeIndex = 0;
    struct switch_dp_link link = {};
    struct switch_dp_tu tu = {};
    uint8_t maxRate = 0, maxLanes = 0;
    unsigned linkCursor = 0;
    struct switch_dp_train train = {};
    struct switch_dp_train_out trainOut = {};
    struct switch_dp_cec cec = {};
    bool probePending = false, modePending = false, irqPending = false;
    bool cecReleasePending = false, cecRestartPending = false;

    static void auxInterrupt(OSObject *owner, IOInterruptEventSource *, int);
    static void dcInterrupt(OSObject *owner, IOInterruptEventSource *, int);
    static void auxTimerFired(OSObject *owner, IOTimerEventSource *);
    static void stepTimerFired(OSObject *owner, IOTimerEventSource *);

    static void cecTimerFired(OSObject *, IOTimerEventSource *);
    void scheduleAux();
    void cecReset();
    void cecRelease();
    void cecComplete(bool ok);
    void cecEdid();

    void auxStart(const struct switch_dp_aux_request &transaction, Next then);
    void auxIssue();
    void auxNative(enum switch_dp_aux_op op, uint32_t address, const uint8_t *bytes, uint8_t size, Next then);
    void auxDone();
    void fail(const char *what, const char *why);
    void teardownLink();
    void powerOff();
    void wakeModeWaiters(bool abort);
    void releaseResources();

    void plugged();
    void unplugged();
    void irqHpd();
    void dispatch();
    void edidNext();
    void edidDone();
    void chooseAndTrain(bool nextChoice = false);
    void applyTraining();
    void trained();
};

#endif
