// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SwitchAudio service: state and shared members across SwitchAudio*.cpp.
#pragma once
#include "SwitchAudioRegisters.h"
#include "SwitchCodecSeq.h"
#include "SwitchHDA.h"
#include <kern/thread_call.h>

struct alignas(8) SwitchAudioSink {
    uint32_t formats, generation;
};

SwitchAudioSink switch_audio_display_sink();

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs external linkage
class SwitchAudio : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchAudio) IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOBufferMemoryDescriptor *memory[5] = {};
    IOMemoryDescriptor *hostClockWire = nullptr;  // pins patched 19.2 MHz shared-cache page against VM eviction
    SwitchAudioControl *control = nullptr;
    IOWorkLoop *displayLoop = nullptr;
    IOCommandGate *displayGate = nullptr;
    IOTimerEventSource *displayPowerTimer = nullptr;
    SwitchHDA displayAudio;
    IOInterruptEventSource *displayInterrupt = nullptr;
    IOInterruptEventSource *displayEvent = nullptr;
    thread_call_t displayCall = nullptr;
    bool displayAsleep = false;
    bool displayPresencePending = false;  // DP uses codec clocks while the Audio gate is open.
    bool displayInit();
    void displayFree();
    static void displayFired(OSObject *, IOInterruptEventSource *, int);
    static void displayChanged(OSObject *, IOInterruptEventSource *, int);
    static void displayPowerFired(OSObject *, IOTimerEventSource *);
    static void displayProbe(thread_call_param_t, thread_call_param_t);
    static IOReturn displayPrepareGated(OSObject *, void *, void *, void *, void *);
    static IOReturn displayConnectGated(OSObject *, void *, void *, void *, void *);
    static IOReturn displayRequestGated(OSObject *, void *, void *, void *, void *);
    static IOReturn displaySleepGated(OSObject *, void *, void *, void *, void *);

    uint64_t hz = 0;
    const switch_audio_clock *clock = &switch_audio_hw::kClocks[0];  // active PLLA/I2S link clock family
    uint32_t clientRate = SWITCH_AUDIO_RATE;                         // active client rate (SFC converts if != link)

    bool tegra = false, tegraCapture = false;
    volatile uint8_t *pinmux = nullptr, *ahub = nullptr, *adma = nullptr, *agic = nullptr;
    uint32_t ringPA[3] = {};
    uint32_t jackPinFound = 0;
    IOInterruptEventSource *cascade = nullptr;
    uint64_t periods[2] = {};
    uint16_t xferSeen[2] = {};
    bool graced = false;

    // Runtime APE idle power-gating (Phase 3): held while any stream or codec session is active.
    SwitchEngineGate<SwitchEngineGateKernel> apeGate;
    bool apeHeld = false;
    bool ahubActive = false;
    bool ahubDraining = false;
    bool ahubStopping = false;
    bool hwBusy = false;
    uint32_t mmioWait = 0;
    bool playbackSfcActive = false;
    bool captureSfcActive = false;

    struct CodecSeqHost {
        SwitchAudio *audio = nullptr;
        void armTimer(uint32_t ms) const;
        void cancelTimer() const;
        void onBiasReady() const;
        void onRouteReady(rt5639::Route route) const;
        void onOff() const;
    };

    IOWorkLoop *codecLoop = nullptr;
    IOCommandGate *codecGate = nullptr;
    IOInterruptEventSource *jackSource = nullptr, *pv6Source = nullptr, *codecEvent = nullptr;
    IOTimerEventSource *jackTimer = nullptr, *idleTimer = nullptr;
    IOTimerEventSource *codecTimer = nullptr, *headsetTimer = nullptr, *unmuteTimer = nullptr, *pauseTimer = nullptr;
    switch_audio::SwitchCodecSeq<SwitchRT5639, CodecSeqHost> codecSeq;
    bool codecOn = false, jackKnown = false, jackIn = false, headset = false, speakerOnly = false;
    bool captureProven = false;
    bool captureRunning = false;
    bool captureCodecOn = false;
    bool wantCaptureCodec = false;
    bool wantUnmuted = false;
    bool unmuteTimerArmed = false;
    bool pauseRampPending = false;
    bool headsetDetectPending = false;
    bool headsetDetectActive = false;
    bool bootPrecharge = false;
    bool wantReleaseApe = false;
    bool codecQuiescing = false;
    uint32_t codecWaitToken = 0;
    rt5639::Route lastAppliedOpeRoute = rt5639::kRouteNone;
    uint64_t captureStartFrame = 0;
    uint32_t requestedRoute = SwitchAudioRouteAuto;
    uint32_t headphoneAttenuationMdB = 0;
    uint32_t speakerAttenuationMdB = 0;
    bool outputMuted = false;
    bool hardwareLost = false;
    bool codecOff = false;
    IONotifier *shutdownNotifier = nullptr;
    SwitchRT5639 codec;
#if DEVELOPMENT || DEBUG
    uint32_t statUnderruns = 0;
#endif
#if SWITCH_STATS_IO
    IOTimerEventSource *statsTimer = nullptr;
    uint64_t statFrames = 0, statFramesShown = 0, statIoCyclesShown = 0, statIoFramesShown = 0;
    uint32_t statStarts = 0, statStartsShown = 0;
    bool statsArmed = false, statsShown = false;
    int statPeak = 0;
    uint32_t statRingPeakL = 0, statRingPeakR = 0;
    int64_t statLagMin = INT64_MAX, statLagMax = INT64_MIN, statLeadMin = INT64_MAX;
    uint64_t periodTicksQ16 = 0;
#endif

    // Exact frame count on the active I2S/SFC clock timeline (zero drift vs HAL ticks_per_frame_q32).
    uint64_t nowFrame() const
    {
        const uint64_t now = mach_absolute_time();
        const uint64_t anchor = control->anchor_host;
        if (now <= anchor)
            return 0;
        const uint64_t tpf = control->ticks_per_frame_q32;
        if (tpf)
            return switch_audio_frames_from_ticks_q32(now - anchor, tpf);
        const uint64_t ticks = now - anchor;
        return ticks / hz * clientRate + ticks % hz * clientRate / hz;
    }

    switch_audio_sample_t *ring(uint32_t type) const
    {
        return static_cast<switch_audio_sample_t *>(memory[type]->getBytesNoCopy());
    }

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
            control->in_frames = now;
    }

    static void tick(OSObject *owner, IOTimerEventSource *source);

    static volatile uint8_t *map(vm_offset_t pa, vm_size_t size, const char *what)
    {
        volatile uint8_t *base = switch_mmio_map(pa, size);
        if (!base)
            panic("SwitchAudio: cannot map %s at %#lx", what, (unsigned long)pa);
        return base;
    }

    uint64_t tegraTicksPerFrame(uint64_t refHz) const
    {
        return switch_audio_client_ticks_per_frame_q32(hz, refHz, *clock, clientRate);
    }

    uint32_t watchdogMS() const
    {
        const uint32_t r = clientRate ? clientRate : clock->rate;
        return static_cast<uint32_t>(4ULL * switch_audio_hw::kOutPeriodFrames * 1000ULL / r);
    }

    struct HwTransitionGuard {
        SwitchAudio *self;

        explicit HwTransitionGuard(SwitchAudio *audio) : self(audio)
        {
            while (self->hwBusy)
                self->gate->commandSleep(&self->hwBusy, THREAD_UNINT);
            self->hwBusy = true;
        }

        ~HwTransitionGuard()
        {
            self->hwBusy = false;
            self->gate->commandWakeup(&self->hwBusy, false);
        }
    };

    void startPlla();
    static void stopPlla();
    void startClocks();
    void tegraClocksAndPads();
    void tegraAhubAndDma();
    void tegraAgic();
    void tegraInit();
    bool waitMmio(const volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs);
    void softReset(uint32_t reg, const char *what);
    void i2sSoftReset(uint32_t reg, const char *what);
    void stopUnit(uint32_t enable, uint32_t status, const char *what);

    static bool apePartitionOn();
    static bool apePartitionOff();
    static int apeGateOn(void *engine);
    static int apeGateOff(void *engine, bool drain);
    bool acquireApe();
    void releaseApe();
    void publishHwAnchor(uint64_t sampleTime, uint64_t hostTime, bool bumpSeed);
    int32_t desiredMvcMdB() const;
    void applyMvcLiveUnlocked(bool immediate);
    void applyMvcLive(bool immediate);
    bool rampMvcMute();
    static IOReturn finishStopGated(OSObject *owner, void *, void *, void *, void *);
    void reconfigureOpeForRoute();

    void tegraStart();
    void programChannel(unsigned ch);
    void captureStart();
    void captureStop();
    void tegraStop();
    void playedPeriod();
#if SWITCH_STATS_IO
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
    void codecPowerDown(bool wait, bool releaseWhenOff);
    void codecBiasReady();
    void codecRouteReady(rt5639::Route route);
    void codecNowOff();
    static void codecWaitGateMS(void *ctx, uint32_t ms);

    enum CodecStep : uintptr_t {
        kCodecPowerUp,
        kCodecUnmute,
        kCodecMute,
        kCodecPause,
        kCodecIdleNow,
        kCodecHeadphoneLevel,
        kCodecNoMicrophone,
        kCodecCaptureOn,
        kCodecCaptureOff,
        kCodecRouteSync,
    };

    static IOReturn codecGated(OSObject *owner, void *step, void *, void *, void *);
    void codecStep(CodecStep step);
    static void codecIdle(OSObject *owner, IOTimerEventSource *);
    static void codecSeqTimerFired(OSObject *owner, IOTimerEventSource *);
    static void headsetDetectSettled(OSObject *owner, IOTimerEventSource *);
    static void codecUnmuteFired(OSObject *owner, IOTimerEventSource *);
    static void codecPauseFired(OSObject *owner, IOTimerEventSource *);
    static void codecNoMicFired(OSObject *owner, IOInterruptEventSource *, int);
    static void jackEdge(OSObject *owner, IOInterruptEventSource *, int);
    void noteJack();
    void publishJack();
    static void jackSettled(OSObject *owner, IOTimerEventSource *);
#if SWITCH_STATS_IO
    static void statsFired(OSObject *owner, IOTimerEventSource *sender);
    void statsResume();
#endif
#if DEVELOPMENT || DEBUG
    void registerSysctl();
#endif
    void codecStart();
    static IOReturn requestGated(OSObject *owner, void *selector, void *, void *, void *);
    static IOReturn sleepGated(OSObject *owner, void *entering, void *, void *, void *);
    void codecQuiesce();
    static IOReturn codecSleepGated(OSObject *owner, void *entering, void *, void *, void *);
    static IOReturn shutdownGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn codecShutdownGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t);
    void systemShutdown();

public:
    static bool waitMmioActive(const volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value,
                               uint32_t timeoutUs);
    void systemSleep(bool entering);
    void displaySink();
    void displayStartProbe();
    IOReturn displayRequest(bool start);
    IOReturn displaySetRate(uint32_t rate);
    bool start(IOService *provider) override;
    void free() override;
    IOReturn request(uint32_t selector);
    void noteClock();
    static IOReturn rateGated(OSObject *owner, void *rateArg, void *, void *, void *);
    IOReturn setRate(uint32_t rate);
    IOReturn setHeadphoneLevel(uint64_t attenuationMdB);
    IOReturn setSpeakerLevel(uint64_t attenuationMdB);
    IOReturn setMute(bool muted);
    IOReturn setRoute(uint32_t route);
    IOReturn setHostClock(task_t task, uint64_t address);
    IOMemoryDescriptor *copyMemory(uint32_t type);
#if DEVELOPMENT || DEBUG
    void formatSysctl(char *buf, size_t len);
#endif
};
