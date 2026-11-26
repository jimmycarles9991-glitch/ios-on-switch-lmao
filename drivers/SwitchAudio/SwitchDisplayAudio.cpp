// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioDriver.h"
#include "../SwitchDisplayPort/SwitchDisplayPort.h"
#include <kern/clock.h>
#include <kern/thread.h>

using namespace switch_audio_hw;

bool SwitchAudio::displayInit()
{
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                 static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                                 static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    memory[SwitchAudioMemoryDisplayControl] =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, PAGE_SIZE, kDmaMask);
    if (!memory[SwitchAudioMemoryDisplayControl])
        return false;
    bzero(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy(), PAGE_SIZE);
    auto *page = static_cast<SwitchAudioControl *>(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy());
    if (!displayAudio.initialize(gate, page, hz))
        return false;
    IOService *nub = switch_interrupt_nub(SwitchHDA::kIntid, false);
    displayInterrupt = IOInterruptEventSource::interruptEventSource(this, displayFired, nub, 0);
    if (!displayInterrupt || loop->addEventSource(displayInterrupt) != kIOReturnSuccess)
        panic("SwitchAudio: cannot attach HDA interrupt, INTID %u", SwitchHDA::kIntid);
    displayEvent = IOInterruptEventSource::interruptEventSource(this, displayChanged);
    if (!displayEvent || loop->addEventSource(displayEvent) != kIOReturnSuccess)
        return false;
    displayEvent->enable();
    displayCall =
        thread_call_allocate_with_options(displayProbe, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return displayCall != nullptr;
}

void SwitchAudio::displayFree()
{
    if (displayEvent) {
        displayEvent->disable();
        if (loop)
            loop->removeEventSource(displayEvent);
        OSSafeReleaseNULL(displayEvent);
    }
    if (displayCall) {
        thread_call_cancel_wait(displayCall);
        thread_call_free(displayCall);
        displayCall = nullptr;
    }
    if (displayInterrupt) {
        displayInterrupt->disable();
        if (loop)
            loop->removeEventSource(displayInterrupt);
        OSSafeReleaseNULL(displayInterrupt);
    }
    displayAudio.release();
}

void SwitchAudio::displayFired(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchAudio *>(owner)->displayAudio.interrupt();
}

void SwitchAudio::displayProbe(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    SwitchAudioSink sink;
    const bool prepared = self->gate->runAction(displayPrepareGated, &sink) == kIOReturnSuccess;
    const bool present = prepared && switch_displayport_audio_prepare_presence();
    self->gate->runAction(displayConnectGated, present ? &sink : nullptr);
}

IOReturn SwitchAudio::displayPrepareGated(OSObject *owner, void *capture, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    auto &sink = *static_cast<SwitchAudioSink *>(capture);
    sink = switch_audio_display_sink();
    if (!sink.formats || self->displayAsleep)
        return kIOReturnNotReady;
    auto *&output = self->memory[SwitchAudioMemoryDisplayOutput];
    if (!output) {
        const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                     static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                                     static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
        output = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, kOutBytes, kDmaMask);
        if (!output)
            return kIOReturnNoMemory;
        bzero(output->getBytesNoCopy(), kOutBytes);
        switch_dcache_clean(output->getBytesNoCopy(), kOutBytes);
    }
    self->displayInterrupt->enable();
    self->displayPresencePending = self->displayAudio.prepare(output);
    return self->displayPresencePending ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn SwitchAudio::displayConnectGated(OSObject *owner, void *present, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    self->displayPresencePending = false;
    const auto *sink = static_cast<const SwitchAudioSink *>(present);
    if (!sink || self->displayAsleep || sink->generation != switch_audio_display_sink().generation ||
        !self->displayAudio.connect(sink->formats) ||
        sink->generation != switch_audio_display_sink().generation) {
        self->displayAudio.sleep(true);
        self->displayInterrupt->disable();
    } else {
        auto *page = static_cast<SwitchAudioControl *>(self->memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy());
        __atomic_store_n(&page->jack, SwitchAudioJackHeadphones, __ATOMIC_RELEASE);
    }
    self->gate->commandWakeup(&self->displayPresencePending, false);
    self->messageClients(kIOMessageServicePropertyChange);
    return kIOReturnSuccess;
}

void SwitchAudio::displayChanged(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const bool probe = switch_audio_display_sink().formats && self->displayCall && !self->displayAsleep;
    if (!probe && !self->displayPresencePending)
        self->displayAudio.sleep(true);
    else
        self->displayAudio.disconnect();
    self->displayInterrupt->disable();
    self->messageClients(kIOMessageServicePropertyChange);
    if (probe)
        self->displayStartProbe();
}

void SwitchAudio::displaySink()
{
    displayEvent->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchAudio::displayStartProbe()
{
    thread_call_enter(displayCall);
}

IOReturn SwitchAudio::displayRequestGated(OSObject *owner, void *start, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (start)
        return self->displayAudio.start();
    self->displayAudio.stop();
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::displayRequest(bool start)
{
    const auto *page =
        static_cast<const SwitchAudioControl *>(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy());
    if (start && (!page->jack || !switch_displayport_audio_format(page->rate, 2, page->sample_bits)))
        return kIOReturnNotReady;
    const IOReturn result = gate->runAction(displayRequestGated, start ? this : nullptr);
    switch_displayport_audio_enable(start && result == kIOReturnSuccess);
    return result;
}

IOReturn SwitchAudio::displaySetRate(uint32_t rate)
{
    return gate->runAction(
        [](OSObject *owner, void *value, void *, void *, void *) -> IOReturn {
            return static_cast<SwitchAudio *>(owner)->displayAudio.setRate(
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
        },
        reinterpret_cast<void *>(static_cast<uintptr_t>(rate)));
}

IOReturn SwitchAudio::displaySleepGated(OSObject *owner, void *entering, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    self->displayAsleep = entering != nullptr;
    if (self->displayAsleep) {
        self->displayAudio.disconnect();
        uint64_t deadline = 0;
        clock_interval_to_deadline(200, kMillisecondScale, &deadline);
        // SwitchSuspend and PM callers wait here; the worker finishes DP with the Audio gate open.
        while (self->displayPresencePending) {
            if (self->gate->commandSleep(&self->displayPresencePending, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
                IOLog("SwitchAudio: timed out waiting for display presence before sleep\n");
                break;
            }
        }
        self->displayAudio.sleep(true);
    } else {
        self->displayAudio.sleep(false);
    }
    if (self->displayInterrupt)
        self->displayInterrupt->disable();
    if (!self->displayAsleep && switch_audio_display_sink().formats && self->displayCall)
        self->displayStartProbe();
    self->messageClients(kIOMessageServicePropertyChange);
    return kIOReturnSuccess;
}
