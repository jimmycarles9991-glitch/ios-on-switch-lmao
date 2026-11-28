// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchAudioAbi.h"
#include "SwitchHDAFormat.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

class SwitchHDA {
    volatile uint8_t *registers = nullptr;
    IOWorkLoop *workLoop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *powerOffTimer = nullptr;
    SwitchAudioControl *control = nullptr;
    IOBufferMemoryDescriptor *output = nullptr;
    switch_dma_buffer commands = {}, tables = {};
    TegraSMMUDomain domain;
    uint32_t codec = 0, converter = 0, pin = 0, afg = 0;
    uint32_t stream = 0, streamBit = 0, lastPeriod = 0, stripe = 0;
#if DEVELOPMENT || DEBUG
    uint32_t periodsRun = 0;
#endif
    uint32_t corbMask = 0, rirbMask = 0, corbWrite = 0, rirbRead = 0;
    uint32_t response = 0;
    uint32_t stateGen = 0;
    uint32_t waitToken = 0;
    uint64_t hostHz = 0;
    bool pending = false, responseValid = false, ready = false, attached = false, mapped = false, prepared = false,
         powered = false, starting = false, powerOffPending = false;
    bool ringEmpty = true;
    uint32_t read(uint32_t offset) const;
    void write(uint32_t offset, uint32_t value);
    uint32_t core(uint32_t offset) const;
    void coreWrite(uint32_t offset, uint32_t value);
    void field(uint32_t offset, uint32_t mask, uint32_t value);
    bool prepareHardware();
    bool stopCommands();
    void finishPowerOff();
    void power(bool on);
    bool waitCoreBit(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs, uint32_t gen);
    bool sleepOnGateUs(uint32_t us, uint32_t gen);
    bool reset();
    bool verb(uint32_t node, uint32_t operation, uint32_t payload, uint32_t *value = nullptr);
    bool discover(uint32_t formats);
    bool programFormat(uint16_t format);
    void setClock(uint32_t rate);

public:
    bool initialize(IOWorkLoop *loop, IOCommandGate *commandGate, IOTimerEventSource *offTimer,
                    SwitchAudioControl *page, uint64_t hostHz);
    bool prepare(IOBufferMemoryDescriptor *ring);
    void release();
    void interrupt();
    void powerOffTimerFired();
    void disconnect();
    bool connect(uint32_t formats);
    IOReturn start();
    IOReturn setRate(uint32_t rate);
    bool stop();
    void sleep(bool entering);
    static constexpr uint32_t kIntid = 32 + 81;
};
