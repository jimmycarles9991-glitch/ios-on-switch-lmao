/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The sample format and hardware register words for SwitchAudio's Tegra210 APE pipeline (ADMAIF -> MIXER1 -> SFC1 ->
 * MVC1 -> OPE1 -> I2S1 -> RT5639) and capture path (I2S1 -> SFC2 -> ADMAIF), as plain C so the kernel service, HAL
 * plug-in, and host tests share one source of truth.
 *
 * Register layouts verified against Tegra X1 TRM Ch. 5/23, mainline sound/soc/tegra/tegra210_{sfc,mvc,ope,peq,mbdrc,
 * mixer,admaif,i2s}.*, tegra_cif.h, and mainline/L4T sound/soc/codecs/rt5640.{h,c}. */
#ifndef SWITCH_AUDIO_FORMAT_H
#define SWITCH_AUDIO_FORMAT_H
#include <stdint.h>

#ifdef __cplusplus
#define SWITCH_AUDIO_FN constexpr inline
#else
#define SWITCH_AUDIO_FN static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* TEGRA210_AUDIOCIF_BITS_* (CIF words) and TEGRA210_I2S_BITS_* (I2S_CTRL bit size) share one encoding. */
enum {
    SWITCH_AUDIO_BITS_16 = 3,
    SWITCH_AUDIO_BITS_24 = 5,
    SWITCH_AUDIO_BITS_32 = 7
};

#define SWITCH_AUDIO_SAMPLE_BITS  32u /* in memory, on ADMAIF and across AXBAR */
#define SWITCH_AUDIO_SLOT_BITS    32u /* one channel's I2S slot */
#define SWITCH_AUDIO_CODEC_BITS   24u /* what the RT5639 takes of each slot */
#define SWITCH_AUDIO_I2S_CHANNELS 2u
#define SWITCH_AUDIO_BCLK_FS      (SWITCH_AUDIO_SLOT_BITS * SWITCH_AUDIO_I2S_CHANNELS) /* 64 */

/* Audio CIF control word (tegra210_xbar_set_cif): FIFO threshold 24, audio channels - 1 at 20, client channels - 1
 * at 16, audio bits 12, client bits 8, stereo-to-mono 4 (0 = channel 0); expand, replicate, truncate, mono 0. */
SWITCH_AUDIO_FN uint32_t switch_audio_cif(uint32_t threshold, uint32_t audio_channels, uint32_t client_channels,
                                          uint32_t audio_bits, uint32_t client_bits)
{
    return threshold << 24 | (audio_channels - 1) << 20 | (client_channels - 1) << 16 | audio_bits << 12 |
           client_bits << 8;
}

/* ADMAIF channel CIF (TX_CTRL/RX_CTRL): PACK8 31, PACK16 30, both off for 32-bit data. */
SWITCH_AUDIO_FN uint32_t switch_audio_admaif_cif(uint32_t channels)
{
    return switch_audio_cif(0, channels, channels, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

/* AHUB internal module CIF (SFC, MIXER, MVC, OPE): full 32-bit internal precision; SFC mandates client_bits = 32. */
SWITCH_AUDIO_FN uint32_t switch_audio_ahub_cif(uint32_t channels)
{
    return switch_audio_cif(0, channels, channels, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

/* I2S1 playback (AXBAR RX: XBAR stereo -> DAP stereo) and capture (AXBAR TX: DAP stereo -> XBAR mono, left slot). */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_rx_cif(void)
{
    return switch_audio_cif(3, 2, 2, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

SWITCH_AUDIO_FN uint32_t switch_audio_i2s_tx_cif(void)
{
    return switch_audio_cif(0, 1, 2, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

/* I2S_CTRL: FSYNC width 31 at 24, master at 10, LRCK frame (12 = 0), LRCK low = left (9 = 0), positive edge (20 = 0),
 * bit size 2:0. */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_ctrl(void)
{
    return 31u << 24 | 1u << 10 | (uint32_t)SWITCH_AUDIO_BITS_32;
}

/* I2S_TIMING: channel bit count, LRCK half a frame: (bclk / fs - 1) >> 1 = 31; symmetric (64 fs is even). */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_timing(void)
{
    return (SWITCH_AUDIO_BCLK_FS - 1) >> 1;
}

/* CAR CLK_SOURCE_I2S0 7.1 divider (value = 2 * ratio - 2) for BCLK = 64 fs from PLLA_OUT0.
 * At PLLA_OUT0 = 1024 fs (49.152 / 45.1584 MHz): ratio = 16 (even, 50% duty) -> reg = 30. */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_divider(uint32_t pllaout0_per_fs)
{
    return 2 * (pllaout0_per_fs / SWITCH_AUDIO_BCLK_FS) - 2;
}

/* CAR CLK_SOURCE_EXTERN1 7.1 divider for MCLK = 256 fs from PLLA_OUT0.
 * At PLLA_OUT0 = 1024 fs: ratio = 4 (even, 50% duty) -> reg = 6. */
SWITCH_AUDIO_FN uint32_t switch_audio_extern1_divider(uint32_t pllaout0_per_fs)
{
    return 2 * (pllaout0_per_fs / 256) - 2;
}

/* T210B01 PLLA clock families from the 38.4 MHz oscillator, matching Tegra210's 49.152 MHz d_audio ceiling:
 *   48 kHz:   PLLA = 368.640234 MHz (+0.64 ppm), PLLA_OUT0 /7.5 = 49.152 MHz (1024 fs),
 *             MCLK /4 = 12.288 MHz (50% duty), BCLK /16 = 3.072 MHz (50% duty).
 *   44.1 kHz: PLLA = 338.688281 MHz (+0.83 ppm), PLLA_OUT0 /7.5 = 45.1584 MHz (1024 fs),
 *             MCLK /4 = 11.2896 MHz (50% duty), BCLK /16 = 2.8224 MHz (50% duty). */
typedef struct {
    uint32_t rate, m, n, p, sdm, out0_x2, out0_per_fs;
} switch_audio_clock;

#define SWITCH_AUDIO_CLOCK_48000 {48000u, 2u, 38u, 2u, 0xfccdu, 15u, 1024u}
#define SWITCH_AUDIO_CLOCK_44100 {44100u, 2u, 35u, 2u, 0xf8f6u, 15u, 1024u}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_n_eff(switch_audio_clock c)
{
    return (uint64_t)(8192 * (int64_t)c.n + 4096 + (int16_t)(uint16_t)c.sdm);
}

/* RT5639 (rt5640.h): I2S1_SDP slave (15), normal BCLK (7=0), 24-bit (3:2=2), I2S (1:0=0); ADDA_CLK1 BCLK/LRCK 64
 * (15=1) and I2S1 pre-divider /1 (14:12=0), preserving silicon/L4T 64x DAC OSR and 128x ADC OSR at MCLK = 256 fs. */
#define SWITCH_AUDIO_RT5640_SDP_MASK  ((1u << 15) | (1u << 7) | (3u << 2) | 3u)
#define SWITCH_AUDIO_RT5640_SDP       ((1u << 15) | (2u << 2))
#define SWITCH_AUDIO_RT5640_ADDA_MASK ((1u << 15) | (7u << 12))
#define SWITCH_AUDIO_RT5640_ADDA      (1u << 15)

SWITCH_AUDIO_FN uint64_t switch_audio_gcd64(uint64_t a, uint64_t b)
{
    while (b != 0) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

/* Host ticks per I2S frame in 32.32 fixed-point without 128-bit division helpers. */
SWITCH_AUDIO_FN uint64_t switch_audio_ticks_per_frame_q32_parts(uint64_t num, uint64_t den)
{
    if (!den)
        return 0;
    return (num / den) << 32 | ((num % den) << 16) / den << 16 | (((((num % den) << 16) % den) << 16) / den);
}

SWITCH_AUDIO_FN uint64_t switch_audio_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint64_t m, uint64_t p,
                                                          uint64_t n_eff, uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    const uint64_t g = switch_audio_gcd64(host_hz, 2 * ref_hz);
    const uint64_t h = host_hz / g;
    const uint64_t r = (2 * ref_hz) / g;
    return switch_audio_ticks_per_frame_q32_parts(h * 8192 * m * p * out0_x2 * pllaout0_per_fs, r * n_eff);
}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, switch_audio_clock c)
{
    return switch_audio_ticks_per_frame_q32(host_hz, ref_hz, c.m, c.p, switch_audio_clock_n_eff(c), c.out0_x2,
                                            c.out0_per_fs);
}

/* Host ticks per client frame in 32.32 when SFC converts client_rate <-> link.rate on PLLA. */
SWITCH_AUDIO_FN uint64_t switch_audio_client_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz,
                                                                 switch_audio_clock link, uint32_t client_rate)
{
    const uint64_t g_hr = switch_audio_gcd64(host_hz, 2 * ref_hz);
    const uint64_t g_rc = switch_audio_gcd64(link.rate, client_rate);
    const uint64_t num =
        (host_hz / g_hr) * 8192 * link.m * link.p * link.out0_x2 * link.out0_per_fs * (link.rate / g_rc);
    const uint64_t den = ((2 * ref_hz) / g_hr) * switch_audio_clock_n_eff(link) * (client_rate / g_rc);
    return switch_audio_ticks_per_frame_q32_parts(num, den);
}

/* Actual link frame rate in mHz, rounded down. */
SWITCH_AUDIO_FN uint64_t switch_audio_rate_millihz(uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff,
                                                   uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    return 2 * ref_hz * n_eff * 1000 / (8192 * m * p * out0_x2 * pllaout0_per_fs);
}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_millihz(uint64_t ref_hz, switch_audio_clock c)
{
    return switch_audio_rate_millihz(ref_hz, c.m, c.p, switch_audio_clock_n_eff(c), c.out0_x2, c.out0_per_fs);
}

/* Exact frame count elapsed in `ticks` host timer ticks at `ticks_per_frame_q32` (32.32), without 128-bit division.
 * Safe from overflow for any rate >= 1500 Hz (q < 2^44 -> rem << 16 < 2^60). */
SWITCH_AUDIO_FN uint64_t switch_audio_frames_from_ticks_q32(uint64_t ticks, uint64_t ticks_per_frame_q32)
{
    if (!ticks_per_frame_q32)
        return 0;
    const uint64_t q0 = ticks / ticks_per_frame_q32;
    const uint64_t r0 = ticks % ticks_per_frame_q32;
    const uint64_t a1 = r0 << 16;
    const uint64_t q1 = a1 / ticks_per_frame_q32;
    const uint64_t r1 = a1 % ticks_per_frame_q32;
    const uint64_t q2 = (r1 << 16) / ticks_per_frame_q32;
    return (q0 << 32) | (q1 << 16) | q2;
}

/* Tegra210 SFC RX_FREQ / TX_FREQ index (tegra210_sfc_rates[13]; index 8 = 64 kHz has no coefficient table). */
SWITCH_AUDIO_FN int32_t switch_audio_sfc_rate_index(uint32_t rate)
{
    switch (rate) {
    case 8000u:
        return 0;
    case 11025u:
        return 1;
    case 16000u:
        return 2;
    case 22050u:
        return 3;
    case 24000u:
        return 4;
    case 32000u:
        return 5;
    case 44100u:
        return 6;
    case 48000u:
        return 7;
    case 88200u:
        return 9;
    case 96000u:
        return 10;
    case 176400u:
        return 11;
    case 192000u:
        return 12;
    default:
        return -1;
    }
}

SWITCH_AUDIO_FN int switch_audio_is_44k_family(uint32_t rate)
{
    return rate == 11025u || rate == 22050u || rate == 44100u || rate == 88200u || rate == 176400u;
}

/* Tegra210 MVC LINEAR_RAMP volume register word: signed S24.8 dB in [-120.0 dB, +40.0 dB] (1/256 dB steps). */
SWITCH_AUDIO_FN int32_t switch_audio_mvc_linear_db_q8(int32_t millidb)
{
    if (millidb < -120000)
        millidb = -120000;
    if (millidb > 40000)
        millidb = 40000;
    const int64_t scaled = (int64_t)millidb * 256;
    return (int32_t)((scaled >= 0 ? scaled + 500 : scaled - 500) / 1000);
}

/* Tegra210 MBDRC IN_THRESHOLD / OUT_THRESHOLD word (bytes 0..3 packed little-endian; fixes upstream right-shift bug). */
SWITCH_AUDIO_FN uint32_t switch_audio_mbdrc_pack_thresh(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3)
{
    return (uint32_t)t0 | ((uint32_t)t1 << 8) | ((uint32_t)t2 << 16) | ((uint32_t)t3 << 24);
}

/* Tegra210 MBDRC FAST_FACTOR word: [31:16] release Q1.15, [15:0] attack Q1.15 (fixes upstream shift bug). */
SWITCH_AUDIO_FN uint32_t switch_audio_mbdrc_fast_factor(uint16_t release_q15, uint16_t attack_q15)
{
    return ((uint32_t)release_q15 << 16) | (uint32_t)attack_q15;
}

/* Hybrid headphone gain staging: keep RT5639 analog HP_VOL at or below 0.0 dB unity (reg 8..39 = 0.0..-46.5 dB in
 * 1.5 dB steps) to eliminate +/-1.8 V rail clipping and maximize SNR at low volumes, with smooth fine attenuation
 * (0..-1.499 dB, and below -46.5 dB) handled in APE MVC1. */
SWITCH_AUDIO_FN void switch_audio_hp_stage_split(uint32_t attenuation_mdb, uint8_t *hp_vol_reg, int32_t *mvc_mdb)
{
    const uint32_t kAnalogUnityReg = 8u;  /* 0.0 dB */
    const uint32_t kAnalogMaxSteps = 31u; /* 31 * 1.5 dB = 46.5 dB -> reg 39 */
    const uint32_t kAnalogMaxMdb = kAnalogMaxSteps * 1500u;
    if (attenuation_mdb <= kAnalogMaxMdb) {
        const uint32_t steps = attenuation_mdb / 1500u;
        *hp_vol_reg = (uint8_t)(kAnalogUnityReg + steps);
        *mvc_mdb = -(int32_t)(attenuation_mdb - steps * 1500u);
    } else {
        *hp_vol_reg = (uint8_t)(kAnalogUnityReg + kAnalogMaxSteps);
        const uint32_t rem = attenuation_mdb - kAnalogMaxMdb;
        *mvc_mdb = rem > 120000u ? -120000 : -(int32_t)rem;
    }
}

/* Direction state machine for playback (bit 0) and capture (bit 1). */
enum {
    SWITCH_AUDIO_HW_START = 1u << 0,
    SWITCH_AUDIO_HW_STOP = 1u << 1,
    SWITCH_AUDIO_CAPTURE_START = 1u << 2,
    SWITCH_AUDIO_CAPTURE_STOP = 1u << 3,
};

SWITCH_AUDIO_FN uint32_t switch_audio_transition(uint32_t was, uint32_t now)
{
    const uint32_t input = 1u << 1;
    uint32_t actions = 0;
    if (!was && now)
        actions |= SWITCH_AUDIO_HW_START;
    if ((now & input) && !(was & input))
        actions |= SWITCH_AUDIO_CAPTURE_START;
    if ((was & input) && !(now & input))
        actions |= SWITCH_AUDIO_CAPTURE_STOP;
    if (was && !now)
        actions |= SWITCH_AUDIO_HW_STOP;
    return actions;
}

/* Capture ring index for sample time t, or -1 (silence) outside [anchor, in_frames). */
SWITCH_AUDIO_FN int64_t switch_audio_capture_frame(uint64_t t, uint64_t anchor, uint64_t in_frames,
                                                   uint32_t ring_frames)
{
    if (anchor == ~(uint64_t)0 || t < anchor || t >= in_frames)
        return -1;
    return (int64_t)((t - anchor) % ring_frames);
}

#ifdef __cplusplus
}
#endif

#undef SWITCH_AUDIO_FN
#endif
