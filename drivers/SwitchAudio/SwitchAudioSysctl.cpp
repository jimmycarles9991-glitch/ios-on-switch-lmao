// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug-only read-only telemetry sysctl (kern.switch_audio) for on-device HWTests.
#include "SwitchAudioDriver.h"

#if DEVELOPMENT || DEBUG
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"

using namespace switch_audio_hw;

namespace {
SwitchAudio *gSysctlAudio;
struct sysctl_oid gAudioOid;

struct ApeSnapshot {
    uint32_t state = 0;
    uint32_t rate = 0;
    uint32_t linkRate = 0;
    uint64_t pllaMilliHz = 0;
    uint32_t activeRoute = 0;
    uint32_t jack = 0;
    uint32_t hwSeed = 0;
    uint64_t hwSample = 0;
    uint32_t underruns = 0;
    uint32_t sfc1En = 0;
    uint32_t sfc1Coef = 0;
    uint32_t mixerEn = 0;
    uint32_t mixerSamples = 0;
    uint32_t mvcEn = 0;
    int32_t mvcVol = 0;
    uint32_t mvcMuted = 0;
    uint32_t mvcPeak = 0;
    uint32_t mvcPeakL = 0;
    uint32_t mvcPeakR = 0;
    uint32_t mvcErr = 0;
    uint32_t opeEn = 0;
    uint32_t peqEn = 0;
    uint32_t mbdrcEn = 0;
};

struct CodecSnapshot {
    uint32_t ok = 0;
    uint16_t hpVol = 0;
    uint16_t spkVol = 0;
    uint16_t addaClk1 = 0;
    uint16_t svZcd1 = 0;
};

const char *routeName(uint32_t route)
{
    if (route == SwitchAudioRouteSpeaker)
        return "speaker";
    if (route == SwitchAudioRouteHeadphones)
        return "headphones";
    return "none";
}

int audioSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    char line[576] = "down";
    if (SwitchAudio *audio = __atomic_load_n(&gSysctlAudio, __ATOMIC_ACQUIRE))
        audio->formatSysctl(line, sizeof(line));
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}
}  // namespace

void SwitchAudio::formatSysctl(char *buf, size_t len)
{
    ApeSnapshot ape;
    const IOCommandGate::Action apeAction = [](OSObject *owner, void *out, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<SwitchAudio *>(owner);
        auto *s = static_cast<ApeSnapshot *>(out);
        s->state = __atomic_load_n(&self->control->state, __ATOMIC_ACQUIRE);
        s->rate = self->clientRate;
        s->linkRate = self->clock ? self->clock->rate : 0u;
        s->pllaMilliHz = self->tegra && self->clock ? switch_audio_clock_millihz(38400000, *self->clock) : 0u;
        s->activeRoute = __atomic_load_n(&self->control->active_route, __ATOMIC_ACQUIRE);
        s->jack = __atomic_load_n(&self->control->jack, __ATOMIC_ACQUIRE);
        s->hwSeed = __atomic_load_n(&self->control->hw_seed, __ATOMIC_ACQUIRE);
        s->hwSample = __atomic_load_n(&self->control->hw_sample_time, __ATOMIC_ACQUIRE);
        s->underruns = self->statUnderruns;
        // Touch AHUB MMIO only while apeHeld pins the engine gate powered and active.
        if (self->tegra && !self->hardwareLost && !self->hwBusy && __atomic_load_n(&self->apeHeld, __ATOMIC_ACQUIRE) &&
            self->apeGate.powered() && self->ahubActive) {
            using namespace switch_audio_ahub;
            s->sfc1En = switch_mmio_read32(self->ahub, kSfc1Base + kSfcEnable) & 1u;
            s->sfc1Coef = switch_mmio_read32(self->ahub, kSfc1Base + kSfcCoefRam) & 1u;
            const uint32_t mixTx = switch_mmio_read32(self->ahub, kMixer1Base + kMixerTx1Enable) & 1u;
            const uint32_t mixGlb = switch_mmio_read32(self->ahub, kMixer1Base + kMixerGlobalEnable) & 1u;
            s->mixerEn = (mixTx && mixGlb) ? 1u : 0u;
            s->mixerSamples = mixerSampleCount(self->ahub);
            s->mvcEn = switch_mmio_read32(self->ahub, kMvc1Base + kMvcEnable) & 1u;
            s->mvcVol = static_cast<int32_t>(switch_mmio_read32(self->ahub, kMvc1Base + kMvcTargetVol));
            s->mvcMuted = (switch_mmio_read32(self->ahub, kMvc1Base + kMvcCtrl) & kMvcCtrlMuteMask) ? 1u : 0u;
            mvcReadPeakChannels(self->ahub, &s->mvcPeakL, &s->mvcPeakR);
            s->mvcPeak = s->mvcPeakL > s->mvcPeakR ? s->mvcPeakL : s->mvcPeakR;
            s->mvcErr = mvcReadConfigErr(self->ahub);
            s->opeEn = switch_mmio_read32(self->ahub, kOpe1Base + kOpeEnable) & 1u;
            s->peqEn = (switch_mmio_read32(self->ahub, kPeq1Base + kPeqCfg) & kPeqCfgModeActive) ? 1u : 0u;
            s->mbdrcEn = (switch_mmio_read32(self->ahub, kMbdrc1Base + kMbdrcCfg) & 0xfu) ? 1u : 0u;
        }
        return kIOReturnSuccess;
    };
    if (gate)
        gate->runAction(apeAction, &ape);

    CodecSnapshot cs;
    const IOCommandGate::Action codecAction = [](OSObject *owner, void *out, void *, void *, void *) -> IOReturn {
        const auto *self = static_cast<const SwitchAudio *>(owner);
        auto *c = static_cast<CodecSnapshot *>(out);
        // Never touch I2C while idle/gated so idle power tests observe a truly asleep subsystem.
        if (self->tegra && !self->codecOff && !self->hardwareLost &&
            __atomic_load_n(&self->apeHeld, __ATOMIC_ACQUIRE) && self->apeGate.powered() && SwitchRT5639::answering()) {
            c->hpVol = SwitchRT5639::read(rt5639::HP_VOL);
            c->spkVol = SwitchRT5639::read(rt5639::SPK_VOL);
            c->addaClk1 = SwitchRT5639::read(rt5639::ADDA_CLK1);
            c->svZcd1 = SwitchRT5639::read(rt5639::SV_ZCD1);
            c->ok = SwitchRT5639::answering() ? 1u : 0u;
        }
        return kIOReturnSuccess;
    };
    if (tegra && codecGate)
        codecGate->runAction(codecAction, &cs);

    const SwitchEngineGateStats gs = apeGate.stats();
    const unsigned powered = tegra && apeGate.powered() ? 1u : 0u;
    snprintf(buf, len,
             "backend=%s state=%u powered=%u gates=%llu ungates=%llu resume_us=%llu "
             "rate=%u link_rate=%u plla_millihz=%llu sfc1_en=%u sfc1_coef=%u "
             "mixer_en=%u mixer_samples=%u mvc_en=%u mvc_vol=%d mvc_muted=%u mvc_peak=%u mvc_peak_l=%u mvc_peak_r=%u "
             "mvc_err=0x%x ope_en=%u peq_en=%u mbdrc_en=%u route=%s jack=%u hw_seed=%u hw_sample=%llu "
             "underruns=%u codec_ok=%u hp_vol=0x%04x spk_vol=0x%04x adda_clk1=0x%04x sv_zcd1=0x%04x",
             tegra ? "tegra" : "null", ape.state, powered, static_cast<unsigned long long>(gs.gates),
             static_cast<unsigned long long>(gs.ungates), static_cast<unsigned long long>(gs.lastResumeUs), ape.rate,
             ape.linkRate, static_cast<unsigned long long>(ape.pllaMilliHz), ape.sfc1En, ape.sfc1Coef, ape.mixerEn,
             ape.mixerSamples, ape.mvcEn, ape.mvcVol, ape.mvcMuted, ape.mvcPeak, ape.mvcPeakL, ape.mvcPeakR, ape.mvcErr,
             ape.opeEn, ape.peqEn, ape.mbdrcEn, routeName(ape.activeRoute), ape.jack, ape.hwSeed,
             static_cast<unsigned long long>(ape.hwSample), ape.underruns, cs.ok, cs.hpVol, cs.spkVol, cs.addaClk1,
             cs.svZcd1);
}

void SwitchAudio::registerSysctl()
{
    static bool registered;
    __atomic_store_n(&gSysctlAudio, this, __ATOMIC_RELEASE);
    if (registered)
        return;
    switch_sysctl_oid(&gAudioOid, &sysctl__kern_children, "switch_audio",
                      static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED), audioSysctl, "A",
                      "SwitchAudio Tegra APE pipeline, clock, power-gate and RT5639 state");
    sysctl_register_oid(&gAudioOid);
    registered = true;
}
#endif
