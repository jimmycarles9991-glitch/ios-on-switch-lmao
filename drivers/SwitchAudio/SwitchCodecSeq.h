// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "SwitchRT5639.h"

namespace switch_audio {

constexpr uint32_t kCodecBiasStandbyMS = 25;
constexpr uint32_t kCodecSpeakerBiasMS = 15;
constexpr uint32_t kCodecSpeakerClassDMS = 15;
constexpr uint32_t kCodecHeadphoneAmpMS = 15;
constexpr uint32_t kCodecHeadphoneDepopMS = 180;
constexpr uint32_t kCodecSpeakerDownMS = 15;
constexpr uint32_t kCodecHeadphoneDownMS = 30;
constexpr uint32_t kCodecBiasOffMS = 5;
constexpr uint32_t kCodecHeadsetDetectMS = 100;
constexpr uint32_t kCodecPauseRampMS = 18;
constexpr uint32_t kCodecUnmuteSettleMS = 5;

enum class CodecPhase : uint8_t {
    kOff,
    kBiasUpWait,
    kBiasReady,
    kSpkUpWaitClassD,
    kSpkUpWaitUnmute,
    kHpUpWaitAmp,
    kHpUpWaitDepop,
    kRouteActive,
    kSpkDownWait,
    kHpDownWait,
    kBiasOffWait,
};

// Non-blocking RT5639 bias and route sequencer driven by a single work loop and one-shot timer.
// Discharge phases (kSpkDownWait, kHpDownWait, kBiasOffWait) always run to completion before reversing
// so Class-D and headphone charge-pump rails settle without audible pops.
template <class Codec, class Host> class SwitchCodecSeq {
public:
    SwitchCodecSeq() = default;

    SwitchCodecSeq(Codec *codec, Host host) : codec_(codec), host_(host)
    {}

    void init(Codec *codec, Host host)
    {
        codec_ = codec;
        host_ = host;
    }

    void reset()
    {
        host_.cancelTimer();
        phase_ = CodecPhase::kOff;
        activeRoute_ = rt5639::kRouteNone;
        wantBias_ = false;
        wantCodecOn_ = false;
        wantRoute_ = rt5639::kRouteNone;
    }

    void setTarget(bool wantBias, bool wantCodecOn, rt5639::Route wantRoute)
    {
        wantBias_ = wantBias || wantCodecOn;
        wantCodecOn_ = wantCodecOn;
        wantRoute_ = wantCodecOn ? wantRoute : rt5639::kRouteNone;
    }

    void setWantBias(bool wantBias)
    {
        wantBias_ = wantBias || wantCodecOn_;
    }

    void setWantRoute(rt5639::Route route)
    {
        wantRoute_ = wantCodecOn_ ? route : rt5639::kRouteNone;
    }

    CodecPhase phase() const
    {
        return phase_;
    }

    rt5639::Route activeRoute() const
    {
        return activeRoute_;
    }

    rt5639::Route targetRoute() const
    {
        return wantCodecOn_ ? wantRoute_ : rt5639::kRouteNone;
    }

    bool wantsBias() const
    {
        return wantBias_ || wantCodecOn_;
    }

    bool wantsCodecOn() const
    {
        return wantCodecOn_;
    }

    bool isOff() const
    {
        return phase_ == CodecPhase::kOff;
    }

    bool isRouteSettled() const
    {
        return wantCodecOn_ && phase_ == CodecPhase::kRouteActive && activeRoute_ == wantRoute_;
    }

    void reconcile()
    {
        for (;;) {
            const bool needBias = wantBias_ || wantCodecOn_;
            const rt5639::Route needRoute = wantCodecOn_ ? wantRoute_ : rt5639::kRouteNone;
            switch (phase_) {
            case CodecPhase::kOff:
                if (!needBias)
                    return;
                codec_->biasStandbyBegin();
                phase_ = CodecPhase::kBiasUpWait;
                host_.armTimer(kCodecBiasStandbyMS);
                return;
            case CodecPhase::kBiasUpWait:
                if (needBias)
                    return;
                host_.cancelTimer();
                codec_->biasOff();
                phase_ = CodecPhase::kBiasOffWait;
                host_.armTimer(kCodecBiasOffMS);
                return;
            case CodecPhase::kBiasReady:
                if (!needBias) {
                    codec_->biasOff();
                    phase_ = CodecPhase::kBiasOffWait;
                    host_.armTimer(kCodecBiasOffMS);
                    return;
                }
                if (needRoute == rt5639::kRouteSpeaker) {
                    codec_->powerDac();
                    codec_->speakerRouteUpBegin();
                    phase_ = CodecPhase::kSpkUpWaitClassD;
                    host_.armTimer(kCodecSpeakerBiasMS);
                    return;
                }
                if (needRoute == rt5639::kRouteHeadphones) {
                    codec_->powerDac();
                    codec_->headphoneRouteUpBegin();
                    phase_ = CodecPhase::kHpUpWaitAmp;
                    host_.armTimer(kCodecHeadphoneAmpMS);
                    return;
                }
                return;
            case CodecPhase::kSpkUpWaitClassD:
                if (needRoute == rt5639::kRouteSpeaker)
                    return;
                host_.cancelTimer();
                codec_->speakerRouteDownEarly();
                phase_ = CodecPhase::kBiasReady;
                continue;
            case CodecPhase::kSpkUpWaitUnmute:
                if (needRoute == rt5639::kRouteSpeaker)
                    return;
                host_.cancelTimer();
                codec_->speakerRouteDownBegin();
                phase_ = CodecPhase::kSpkDownWait;
                host_.armTimer(kCodecSpeakerDownMS);
                return;
            case CodecPhase::kHpUpWaitAmp:
                if (needRoute == rt5639::kRouteHeadphones)
                    return;
                host_.cancelTimer();
                codec_->headphoneRouteDownEarly();
                phase_ = CodecPhase::kBiasReady;
                continue;
            case CodecPhase::kHpUpWaitDepop:
                if (needRoute == rt5639::kRouteHeadphones)
                    return;
                host_.cancelTimer();
                codec_->headphoneRouteDownBegin();
                phase_ = CodecPhase::kHpDownWait;
                host_.armTimer(kCodecHeadphoneDownMS);
                return;
            case CodecPhase::kRouteActive:
                if (needRoute == activeRoute_)
                    return;
                if (activeRoute_ == rt5639::kRouteSpeaker) {
                    codec_->speakerRouteDownBegin();
                    phase_ = CodecPhase::kSpkDownWait;
                    host_.armTimer(kCodecSpeakerDownMS);
                } else {
                    codec_->headphoneRouteDownBegin();
                    phase_ = CodecPhase::kHpDownWait;
                    host_.armTimer(kCodecHeadphoneDownMS);
                }
                return;
            case CodecPhase::kSpkDownWait:
            case CodecPhase::kHpDownWait:
            case CodecPhase::kBiasOffWait:
                return;
            }
        }
    }

    void timerFired()
    {
        switch (phase_) {
        case CodecPhase::kBiasUpWait:
            codec_->biasStandbyFinish();
            phase_ = CodecPhase::kBiasReady;
            host_.onBiasReady();
            reconcile();
            return;
        case CodecPhase::kSpkUpWaitClassD:
            if (wantCodecOn_ && wantRoute_ == rt5639::kRouteSpeaker) {
                codec_->speakerRouteUpClassD();
                phase_ = CodecPhase::kSpkUpWaitUnmute;
                host_.armTimer(kCodecSpeakerClassDMS);
                return;
            }
            codec_->speakerRouteDownEarly();
            phase_ = CodecPhase::kBiasReady;
            reconcile();
            return;
        case CodecPhase::kSpkUpWaitUnmute:
            codec_->speakerRouteUpFinish();
            activeRoute_ = rt5639::kRouteSpeaker;
            phase_ = CodecPhase::kRouteActive;
            if (isRouteSettled())
                host_.onRouteReady(activeRoute_);
            reconcile();
            return;
        case CodecPhase::kHpUpWaitAmp:
            if (wantCodecOn_ && wantRoute_ == rt5639::kRouteHeadphones) {
                codec_->headphoneRouteUpDepop();
                phase_ = CodecPhase::kHpUpWaitDepop;
                host_.armTimer(kCodecHeadphoneDepopMS);
                return;
            }
            codec_->headphoneRouteDownEarly();
            phase_ = CodecPhase::kBiasReady;
            reconcile();
            return;
        case CodecPhase::kHpUpWaitDepop:
            codec_->headphoneRouteUpFinish();
            activeRoute_ = rt5639::kRouteHeadphones;
            phase_ = CodecPhase::kRouteActive;
            if (isRouteSettled())
                host_.onRouteReady(activeRoute_);
            reconcile();
            return;
        case CodecPhase::kSpkDownWait:
            codec_->speakerRouteDownFinish();
            activeRoute_ = rt5639::kRouteNone;
            phase_ = CodecPhase::kBiasReady;
            reconcile();
            return;
        case CodecPhase::kHpDownWait:
            codec_->headphoneRouteDownFinish();
            activeRoute_ = rt5639::kRouteNone;
            phase_ = CodecPhase::kBiasReady;
            reconcile();
            return;
        case CodecPhase::kBiasOffWait:
            phase_ = CodecPhase::kOff;
            if (!wantsBias()) {
                host_.onOff();
                return;
            }
            reconcile();
            return;
        case CodecPhase::kOff:
        case CodecPhase::kBiasReady:
        case CodecPhase::kRouteActive:
            return;
        }
    }

private:
    Codec *codec_ = nullptr;
    Host host_{};
    CodecPhase phase_ = CodecPhase::kOff;
    rt5639::Route activeRoute_ = rt5639::kRouteNone;
    bool wantBias_ = false;
    bool wantCodecOn_ = false;
    rt5639::Route wantRoute_ = rt5639::kRouteNone;
};

}  // namespace switch_audio
