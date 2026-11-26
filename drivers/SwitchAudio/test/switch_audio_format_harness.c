// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for switch_audio_format_test.cpp: SwitchAudioFormat.h and hal/SwitchAudioConvert.h compiled as C,
// exported twice (neon_* and scalar_*) to verify bit-exact parity.
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

uint32_t SWITCH_AUDIO_HARNESS(h_ahub_cif)(uint32_t channels)
{
    return switch_audio_ahub_cif(channels);
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

uint64_t SWITCH_AUDIO_HARNESS(h_client_ticks_per_frame_q32)(uint64_t host_hz, uint64_t ref_hz, uint32_t index,
                                                            uint32_t client_rate)
{
    return switch_audio_client_ticks_per_frame_q32(host_hz, ref_hz, clock_at(index), client_rate);
}

uint64_t SWITCH_AUDIO_HARNESS(h_frames_from_ticks_q32)(uint64_t ticks, uint64_t ticks_per_frame_q32)
{
    return switch_audio_frames_from_ticks_q32(ticks, ticks_per_frame_q32);
}

uint64_t SWITCH_AUDIO_HARNESS(h_clock_millihz)(uint64_t ref_hz, uint32_t index)
{
    return switch_audio_clock_millihz(ref_hz, clock_at(index));
}

uint32_t SWITCH_AUDIO_HARNESS(h_extern1_divider)(uint32_t pllaout0_per_fs)
{
    return switch_audio_extern1_divider(pllaout0_per_fs);
}

int32_t SWITCH_AUDIO_HARNESS(h_sfc_rate_index)(uint32_t rate)
{
    return switch_audio_sfc_rate_index(rate);
}

int SWITCH_AUDIO_HARNESS(h_is_44k_family)(uint32_t rate)
{
    return switch_audio_is_44k_family(rate);
}

int32_t SWITCH_AUDIO_HARNESS(h_mvc_linear_db_q8)(int32_t millidb)
{
    return switch_audio_mvc_linear_db_q8(millidb);
}

uint32_t SWITCH_AUDIO_HARNESS(h_mbdrc_pack_thresh)(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3)
{
    return switch_audio_mbdrc_pack_thresh(t0, t1, t2, t3);
}

uint32_t SWITCH_AUDIO_HARNESS(h_mbdrc_fast_factor)(uint16_t release_q15, uint16_t attack_q15)
{
    return switch_audio_mbdrc_fast_factor(release_q15, attack_q15);
}

void SWITCH_AUDIO_HARNESS(h_hp_stage_split)(uint32_t attenuation_mdb, uint8_t *hp_vol_reg, int32_t *mvc_mdb)
{
    switch_audio_hp_stage_split(attenuation_mdb, hp_vol_reg, mvc_mdb);
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

void SWITCH_AUDIO_HARNESS(h_fold)(int32_t *dst, const float *src, uint32_t frames, float gain)
{
    switch_audio_fold_s32(dst, src, frames, gain);
}

uint32_t SWITCH_AUDIO_HARNESS(h_ring_fold)(int32_t *ring, uint32_t ring_frames, uint64_t first, const float *src,
                                           uint32_t frames, float gain)
{
    return switch_audio_ring_fold(ring, ring_frames, first, src, frames, gain);
}

float SWITCH_AUDIO_HARNESS(h_s32_to_float)(int32_t v)
{
    return switch_audio_s32_to_float(v);
}

void SWITCH_AUDIO_HARNESS(h_s32_to_float_buf)(float *dst, const int32_t *src, uint32_t samples)
{
    switch_audio_s32_to_float_buf(dst, src, samples);
}

void SWITCH_AUDIO_HARNESS(h_ring_read_s32)(float *dst, const int32_t *ring, uint32_t ring_frames, uint64_t first,
                                           uint32_t frames, uint64_t anchor, uint64_t in_frames)
{
    switch_audio_ring_read_s32(dst, ring, ring_frames, first, frames, anchor, in_frames);
}

int SWITCH_AUDIO_HARNESS(h_neon)(void)
{
#if defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}
