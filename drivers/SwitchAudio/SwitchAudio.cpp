// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: kernel half of the Switch's iPadOS audio service (SwitchAudioAbi.h).
// Drives the Tegra210 APE pipeline (ADMAIF1 -> MIXER1 -> SFC1 -> MVC1 -> OPE1 -> I2S1 -> RT5639; capture
// I2S1 -> [SFC2] -> ADMAIF1) with runtime APE power-gating, or a timer-driven null backend under switch-no-audio.
#include "SwitchAudioDriver.h"
#include "../SwitchDisplayPort/SwitchDisplayPort.h"

using namespace switch_audio_hw;

namespace {
uint64_t timerHz()
{
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    return 1000000000ULL * tb.denom / tb.numer;
}

void removeSources(IOWorkLoop *workLoop, IOEventSource *const *sources, size_t count)
{
    if (!workLoop)
        return;
    for (size_t i = 0; i < count; ++i) {
        if (sources[i])
            workLoop->removeEventSource(sources[i]);
    }
}
}  // namespace

// CoreAudio reads a 24 MHz host-clock constant from a read-only dyld shared cache page; rewrite it once to this
// board's timer frequency (19.2 MHz) and keep the page wired in hostClockWire so VM reclaim cannot evict and reset it.
IOReturn SwitchAudio::setHostClock(task_t task, uint64_t address)
{
    constexpr uint64_t kApple = 0x4176e36000000000ULL;  // 24000000.0
    if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL)
        return kIOReturnBadArgument;
    IOMemoryDescriptor *wire = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!wire)
        return kIOReturnNoMemory;
    IOReturn result = wire->prepare();
    if (result != kIOReturnSuccess) {
        wire->release();
        return result;
    }
    uint64_t seen = 0;
    const uint64_t pa = copyin(address, &seen, sizeof(seen)) ? 0 : pmap_find_pa(get_task_pmap(task), address);
    const unsigned top = 63 - static_cast<unsigned>(__builtin_clzll(hz));
    const uint64_t ours = static_cast<uint64_t>(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
    result = pa ? kIOReturnUnsupported : kIOReturnNotReadable;
    if (pa) {
        volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
        if (*value == seen && (seen == kApple || seen == ours)) {
            *value = ours;
            __asm__ volatile("dsb sy" ::: "memory");
            result = kIOReturnSuccess;
        }
    }
    if (result == kIOReturnSuccess) {
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): ownership transfers to hostClockWire (released on replace/free)
        IOMemoryDescriptor *prev = __atomic_exchange_n(&hostClockWire, wire, __ATOMIC_ACQ_REL);
        if (prev) {
            prev->complete();
            prev->release();
        }
        return kIOReturnSuccess;
    }
    wire->complete();
    wire->release();
    return result;
}

IOReturn SwitchAudio::sleepGated(OSObject *owner, void *entering, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const HwTransitionGuard guard(self);
    if (entering) {
        if (self->control->state || self->ahubDraining) {
            self->ahubDraining = false;
            self->tegraStop();
        }
        self->timer->cancelTimeout();
        return kIOReturnSuccess;
    }
    if (self->hardwareLost)
        return kIOReturnSuccess;
    if ((self->control->state || !self->codecOff) && !self->acquireApe()) {
        IOLog("SwitchAudio: APE partition did not power on after sleep (PMC PWRGATE_STATUS %#x CLAMP %#x); audio off\n",
              switch_pmc_read(PWRGATE_STATUS), switch_pmc_read(CLAMP_STATUS));
        self->hardwareLost = true;
        return kIOReturnSuccess;
    }
    if (self->control->state)
        self->tegraStart();
    if (self->control->state & SwitchAudioInputRunning)
        self->captureStart();
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::shutdownGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const HwTransitionGuard guard(self);
    if (self->control->state || self->ahubDraining) {
        self->ahubDraining = false;
        self->tegraStop();
    }
    self->hardwareLost = true;
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart)
        static_cast<SwitchAudio *>(target)->systemShutdown();
    return kIOReturnSuccess;
}

void SwitchAudio::systemShutdown()
{
    if (displayGate)
        displayGate->runAction(&SwitchAudio::displaySleepGated, this);
    if (!tegra)
        return;
    codecStep(kCodecMute);
    gate->runAction(&SwitchAudio::shutdownGated);
    codecGate->runAction(&SwitchAudio::codecShutdownGated);
}

void SwitchAudio::systemSleep(bool entering)
{
    if (displayGate)
        displayGate->runAction(&SwitchAudio::displaySleepGated, entering ? this : nullptr);
    if (!tegra)
        return;
    if (entering) {
        codecStep(kCodecMute);
        gate->runAction(&SwitchAudio::sleepGated, reinterpret_cast<void *>(1));
        codecGate->runAction(&SwitchAudio::codecSleepGated, reinterpret_cast<void *>(1));
        apeGate.suspend();
    } else {
        apeGate.resume();
        gate->runAction(&SwitchAudio::sleepGated, nullptr);
        codecGate->runAction(&SwitchAudio::codecSleepGated, nullptr);
    }
}

bool SwitchAudio::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    uint32_t off = 0;
    tegra = PE_parse_boot_argn("switch-no-audio", &off, sizeof(off)) == 0 || off == 0u;
    if (tegra && !SwitchRT5639::present())
        tegra = false;
    tegraCapture = tegra;
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                 static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                                 static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    const uint32_t sizes[3] = {static_cast<uint32_t>(PAGE_SIZE), kOutBytes, kInBytes};
    for (int i = 0; i < 3; ++i) {
        memory[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, sizes[i], kDmaMask);
        if (!memory[i])
            return false;
        bzero(memory[i]->getBytesNoCopy(), sizes[i]);
    }
    loop = IOWorkLoop::workLoop();
    gate = loop ? IOCommandGate::commandGate(this) : nullptr;
    if (!gate || loop->addEventSource(gate) != kIOReturnSuccess)
        return false;

    control = static_cast<SwitchAudioControl *>(memory[SwitchAudioMemoryControl]->getBytesNoCopy());
    control->magic = SWITCH_AUDIO_MAGIC;
    control->version = SWITCH_AUDIO_VERSION;
    control->rate = clientRate;
    control->out_channels = SWITCH_AUDIO_OUT_CHANNELS;
    control->in_channels = SWITCH_AUDIO_IN_CHANNELS;
    control->ring_frames = SWITCH_AUDIO_RING_FRAMES;
    control->lead_frames = SWITCH_AUDIO_LEAD_FRAMES;
    hz = timerHz();
    if (tegra) {
        // Attach the interrupt source before the watchdog timer so pending period interrupts are served first.
        tegraInit();
        codecStart();
        control->period_frames = kInPeriodFrames;
        control->backend = SwitchAudioBackendTegraI2S;
        for (uint32_t i = 0; i < SWITCH_AUDIO_MAX_RATES; ++i)
            control->rates[i] = kSupportedRates[i];
        noteClock();
        timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::watchdogFired);
    } else {
        control->period_frames = SWITCH_AUDIO_PERIOD_FRAMES;
        control->backend = SwitchAudioBackendNull;
        control->rates[0] = SWITCH_AUDIO_RATE;
        noteClock();
        timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::tick);
    }
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess)
        return false;
    // registerPrioritySleepWakeInterest dereferences the root domain unchecked.
    shutdownNotifier =
        getPMRootDomain() ? registerPrioritySleepWakeInterest(&SwitchAudio::powerMessage, this) : nullptr;
    if (!shutdownNotifier)
        IOLog("SwitchAudio: no shut down notification; the codec loses power with the PMIC\n");
    control->anchor_host = mach_absolute_time();
    if (!displayInit())
        return false;

    setProperty("NativeSandboxGrantProcess", "audiomxd");
    setProperty("IOUserClientClass", "SwitchAudioUserClient");
    setProperty("SwitchAudioBackend", tegra ? "tegra-i2s" : "null");
    registerService();
#if DEVELOPMENT || DEBUG
    registerSysctl();
#endif
    if (tegra) {
        const uint64_t milliHz = switch_audio_clock_millihz(38400000, *clock);
        IOLog("SwitchAudio: Tegra I2S1 + RT5639 ready (%s%s): S32_LE, I2S 32-bit slots at 64 fs (%u kHz BCLK), "
              "RT5639 %u-bit, %llu.%03llu Hz\n",
              tegraCapture ? "playback and capture" : "playback", speakerOnly ? ", speaker only" : "",
              static_cast<unsigned>((milliHz * static_cast<uint64_t>(SWITCH_AUDIO_BCLK_FS) + 500000) / 1000000),
              SWITCH_AUDIO_CODEC_BITS, milliHz / 1000, milliHz % 1000);
    }
    return true;
}

void SwitchAudio::free()
{
    displayFree();
    if (shutdownNotifier)
        shutdownNotifier->remove();
    if (timer)
        timer->cancelTimeout();
    if (cascade)
        cascade->disable();
    IOEventSource *const audioSources[] = {timer, cascade, gate};
    removeSources(loop, audioSources, sizeof(audioSources) / sizeof(audioSources[0]));

    if (jackSource)
        jackSource->disable();
    if (pv6Source)
        pv6Source->disable();
    if (codecEvent)
        codecEvent->disable();
    if (jackTimer)
        jackTimer->cancelTimeout();
    if (idleTimer)
        idleTimer->cancelTimeout();
    if (codecTimer)
        codecTimer->cancelTimeout();
    if (headsetTimer)
        headsetTimer->cancelTimeout();
    if (unmuteTimer)
        unmuteTimer->cancelTimeout();
    if (pauseTimer)
        pauseTimer->cancelTimeout();
#if SWITCH_STATS_IO
    if (statsTimer)
        statsTimer->cancelTimeout();
    IOEventSource *const codecSources[] = {jackSource,   pv6Source,   codecEvent, jackTimer,  idleTimer, codecTimer,
                                           headsetTimer, unmuteTimer, pauseTimer, statsTimer, codecGate};
#else
    IOEventSource *const codecSources[] = {jackSource, pv6Source,    codecEvent,  jackTimer,  idleTimer,
                                           codecTimer, headsetTimer, unmuteTimer, pauseTimer, codecGate};
#endif
    removeSources(codecLoop, codecSources, sizeof(codecSources) / sizeof(codecSources[0]));

    if (IOMemoryDescriptor *wire = __atomic_exchange_n(&hostClockWire, nullptr, __ATOMIC_ACQ_REL)) {
        wire->complete();
        wire->release();
    }
    OSSafeReleaseNULL(timer);
#if SWITCH_STATS_IO
    OSSafeReleaseNULL(statsTimer);
#endif
    OSSafeReleaseNULL(cascade);
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(loop);
    OSSafeReleaseNULL(jackSource);
    OSSafeReleaseNULL(pv6Source);
    OSSafeReleaseNULL(codecEvent);
    OSSafeReleaseNULL(jackTimer);
    OSSafeReleaseNULL(idleTimer);
    OSSafeReleaseNULL(codecTimer);
    OSSafeReleaseNULL(headsetTimer);
    OSSafeReleaseNULL(unmuteTimer);
    OSSafeReleaseNULL(pauseTimer);
    OSSafeReleaseNULL(codecGate);
    OSSafeReleaseNULL(codecLoop);
    for (auto &m : memory)
        OSSafeReleaseNULL(m);
    IOService::free();
}

IOMemoryDescriptor *SwitchAudio::copyMemory(uint32_t type)
{
    if (type > SwitchAudioMemoryDisplayOutput || !memory[type])
        return nullptr;
    memory[type]->retain();
    return memory[type];
}

OSDefineMetaClassAndStructors(SwitchAudio, IOService)

    // NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
    class SwitchAudioUserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchAudioUserClient) task_t owner = nullptr;
    SwitchAudio *audio = nullptr;

public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
    {
        if (!IOUserClient::initWithTask(task, security, type, properties))
            return false;
        owner = task;
        return true;
    }

    bool start(IOService *provider) override
    {
        audio = OSDynamicCast(SwitchAudio, provider);
        return (audio != nullptr) && IOUserClient::start(provider);
    }

    IOReturn clientClose() override
    {
        audio->request(SwitchAudioStopOutput);
        audio->request(SwitchAudioStopInput);
        audio->displayRequest(false);
        terminate();
        return kIOReturnSuccess;
    }

    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override
    {
        IOMemoryDescriptor *m = audio->copyMemory(type);
        if (!m)
            return kIOReturnBadArgument;
        *options = 0;
        *memory = m;
        return kIOReturnSuccess;
    }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override
    {
        if (args->structureInputSize || args->structureOutputSize || args->scalarOutputCount)
            return kIOReturnBadArgument;
        switch (selector) {
        case SwitchAudioStartOutput:
        case SwitchAudioStopOutput:
        case SwitchAudioStartInput:
        case SwitchAudioStopInput:
            return args->scalarInputCount == 0 ? audio->request(selector) : kIOReturnBadArgument;
        case SwitchAudioSetHostClock:
            return args->scalarInputCount == 1 ? audio->setHostClock(owner, args->scalarInput[0])
                                               : kIOReturnBadArgument;
        case SwitchAudioSetRate:
            return args->scalarInputCount == 1 && args->scalarInput[0] <= UINT32_MAX
                       ? audio->setRate(static_cast<uint32_t>(args->scalarInput[0]))
                       : kIOReturnBadArgument;
        case SwitchAudioSetHeadphoneLevel:
            return args->scalarInputCount == 1 ? audio->setHeadphoneLevel(args->scalarInput[0]) : kIOReturnBadArgument;
        case SwitchAudioSetSpeakerLevel:
            return args->scalarInputCount == 1 ? audio->setSpeakerLevel(args->scalarInput[0]) : kIOReturnBadArgument;
        case SwitchAudioSetMute:
            return args->scalarInputCount == 1 ? audio->setMute(args->scalarInput[0] != 0) : kIOReturnBadArgument;
        case SwitchAudioSetRoute:
            return args->scalarInputCount == 1 && args->scalarInput[0] <= UINT32_MAX
                       ? audio->setRoute(static_cast<uint32_t>(args->scalarInput[0]))
                       : kIOReturnBadArgument;
        case SwitchAudioStartDisplayOutput:
        case SwitchAudioStopDisplayOutput:
            return args->scalarInputCount == 0 ? audio->displayRequest(selector == SwitchAudioStartDisplayOutput)
                                               : kIOReturnBadArgument;
        case SwitchAudioSetDisplayRate:
            return args->scalarInputCount == 1 && args->scalarInput[0] <= UINT32_MAX
                       ? audio->displaySetRate(static_cast<uint32_t>(args->scalarInput[0]))
                       : kIOReturnBadArgument;
        default:
            return kIOReturnBadArgument;
        }
    }
};

OSDefineMetaClassAndStructors(SwitchAudioUserClient, IOUserClient)

    static SwitchAudio *gAudio;
static SwitchAudioSink gDisplaySink;

bool SwitchAudio::waitMmioActive(const volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value,
                                 uint32_t timeoutUs)
{
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        return audio->waitMmio(base, offset, mask, value, timeoutUs);
    constexpr uint32_t kFastUs = 50u;
    const uint32_t fastUs = timeoutUs < kFastUs ? timeoutUs : kFastUs;
    if (switch_mmio_wait32(base, offset, mask, value, fastUs))
        return true;
    if (timeoutUs <= kFastUs)
        return false;
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeoutUs - kFastUs, NSEC_PER_USEC, &deadline);
    while (mach_absolute_time() < deadline) {
        delay_for_interval(250u, NSEC_PER_USEC);
        if ((switch_mmio_read32(base, offset) & mask) == value)
            return true;
    }
    return (switch_mmio_read32(base, offset) & mask) == value;
}

SwitchAudioSink switch_audio_display_sink()
{
    SwitchAudioSink sink;
    __atomic_load(&gDisplaySink, &sink, __ATOMIC_ACQUIRE);
    return sink;
}

extern "C" void switch_displayport_audio_sink(const uint8_t *edid, unsigned valid_blocks, bool connected)
{
    const uint32_t formats = connected ? switch_hda_sink_formats(edid, valid_blocks) : 0;
    SwitchAudioSink sink = switch_audio_display_sink();
    if (!formats && !sink.formats)
        return;
    sink.formats = formats;
    ++sink.generation;
    __atomic_store(&gDisplaySink, &sink, __ATOMIC_RELEASE);
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        audio->displaySink();
}

extern "C" bool switch_displayport_audio_eld(const uint8_t *edid, unsigned valid_blocks)
{
    uint8_t eld[128];
    const uint32_t bytes = switch_hda_eld(edid, valid_blocks, eld);
    switch_displayport_audio_presence(eld, bytes, bytes != 0);
    return bytes != 0;
}

extern "C" int switch_audio_publish(void)
{
    static bool published;
    if (published)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchAudio *audio = OSTypeAlloc(SwitchAudio);
    if (!resources || !audio || !audio->init() || !audio->attach(resources)) {
        OSSafeReleaseNULL(audio);
        IOLog("SwitchAudio: publish failed\n");
        return -1;
    }
    if (!audio->start(resources)) {
        audio->detach(resources);
        audio->release();
        IOLog("SwitchAudio: start failed\n");
        return -1;
    }
    __atomic_store_n(&gAudio, audio, __ATOMIC_RELEASE);
    if (switch_audio_display_sink().formats)
        audio->displayStartProbe();
    audio->release();
    published = true;
    IOLog("SwitchAudio: started\n");
    return 0;
}

extern "C" void switch_audio_suspend(void)
{
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        audio->systemSleep(true);
}

extern "C" void switch_audio_resume(void)
{
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        audio->systemSleep(false);
}
