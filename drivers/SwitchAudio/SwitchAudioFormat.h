/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The sample format SwitchAudio's Tegra backend runs the APE in, as plain C (the kernel service, and the host test
 * test/switch_audio_format_test.cpp through test/switch_audio_format_harness.c, use this one header).
 *
 * S32_LE from memory to the codec: the rings hold 32-bit samples, ADMAIF1 moves them to the XBAR without packing
 * (32-bit audio and client words), I2S1 sends each in a 32-bit slot (bit clock 64 fs, 3.072 MHz at 48 kHz, an exact
 * 16th of PLLA_OUT0's 49.152 MHz), and the RT5639 reads the 24 bits it has from each slot (its I2S1 data length 24, its
 * BCLK/LRCK ratio 64). The APE's audio interfaces carry integer PCM only: an audio CIF word has 8/16/24/32-bit integer
 * samples (TEGRA210_AUDIOCIF_BITS_*) and no float format, so the plug-in's float32 still becomes integer on the CPU; at
 * 32 bits that is one saturating NEON conversion per four samples and keeps all 24 bits the DAC has.
 *
 * Register words as Linux builds them for that stream (GPL-2.0 sources, field layouts and formulas only):
 *  - CIF words: NVIDIA L4T sound/soc/tegra-alt/utils/tegra210_xbar_utils_alt.c tegra210_xbar_set_cif (Copyright (c)
 *    2014-2019 NVIDIA CORPORATION), fields in include/tegra210_xbar_alt.h;
 *  - ADMAIF: tegra210_admaif_alt.c tegra_admaif_hw_params for SNDRV_PCM_FORMAT_S32_LE (DATA_32BIT: PACK8 and PACK16
 *    off, 32-bit audio and client bits);
 *  - I2S: tegra210_i2s_alt.c tegra210_i2s_hw_params for S32_LE (BIT_SIZE_32, i2sclock = srate * 32 * 2 channels,
 *    TIMING channel bit count (i2sclock / srate - 1) >> 1 in LRCK mode, NON_SYM only if i2sclock % (2 srate));
 *  - RT5639: mainline sound/soc/codecs/rt5640.c rt5640_hw_params (Copyright 2011 Realtek Semiconductor Corp.) for a
 *    24-bit sample in a 64-bit frame (frame size > 32 -> bclk_ms 1, width 24 -> RT5640_I2S_DL_24, pre-divider 1 for
 *    MCLK 256 fs), fields in rt5640.h (RT5640_I2S1_SDP 0x70, RT5640_ADDA_CLK1 0x73). */
#ifndef SWITCH_AUDIO_FORMAT_H
#define SWITCH_AUDIO_FORMAT_H
#include <stdint.h>

#ifdef __cplusplus
#define SWITCH_AUDIO_FN constexpr inline
#else
#define SWITCH_AUDIO_FN static inline
#endif

/* TEGRA210_AUDIOCIF_BITS_* (CIF words) and TEGRA210_I2S_BITS_* (I2S_CTRL bit size) share one encoding. */
enum {
    SWITCH_AUDIO_BITS_16 = 3,
    SWITCH_AUDIO_BITS_24 = 5,
    SWITCH_AUDIO_BITS_32 = 7
};

#define SWITCH_AUDIO_SAMPLE_BITS  32u /* in memory, on the ADMAIF and the XBAR */
#define SWITCH_AUDIO_SLOT_BITS    32u /* one channel's I2S slot */
#define SWITCH_AUDIO_CODEC_BITS   24u /* what the RT5639 takes of each slot */
#define SWITCH_AUDIO_I2S_CHANNELS 2u
#define SWITCH_AUDIO_BCLK_FS      (SWITCH_AUDIO_SLOT_BITS * SWITCH_AUDIO_I2S_CHANNELS) /* 64 */

/* An audio CIF control word (tegra210_xbar_set_cif): FIFO threshold 24, audio channels - 1 at 20, client channels - 1
 * at 16, audio bits 12, client bits 8, stereo-to-mono 4 (0 = channel 0); expand, replicate, truncate, mono 0. */
SWITCH_AUDIO_FN uint32_t switch_audio_cif(uint32_t threshold, uint32_t audio_channels, uint32_t client_channels,
                                          uint32_t audio_bits, uint32_t client_bits)
{
    return threshold << 24 | (audio_channels - 1) << 20 | (client_channels - 1) << 16 | audio_bits << 12 |
           client_bits << 8;
}

/* ADMAIF channel CIF (TX_CTRL/RX_CTRL): PACK8 31, PACK16 30, both off for 32-bit data (tegra_admaif_set_pack_mode). */
SWITCH_AUDIO_FN uint32_t switch_audio_admaif_cif(uint32_t channels)
{
    return switch_audio_cif(0, channels, channels, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

/* I2S1 playback (AXBAR RX: XBAR stereo -> DAP stereo) and capture (AXBAR TX: DAP stereo -> XBAR mono, the left slot;
 * the codec puts IN1 on both). */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_rx_cif(void)
{
    return switch_audio_cif(3, 2, 2, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

SWITCH_AUDIO_FN uint32_t switch_audio_i2s_tx_cif(void)
{
    return switch_audio_cif(0, 1, 2, SWITCH_AUDIO_BITS_32, SWITCH_AUDIO_BITS_32);
}

/* I2S_CTRL: FSYNC width 31 at 24 (DTB fsync-width), master at 10, LRCK frame (12 = 0), LRCK low = left (9 = 0), data
 * on the positive edge (20 = 0), bit size 2:0. */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_ctrl(void)
{
    return 31u << 24 | 1u << 10 | (uint32_t)SWITCH_AUDIO_BITS_32;
}

/* I2S_TIMING: the channel bit count, LRCK half a frame: (bclk / fs - 1) >> 1 = 31; symmetric (64 fs is even). */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_timing(void)
{
    return (SWITCH_AUDIO_BCLK_FS - 1) >> 1;
}

/* CAR CLK_SOURCE_I2S0 divider (7.1: value = 2 * ratio - 2) for the bit clock from PLLA_OUT0: 49.152 MHz / 3.072 MHz
 * = 16 -> 30. pllaout0_per_fs is PLLA_OUT0 / fs (1024 at 49.152 MHz and 48 kHz). */
SWITCH_AUDIO_FN uint32_t switch_audio_i2s_divider(uint32_t pllaout0_per_fs)
{
    return 2 * (pllaout0_per_fs / SWITCH_AUDIO_BCLK_FS) - 2;
}

/* One sample-rate family's clocks from the 38.4 MHz oscillator: PLLA = ref * n_eff / (8192 m p) with n_eff = 8192 n +
 * 4096 + (int16_t)sdm (CTCaer switch-l4t-kernel-4.9 clk-tegra210b01.c pll_a_freq_table: 368.64 and 282.24 MHz), and
 * PLLA_OUT0 = PLLA * 2 / out0_x2, which is out0_per_fs times fs. The bit clock (64 fs) and MCLK (extern1, 256 fs)
 * divide PLLA_OUT0. 282.24 MHz is 6400 fs, and 1024 fs would need a ratio of 6.25, which a 7.1 divider can't make, so
 * the 44.1 kHz family's PLLA_OUT0 is 1280 fs (/5). */
typedef struct {
    uint32_t rate, m, n, p, sdm, out0_x2, out0_per_fs;
} switch_audio_clock;

#define SWITCH_AUDIO_CLOCK_48000 {48000u, 2u, 38u, 2u, 0xfcccu, 15u, 1024u}
#define SWITCH_AUDIO_CLOCK_44100 {44100u, 2u, 29u, 2u, 0xfcccu, 10u, 1280u}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_n_eff(switch_audio_clock c)
{
    return (uint64_t)(8192 * (int64_t)c.n + 4096 + (int16_t)(uint16_t)c.sdm);
}

/* CAR CLK_SOURCE_EXTERN1 divider (7.1) for MCLK = 256 fs from PLLA_OUT0. */
SWITCH_AUDIO_FN uint32_t switch_audio_extern1_divider(uint32_t pllaout0_per_fs)
{
    return 2 * (pllaout0_per_fs / 256) - 2;
}

/* RT5639 (rt5640.h): I2S1_SDP master/slave 15 (1 = slave), BCLK polarity 7 (0 = normal), data length 3:2 (2 = 24
 * bits), format 1:0 (0 = I2S); ADDA_CLK1 BCLK/LRCK 15 (1 = 64), I2S1 pre-divider 14:12 (0 = /1). */
#define SWITCH_AUDIO_RT5640_SDP_MASK  ((1u << 15) | (1u << 7) | (3u << 2) | 3u)
#define SWITCH_AUDIO_RT5640_SDP       ((1u << 15) | (2u << 2))
#define SWITCH_AUDIO_RT5640_ADDA_MASK ((1u << 15) | (7u << 12))
#define SWITCH_AUDIO_RT5640_ADDA      (1u << 15)

/* Host ticks per I2S frame in 32.32 for PLLA = ref * n_eff / (8192 m p) (n_eff in 1/8192: n + 1/2 + sdm / 8192),
 * PLLA_OUT0 = PLLA * 2 / out0_x2 (out0_x2 = 2 * the PLLA_OUT0 ratio: 15 for /7.5), bit clock = PLLA_OUT0 /
 * (pllaout0_per_fs / 64), fs = bit clock / 64. Split so nothing passes 64 bits (no 128-bit helpers in the kernel). */
SWITCH_AUDIO_FN uint64_t switch_audio_ticks_per_frame_q32_parts(uint64_t num, uint64_t den)
{
    return (num / den) << 32 | ((num % den) << 16) / den << 16 | (((((num % den) << 16) % den) << 16) / den);
}

SWITCH_AUDIO_FN uint64_t switch_audio_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint64_t m, uint64_t p,
                                                          uint64_t n_eff, uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    return switch_audio_ticks_per_frame_q32_parts(host_hz * 8192 * m * p * out0_x2 * pllaout0_per_fs,
                                                  2 * ref_hz * n_eff);
}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, switch_audio_clock c)
{
    return switch_audio_ticks_per_frame_q32(host_hz, ref_hz, c.m, c.p, switch_audio_clock_n_eff(c), c.out0_x2,
                                            c.out0_per_fs);
}

/* The same frame rate in mHz (for the log): 2 ref n_eff / (8192 m p out0_x2 pllaout0_per_fs), rounded down. */
SWITCH_AUDIO_FN uint64_t switch_audio_rate_millihz(uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff,
                                                   uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    return 2 * ref_hz * n_eff * 1000 / (8192 * m * p * out0_x2 * pllaout0_per_fs);
}

SWITCH_AUDIO_FN uint64_t switch_audio_clock_millihz(uint64_t ref_hz, switch_audio_clock c)
{
    return switch_audio_rate_millihz(ref_hz, c.m, c.p, switch_audio_clock_n_eff(c), c.out0_x2, c.out0_per_fs);
}

/* What the hardware does when a request moves the running directions from `was` to `now` (SwitchAudio*Running bits:
 * output 1, input 2). The I2S clocks and the playback DMA run while either direction does (one timeline); the capture
 * path (ADMA channel 1, I2S TX, the codec's ADC and microphone bias) only while input does, so playback alone never
 * powers it. */
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

/* The capture ring frame holding sample time t: ADMA channel 1 writes ring frame k at sample time anchor + k, and frames
 * below in_frames are in memory. -1 (silence) outside that window, or with no capture session (anchor all ones). */
SWITCH_AUDIO_FN int64_t switch_audio_capture_frame(uint64_t t, uint64_t anchor, uint64_t in_frames,
                                                   uint32_t ring_frames)
{
    if (anchor == ~(uint64_t)0 || t < anchor || t >= in_frames)
        return -1;
    return (int64_t)((t - anchor) % ring_frames);
}

#undef SWITCH_AUDIO_FN
#endif
