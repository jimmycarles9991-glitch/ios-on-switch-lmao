// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for switch_audio_format_test.cpp: the APE/codec register words of SwitchAudio's S32 link
// (drivers/SwitchAudio/SwitchAudioFormat.h) and the plug-in's float32 <-> S32 conversions
// (drivers/SwitchAudio/hal/SwitchAudioConvert.h), compiled as C as they ship and exported
// (switch_audio_format_harness.h). Built twice (BUILD.bazel): with NEON (the arm64 host, as the device), its exports
// prefixed neon_, and with __ARM_NEON undefined (the scalar fallback), prefixed scalar_, to compare.
#include <stdint.h>

#include "../SwitchAudioFormat.h"
#include "../hal/SwitchAudioConvert.h"
#include "switch_audio_format_harness.h"

uint32_t SWITCH_AUDIO_HARNESS(h_cif)(uint32_t threshold, uint32_t audio_channels, uint32_t client_channels,
                                     uint32_t audio_bits, uint32_t client_bits)
{
    return switch_audio_cif(threshold, audio_channels, client_channels, audio_bits, client_bits);
}

uint32_t SWITCH_AUDIO_HARNESS(h_admaif_cif)(uint32_t channels)
{
    return switch_audio_admaif_cif(channels);
}

uint32_t SWITCH_AUDIO_HARNESS(h_i2s_rx_cif)(void)
{
    return switch_audio_i2s_rx_cif();
}

uint32_t SWITCH_AUDIO_HARNESS(h_i2s_tx_cif)(void)
{
    return switch_audio_i2s_tx_cif();
}

uint32_t SWITCH_AUDIO_HARNESS(h_i2s_ctrl)(void)
{
    return switch_audio_i2s_ctrl();
}

uint32_t SWITCH_AUDIO_HARNESS(h_i2s_timing)(void)
{
    return switch_audio_i2s_timing();
}

uint32_t SWITCH_AUDIO_HARNESS(h_i2s_divider)(uint32_t pllaout0_per_fs)
{
    return switch_audio_i2s_divider(pllaout0_per_fs);
}

uint32_t SWITCH_AUDIO_HARNESS(h_bclk_fs)(void)
{
    return SWITCH_AUDIO_BCLK_FS;
}

uint32_t SWITCH_AUDIO_HARNESS(h_codec_bits)(void)
{
    return SWITCH_AUDIO_CODEC_BITS;
}

uint32_t SWITCH_AUDIO_HARNESS(h_rt5640_sdp_mask)(void)
{
    return SWITCH_AUDIO_RT5640_SDP_MASK;
}

uint32_t SWITCH_AUDIO_HARNESS(h_rt5640_sdp)(void)
{
    return SWITCH_AUDIO_RT5640_SDP;
}

uint32_t SWITCH_AUDIO_HARNESS(h_rt5640_adda_mask)(void)
{
    return SWITCH_AUDIO_RT5640_ADDA_MASK;
}

uint32_t SWITCH_AUDIO_HARNESS(h_rt5640_adda)(void)
{
    return SWITCH_AUDIO_RT5640_ADDA;
}

uint64_t SWITCH_AUDIO_HARNESS(h_ticks_per_frame_q32)(uint64_t host_hz, uint64_t ref_hz, uint64_t m, uint64_t p,
                                                     uint64_t n_eff, uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    return switch_audio_ticks_per_frame_q32(host_hz, ref_hz, m, p, n_eff, out0_x2, pllaout0_per_fs);
}

uint64_t SWITCH_AUDIO_HARNESS(h_rate_millihz)(uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff, uint64_t out0_x2,
                                              uint64_t pllaout0_per_fs)
{
    return switch_audio_rate_millihz(ref_hz, m, p, n_eff, out0_x2, pllaout0_per_fs);
}

static switch_audio_clock clock_at(uint32_t index)
{
    const switch_audio_clock clocks[] = {SWITCH_AUDIO_CLOCK_48000, SWITCH_AUDIO_CLOCK_44100};
    return clocks[index];
}

void SWITCH_AUDIO_HARNESS(h_clock)(uint32_t index, uint32_t fields[7])
{
    const switch_audio_clock c = clock_at(index);
    const uint32_t values[7] = {c.rate, c.m, c.n, c.p, c.sdm, c.out0_x2, c.out0_per_fs};
    for (uint32_t i = 0; i < 7; ++i)
        fields[i] = values[i];
}

uint64_t SWITCH_AUDIO_HARNESS(h_clock_n_eff)(uint32_t index)
{
    return switch_audio_clock_n_eff(clock_at(index));
}

uint64_t SWITCH_AUDIO_HARNESS(h_clock_ticks_per_frame_q32)(uint64_t host_hz, uint64_t ref_hz, uint32_t index)
{
    return switch_audio_clock_ticks_per_frame_q32(host_hz, ref_hz, clock_at(index));
}

uint64_t SWITCH_AUDIO_HARNESS(h_clock_millihz)(uint64_t ref_hz, uint32_t index)
{
    return switch_audio_clock_millihz(ref_hz, clock_at(index));
}

uint32_t SWITCH_AUDIO_HARNESS(h_extern1_divider)(uint32_t pllaout0_per_fs)
{
    return switch_audio_extern1_divider(pllaout0_per_fs);
}

uint32_t SWITCH_AUDIO_HARNESS(h_transition)(uint32_t was, uint32_t now)
{
    return switch_audio_transition(was, now);
}

int64_t SWITCH_AUDIO_HARNESS(h_capture_frame)(uint64_t t, uint64_t anchor, uint64_t in_frames, uint32_t ring_frames)
{
    return switch_audio_capture_frame(t, anchor, in_frames, ring_frames);
}

void SWITCH_AUDIO_HARNESS(h_mix)(int32_t *dst, const float *src, uint32_t samples, float gain)
{
    switch_audio_mix_s32(dst, src, samples, gain);
}

uint32_t SWITCH_AUDIO_HARNESS(h_ring_mix)(int32_t *ring, uint32_t ring_frames, uint32_t channels, uint64_t first,
                                          const float *src, uint32_t frames, float gain)
{
    return switch_audio_ring_mix(ring, ring_frames, channels, first, src, frames, gain);
}

float SWITCH_AUDIO_HARNESS(h_s32_to_float)(int32_t v)
{
    return switch_audio_s32_to_float(v);
}

int SWITCH_AUDIO_HARNESS(h_neon)(void)
{
#if defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}
