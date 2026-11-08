// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SwitchAudio service: its state and the members its files share. SwitchAudio.cpp has the design and the
// lifecycle, SwitchAudioClocks.cpp the APE bring-up and rates, SwitchAudioStream.cpp the DMA streams and their
// interrupts, SwitchAudioCodec.cpp the RT5639 and the jack.
#pragma once
#include "SwitchAudioRegisters.h"

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchAudio : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchAudio) IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;  // null: the play clock; Tegra: the period interrupt's deadline
    IOBufferMemoryDescriptor *memory[3] = {};
    SwitchAudioControl *control = nullptr;

    uint64_t hz = 0;
    const switch_audio_clock *clock = &switch_audio_hw::kClocks[0];  // what the I2S runs (or runs next)

    // Tegra backend: hardware on `loop`, the codec (I2C transfers sleep) on its own work loop.
    bool tegra = false, tegraCapture = false;
    volatile uint8_t *car = nullptr, *pmc = nullptr, *pinmux = nullptr, *ahub = nullptr, *adma = nullptr,
                     *agic = nullptr;
    uint32_t ringPA[3] = {};
    uint32_t jackPinFound = 0;  // PV6's pinmux as the boot chain left it (logged with the first jack reading)
    IOInterruptEventSource *cascade = nullptr;
    uint64_t periods[2] = {};   // periods done since the DMA started
    uint16_t xferSeen[2] = {};  // ADMA_CH_XFER_STATUS (completed-period counter) at the last interrupt
    bool graced = false;
    IOWorkLoop *codecLoop = nullptr;
    IOCommandGate *codecGate = nullptr;
    IOInterruptEventSource *jackSource = nullptr;
    IOTimerEventSource *jackTimer = nullptr, *idleTimer = nullptr;
    bool codecOn = false, jackKnown = false, jackIn = false, headset = false, speakerOnly = false;
    bool captureProven = false;      // a capture period has arrived since start
    bool captureRunning = false;     // ADMA ch1 and I2S TX run: input is requested (captureStart)
    bool captureCodecOn = false;     // the codec's ADC path and microphone bias are up (codec work loop)
    uint64_t captureStartFrame = 0;  // the sample time of capture ring frame 0
    unsigned headphoneSteps = 0;     // the plug-in's headphone level, in the DAC's 0.375 dB steps (setHeadphoneLevel)
    bool hardwareLost = false;       // the APE did not come back after sleep, or shut down: never touched again
    bool codecOff = false;           // shut down: the codec is unpowered
    IONotifier *shutdownNotifier = nullptr;
    SwitchRT5639 codec;
#if DEVELOPMENT || DEBUG
    // The stats: counted on the audio work loop, printed (and the peak reset) on the codec work loop.
    IOTimerEventSource *statsTimer = nullptr;
    uint64_t statFrames = 0, statFramesShown = 0, statIoCyclesShown = 0, statIoFramesShown = 0;
    uint32_t statStarts = 0, statStartsShown = 0, statUnderruns = 0;
    bool statsArmed = false, statsShown = false;  // statsTimer: armed / a line written (statsFired)
    int statPeak = 0;
    int64_t statLagMin = INT64_MAX, statLagMax = INT64_MIN, statLeadMin = INT64_MAX;
    uint64_t periodTicksQ16 =
        0;  // host ticks per playback period in 48.16 (kOutPeriodFrames * ticks_per_frame_q32 >> 16)
#endif

    uint64_t nowFrame() const
    {
        const uint64_t now = mach_absolute_time();
        if (now <= control->anchor_host)
            return 0;
        const uint64_t ticks = now - control->anchor_host;  // split so nothing overflows (no 128-bit helpers here)
        return ticks / hz * clock->rate + ticks % hz * clock->rate / hz;
    }

    switch_audio_sample_t *ring(uint32_t type) const
    {
        return static_cast<switch_audio_sample_t *>(memory[type]->getBytesNoCopy());
    }

    // Playback up to now is "played"; clear it so the next turn of the ring mixes onto silence.
    void advance()
    {
        const uint64_t now = nowFrame();
        if (control->state & SwitchAudioOutputRunning) {
            switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
            uint64_t from = control->out_frames;
            if (now > from + SWITCH_AUDIO_RING_FRAMES)
                from = now - SWITCH_AUDIO_RING_FRAMES;
            while (from < now) {
                const uint64_t at = from % SWITCH_AUDIO_RING_FRAMES;
                const uint64_t run =
                    now - from < SWITCH_AUDIO_RING_FRAMES - at ? now - from : SWITCH_AUDIO_RING_FRAMES - at;
                bzero(out + at * SWITCH_AUDIO_OUT_CHANNELS, run * SWITCH_AUDIO_OUT_CHANNELS * sizeof(*out));
                from += run;
            }
            control->out_frames = now;
        }
        if (control->state & SwitchAudioInputRunning)
            control->in_frames = now;  // the capture ring stays silent
    }

    static void tick(OSObject *owner, IOTimerEventSource *source);

    static volatile uint8_t *map(vm_offset_t pa, vm_size_t size, const char *what)
    {
        volatile uint8_t *base = switch_mmio_map(pa, size);
        if (!base)
            panic("SwitchAudio: cannot map %s at %#lx", what, (unsigned long)pa);
        return base;
    }

    // The rate the I2S really runs at, as host ticks per frame in 32.32 (47999.878 Hz for 48 kHz, 44099.854 Hz for
    // 44.1 kHz).
    uint64_t tegraTicksPerFrame(uint64_t refHz) const
    {
        return switch_audio_clock_ticks_per_frame_q32(hz, refHz, *clock);
    }

    uint32_t watchdogMS() const  // 4 periods: 170 ms at 48 kHz
    {
        return 4 * switch_audio_hw::kOutPeriodFrames * 1000 / clock->rate;
    }

    void startPlla();

    void startClocks();

    void tegraClocksAndPads();

    void tegraAhubAndDma();

    void tegraAgic();

    void tegraInit();

    void softReset(uint32_t reg, const char *what);

    void i2sSoftReset(uint32_t reg, const char *what);

    void stopUnit(uint32_t enable, uint32_t status, const char *what);

    void tegraStart();

    void programChannel(unsigned ch);

    void captureStart();

    void captureStop();

    void tegraStop();

    void playedPeriod();

#if DEVELOPMENT || DEBUG
    void notePlayed(const switch_audio_sample_t *slot, uint64_t done);
#endif

    void captureOff();

    void capturedPeriod();

    void channelInterrupt(unsigned ch);

    static void cascadeFired(OSObject *owner, IOInterruptEventSource *, int);

    static void watchdogFired(OSObject *owner, IOTimerEventSource *sender);

    rt5639::Route wantedRoute() const;

    void codecPowerUp();

    void captureCodec(bool on);

    void codecPowerDown();

    enum CodecStep : uintptr_t {
        kCodecPowerUp,
        kCodecUnmute,
        kCodecMute,
        kCodecPause,           // the last stream stopped: muted now, powered down after kIdleMS unless one starts again
        kCodecIdleNow,         // no stream: powered down now (before its MCLK changes), not kIdleMS later
        kCodecHeadphoneLevel,  // headphoneSteps
        kCodecNoMicrophone,    // capture turned off for the boot
        kCodecCaptureOn,
        kCodecCaptureOff,
    };

    static IOReturn codecGated(OSObject *owner, void *step, void *, void *, void *);

    void codecStep(CodecStep step);

    static void codecIdle(OSObject *owner, IOTimerEventSource *);

    static void jackEdge(OSObject *owner, IOInterruptEventSource *, int);

    void noteJack();

    void publishJack();

    static void jackSettled(OSObject *owner, IOTimerEventSource *);

#if DEVELOPMENT || DEBUG
    static void statsFired(OSObject *owner, IOTimerEventSource *sender);

    void statsResume();
#endif

    void codecStart();

    static IOReturn requestGated(OSObject *owner, void *selector, void *, void *, void *);

    bool apePartitionOn();

    static IOReturn sleepGated(OSObject *owner, void *entering, void *, void *, void *);

    void codecQuiesce();

    static IOReturn codecSleepGated(OSObject *owner, void *entering, void *, void *, void *);

    static IOReturn shutdownGated(OSObject *owner, void *, void *, void *, void *);

    static IOReturn codecShutdownGated(OSObject *owner, void *, void *, void *, void *);

    static IOReturn powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t);

    void systemShutdown();

public:
    void systemSleep(bool entering);

    bool start(IOService *provider) override;

    void free() override;

    IOReturn request(uint32_t selector);

    void noteClock();

    static IOReturn rateGated(OSObject *owner, void *next, void *, void *, void *);

    IOReturn setRate(uint32_t rate);

    IOReturn setHeadphoneLevel(uint64_t attenuationMdB);

    IOMemoryDescriptor *copyMemory(uint32_t type);
};
