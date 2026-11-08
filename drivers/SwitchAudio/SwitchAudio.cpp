// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the kernel half of the Switch's iPadOS audio device. iPadOS 17's audio server (audiomxd) loads our
// AudioServerPlugIn (hal/SwitchAudioPlugIn.c); the plug-in opens this service, maps its control page and two sample
// rings (SwitchAudioAbi.h) and asks it to start and stop each direction. Design after UniXNU-sheng's
// package/ShengAudioService.cpp, the iPadOS 17.7.11 audio path that is known to play on real hardware.
//
// Backends:
//   Tegra I2S playback (speaker and headphone jack) and capture (the headset microphone on the jack). The jack's state,
//      headphones and whether the plug has a microphone, is in the control page for the plug-in's Codec device. A
//      capture channel that delivers nothing in its first periods is turned off for the boot with one line (it had
//      never run on hardware before 11-07); the microphone then reads as absent.
//   null, with boot-arg switch-no-audio: a 10 ms timer advances the play position on the host clock and clears what it
//      passed; capture is silence; no Tegra hardware is touched. The timer runs only while a stream does.
// The Tegra backend logs one line every 5 s while streams run: streams started, frames played, underruns, the loudest sample, the
// DMA, I2S and jack state and the codec's power and volume registers. The register fields are read at the moment of the
// line: between streams the I2S is off (en 0) and, 5 s after the last one, the codec's power registers are 0.
// The Tegra backend is interrupt driven throughout: the ADMA period interrupt moves the positions, the codec's IRQ
// line reports the jack, and an interrupt that does not arrive, a DMA that stalls or runs fast, or SoC hardware that
// does not answer, panics with one line. An RT5639 that does not answer is one line: absent at boot, the null backend
// (as switch-no-audio); lost later, off for the boot while the I2S plays on.
//
// The OLED's audio path. Sources: the shipped DTB (oled-sku3.dtb in bl33.bin = CTCaer
// switch-l4t-platform-t210-nx tegra210b01-fric.dts over tegra210b01-nx-common.dtsi:864-950), switchroot Android
// android_device_nintendo_nx media/nvaudio_conf.xml (the mixer paths), CTCaer switch-l4t-kernel-nvidia
// sound/soc/codecs/rt5640.c and sound/soc/tegra-alt (tegra_machine_driver_mobile.c, tegra_asoc_utils_alt.c,
// tegra210_i2s_alt.c, tegra210-adma.c), CTCaer switch-l4t-kernel-4.9 drivers/clk/tegra, mainline sound/soc/tegra
// (tegra210_ahub.c, tegra210_admaif.c, tegra210_i2s.c) and pinctrl-tegra210.c.
//  - Codec: Realtek RT5639 (rt5640 family, VENDOR_ID2 0x6231) on I2C1 at 0x1c. Its own class-D amplifier drives the
//    speakers (SPOLP/N, SPORP/N; spkvdd = vdd-ac-bat) and its HP amplifier the jack (HPOL/R); there is no external
//    amplifier. The only microphone is a headset's, on IN1P (single ended). LDO1_EN = PZ4 (high = on, then 400 ms);
//    the codec's GPIO1 is its IRQ output, on PBB4; jack detect is the codec's own (realtek,sel-jd-source 4 = its
//    GPIO2, jack-detect-is-jd1, inverted: JD_STATUS 0 = plugged). The same headphone-detect net reaches Tegra PV6
//    (the DT's jack-detect-gpios, active low; tegra210b01-fric-pinmux.dtsi touch_rst_pv6 "Audio Codec (ALC5639)
//    Headphone Detect": rsvd0, no pull, tristate, input). rt5640.c never reads PV6, but its pad is on the net, so it is
//    muxed as L4T leaves it before the codec is read; its level only goes into the diagnostics here.
//    Microphone vs plain headphones: MICBIAS1 over-current at 600 uA.
//  - Link: Tegra I2S1 (AHUB 0x702d1000; CAR clock "i2s0") is bit and frame clock master, the codec slave
//    (dai-link-1: format i2s, no bitclock-master), stereo, 48 kHz. Samples are S32_LE from memory to the I2S
//    (SwitchAudioFormat.h): 32-bit slots, BCLK 64 fs, and the codec reads 24 bits of each (I2S1 data length 24, BCLK
//    64 fs), as rt5640_hw_params sets it for 24-bit samples in a 64-bit frame. (The DTB's dai-link says s16_le, BCLK
//    32 fs, as L4T runs it.) MCLK 256 fs = 12.288 MHz (mclk-fs 256) on AUD_MCLK = PMC
//    clk_out_1 <- extern1 <- PLLA_OUT0 (clk-tegra210.c init table); the codec runs from MCLK directly (sysclk source
//    MCLK, pre-divider 1). Pins DAP1_FS/DIN/DOUT/SCLK (function i2s1), AUD_MCLK.
//  - Clocks for 48 kHz (tegra_asoc_utils_alt.c): PLLA 368.64 MHz (T210B01 table, clk-tegra210b01.c:1789-1793:
//    38.4 MHz ref, m 2, n 38, p 2, SDM 0xfccc, really 368639062.5 Hz), PLLA_OUT0 49.152 MHz, AHUB (d_audio)
//    = PLLA_OUT0, I2S 3.072 MHz (64 fs), extern1 12.288 MHz, APE from PLLP at 25.5 MHz. The real frame rate is
//    therefore 47999.878 Hz, and ticks_per_frame_q32 says so.
//  - The 44.1 kHz family (SwitchAudioFormat.h's clock table, chosen by the plug-in through SwitchAudioSetRate between
//    streams): PLLA 282.24 MHz (m 2, n 29, p 2, SDM 0xfccc, the T210B01 table's other entry), PLLA_OUT0 / 5 = 56.448
//    MHz (1280 fs), I2S / 20, extern1 / 5, so 44099.854 Hz. The codec needs nothing new: MCLK stays 256 fs.
//  - Routing: XBAR "I2S1 Mux" = ADMAIF1 (playback), "ADMAIF1 Mux" = I2S1 (capture), as nvaudio_conf.xml sets them;
//    ADMA request line 1 = ADMAIF1 (DTB admaif dmas). ADSP and the AHUB effect blocks are not used.
//  - Interrupts: ADMA channel n raises SPI 24+n on the AGIC, the APE's own GICv2 (distributor 0x702f9000, CPU
//    interface 0x702fa000; DTB adma@702e2000 interrupts), whose host output is main-GIC SPI 102 = INTID 134 (DTB
//    agic@702f9000 interrupts <0 0x66 ..>). This driver claims switch_interrupt_nub(134, false) and runs the
//    cascade: the channel's AGIC SPI targeted at the interface the CPU reads in its banked ITARGETSR0, IAR/EOIR on the
//    AGIC, ADMA_CH_INT_STATUS/CLEAR. Channels 0 (playback) and 1 (capture) are host channels.
//
// The ADMA plays the playback ring and fills the capture ring in place: one cycle of 8 periods (the most an ADMA
// cycle holds) is exactly one ring, so a period is 2048 frames (42.7 ms, 16 KiB of S32 stereo). Each period interrupt
// clears the playback period just consumed (so the next turn mixes onto silence) and invalidates the capture period
// just written, then moves out_frames/in_frames on. The ADMA does not snoop the caches: the plug-in cleans what it
// mixes, this driver cleans what it clears and invalidates what the ADMA wrote. Sample time 0 is the host time the
// playback DMA started.
//
// Started by switch_audio_publish() from the board thread; the classes register through the normal static
// initializer (they derive only from XNU's own IOService and IOUserClient).
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

namespace {
uint64_t timerHz()
{
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    return 1000000000ULL * tb.denom / tb.numer;
}

// CoreAudio assumes a 24 MHz host clock (Apple's timer): AudioGetHostClockFrequency and the audio server's IO thread
// read one double constant in the dyld shared cache. This board's timer runs at 19.2 MHz (ADT /cpus/cpu0
// timebase-frequency), so without this the audio server's IO cycles run 25 % long (UniXNU-sheng, sheng.md). The
// constant sits on a read-only shared page, so the caller cannot write it; this rewrites that physical page once for
// every process. Only a value of exactly 24 MHz (or ours already) at an address inside the shared region is touched.
IOReturn setHostClock(task_t task, uint64_t address)
{
    constexpr uint64_t kApple = 0x4176e36000000000ULL;  // 24000000.0
    if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL)
        return kIOReturnBadArgument;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!memory)
        return kIOReturnNoMemory;
    IOReturn result = memory->prepare();  // wires the page while we write it
    if (result != kIOReturnSuccess) {
        memory->release();
        return result;
    }
    uint64_t seen = 0;
    const uint64_t pa = copyin(address, &seen, sizeof(seen)) ? 0 : pmap_find_pa(get_task_pmap(task), address);
    // The timer frequency as an IEEE double, from the integer (no floating point in the kernel).
    const uint64_t hz = timerHz();
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
    memory->complete();
    memory->release();
    return result;
}
}  // namespace

IOReturn SwitchAudio::sleepGated(OSObject *owner, void *entering, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (entering) {
        if (self->control->state)
            self->tegraStop();  // iOS holds sleep off while audio plays; nothing runs here
        self->timer->cancelTimeout();
        self->cascade->disable();
        return kIOReturnSuccess;
    }
    if (!self->apePartitionOn()) {
        IOLog("SwitchAudio: APE partition did not power on after sleep (PMC PWRGATE_STATUS %#x CLAMP %#x); audio "
              "off\n",
              switch_mmio_read32(self->pmc, PWRGATE_STATUS), switch_mmio_read32(self->pmc, CLAMP_STATUS));
        self->hardwareLost = true;
        return kIOReturnSuccess;
    }
    self->tegraClocksAndPads();
    self->tegraAhubAndDma();
    self->tegraAgic();
    self->cascade->enable();
    if (self->control->state)
        self->tegraStart();
    if (self->control->state & SwitchAudioInputRunning)
        self->captureStart();
    return kIOReturnSuccess;
}

// Shut Down and Restart, before the PMIC cuts the rails: the hardware stopped and nothing touched again.
IOReturn SwitchAudio::shutdownGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->control->state)
        self->tegraStop();
    self->hardwareLost = true;
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart)
        static_cast<SwitchAudio *>(target)->systemShutdown();
    return kIOReturnSuccess;
}

// As at sleep, then the codec reset and its LDO1_EN low: the datasheet's power-off order, ahead of its rails.
void SwitchAudio::systemShutdown()
{
    codecStep(kCodecMute);
    gate->runAction(&SwitchAudio::shutdownGated);
    codecGate->runAction(&SwitchAudio::codecShutdownGated);
}

// System sleep (switch_audio_suspend/resume, SwitchSuspend's thread), in ASoC's suspend order: the outputs muted
// while the I2S still clocks, then the hardware stopped, then the codec powered down. On wake the hardware first,
// then the codec, unmuted only if a stream runs. Null backend: nothing to do.
void SwitchAudio::systemSleep(bool entering)
{
    if (!tegra)
        return;
    if (entering) {
        codecStep(kCodecMute);
        gate->runAction(&SwitchAudio::sleepGated, reinterpret_cast<void *>(1));
        codecGate->runAction(&SwitchAudio::codecSleepGated, reinterpret_cast<void *>(1));
    } else {
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
    control->rate = clock->rate;
    control->out_channels = SWITCH_AUDIO_OUT_CHANNELS;
    control->in_channels = SWITCH_AUDIO_IN_CHANNELS;
    control->ring_frames = SWITCH_AUDIO_RING_FRAMES;
    // The plug-in writes at least this far ahead of now: past one null tick, or past the ADMA's prefetch.
    control->lead_frames = 2 * SWITCH_AUDIO_PERIOD_FRAMES;
    hz = timerHz();
    if (tegra) {
        // The interrupt source goes on the loop before the deadline timer, so a pending interrupt is served first.
        tegraInit();
        codecStart();
        control->period_frames = kInPeriodFrames;
        control->backend = SwitchAudioBackendTegraI2S;
        for (uint32_t i = 0; i < SWITCH_AUDIO_MAX_RATES; ++i)
            control->rates[i] = kClocks[i].rate;
        noteClock();
        timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::watchdogFired);
    } else {
        control->period_frames = SWITCH_AUDIO_PERIOD_FRAMES;
        control->backend = SwitchAudioBackendNull;
        control->rates[0] = SWITCH_AUDIO_RATE;
        control->ticks_per_frame_q32 = (hz << 32) / SWITCH_AUDIO_RATE;
        timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::tick);
    }
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess)
        return false;
    if (tegra) {
        // registerPrioritySleepWakeInterest dereferences the root domain unchecked.
        shutdownNotifier =
            getPMRootDomain() ? registerPrioritySleepWakeInterest(&SwitchAudio::powerMessage, this) : nullptr;
        if (!shutdownNotifier)
            IOLog("SwitchAudio: no shut down notification; the codec loses power with the PMIC\n");
    }
    control->anchor_host = mach_absolute_time();

    // audiomxd's sandbox names no class of ours; the kernel's NativeSandboxGrantProcess check (IOUserClient.cpp)
    // lets exactly this process open the service. Our check compares the whole string to one process name.
    setProperty("NativeSandboxGrantProcess", "audiomxd");
    setProperty("IOUserClientClass", "SwitchAudioUserClient");
    setProperty("SwitchAudioBackend", tegra ? "tegra-i2s" : "null");
    registerService();
    if (tegra) {
        // The format and rate, once: S32_LE rings and APE path, 32-bit I2S slots at 64 fs, the codec's 24 bits.
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
    if (shutdownNotifier)
        shutdownNotifier->remove();
    if (timer) {
        timer->cancelTimeout();
        if (loop)
            loop->removeEventSource(timer);
    }
    if (cascade && loop) {
        cascade->disable();
        loop->removeEventSource(cascade);
    }
    if (gate && loop)
        loop->removeEventSource(gate);
    if (codecLoop) {
        if (jackSource) {
            jackSource->disable();
            codecLoop->removeEventSource(jackSource);
        }
#if DEVELOPMENT || DEBUG
        if (statsTimer)
            statsTimer->cancelTimeout();
        IOEventSource *sources[] = {jackTimer, idleTimer, statsTimer, codecGate};
#else
        IOEventSource *sources[] = {jackTimer, idleTimer, codecGate};
#endif
        for (IOEventSource *source : sources) {
            if (source)
                codecLoop->removeEventSource(source);
        }
    }
    OSSafeReleaseNULL(timer);
#if DEVELOPMENT || DEBUG
    OSSafeReleaseNULL(statsTimer);
#endif
    OSSafeReleaseNULL(cascade);
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(loop);
    OSSafeReleaseNULL(jackSource);
    OSSafeReleaseNULL(jackTimer);
    OSSafeReleaseNULL(idleTimer);
    OSSafeReleaseNULL(codecGate);
    OSSafeReleaseNULL(codecLoop);
    for (auto &m : memory)
        OSSafeReleaseNULL(m);
    IOService::free();
}

IOMemoryDescriptor *SwitchAudio::copyMemory(uint32_t type)
{
    if (type > SwitchAudioMemoryInput)
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
        terminate();
        return kIOReturnSuccess;
    }

    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override
    {
        IOMemoryDescriptor *m = audio->copyMemory(type);
        if (!m)
            return kIOReturnBadArgument;
        *options = 0;
        *memory = m;  // the caller consumes the reference
        return kIOReturnSuccess;
    }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override
    {
        if (args->structureInputSize || args->structureOutputSize || args->scalarOutputCount)
            return kIOReturnBadArgument;
        if (selector == SwitchAudioSetHostClock)
            return args->scalarInputCount == 1 ? setHostClock(owner, args->scalarInput[0]) : kIOReturnBadArgument;
        if (selector == SwitchAudioSetHeadphoneLevel)
            return args->scalarInputCount == 1 ? audio->setHeadphoneLevel(args->scalarInput[0]) : kIOReturnBadArgument;
        if (selector == SwitchAudioSetRate) {
            return args->scalarInputCount == 1 && args->scalarInput[0] <= UINT32_MAX
                       ? audio->setRate(static_cast<uint32_t>(args->scalarInput[0]))
                       : kIOReturnBadArgument;
        }
        if (selector > SwitchAudioStopInput || args->scalarInputCount)
            return kIOReturnBadArgument;
        return audio->request(selector);
    }
};

OSDefineMetaClassAndStructors(SwitchAudioUserClient, IOUserClient)

    // Publishes the service under IOResources. Call once from the board thread (the Tegra backend sleeps on the GIC, the
    // GPIO controller and I2C1), any point before launchd starts audiomxd; returns 0 on success.
    static SwitchAudio *gAudio;  // set once published (switch_audio_suspend/resume)

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
    gAudio = audio;
    audio->release();  // the attachment keeps it
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
