// switch_audio_format_harness.c's exports, once per build of it: neon_* (NEON, as on the device) and scalar_*
// (__ARM_NEON undefined, the scalar fallback). The harness defines the set its SWITCH_AUDIO_HARNESS_PREFIX names.
#pragma once
#include <stdint.h>

#define SWITCH_AUDIO_HARNESS_CAT2(a, b) a##b
#define SWITCH_AUDIO_HARNESS_CAT(a, b)  SWITCH_AUDIO_HARNESS_CAT2(a, b)
#ifdef SWITCH_AUDIO_HARNESS_PREFIX
#define SWITCH_AUDIO_HARNESS(name) SWITCH_AUDIO_HARNESS_CAT(SWITCH_AUDIO_HARNESS_PREFIX, name)
#endif

#define SWITCH_AUDIO_HARNESS_DECLARE(prefix)                                                                           \
    uint32_t prefix##h_cif(uint32_t threshold, uint32_t audio_channels, uint32_t client_channels, uint32_t audio_bits, \
                           uint32_t client_bits);                                                                      \
    uint32_t prefix##h_admaif_cif(uint32_t channels);                                                                  \
    uint32_t prefix##h_ahub_cif(uint32_t channels);                                                                    \
    uint32_t prefix##h_i2s_rx_cif(void);                                                                               \
    uint32_t prefix##h_i2s_tx_cif(void);                                                                               \
    uint32_t prefix##h_i2s_ctrl(void);                                                                                 \
    uint32_t prefix##h_i2s_timing(void);                                                                               \
    uint32_t prefix##h_i2s_divider(uint32_t pllaout0_per_fs);                                                          \
    uint32_t prefix##h_bclk_fs(void);                                                                                  \
    uint32_t prefix##h_codec_bits(void);                                                                               \
    uint32_t prefix##h_rt5640_sdp_mask(void);                                                                          \
    uint32_t prefix##h_rt5640_sdp(void);                                                                               \
    uint32_t prefix##h_rt5640_adda_mask(void);                                                                         \
    uint32_t prefix##h_rt5640_adda(void);                                                                              \
    uint64_t prefix##h_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff,  \
                                           uint64_t out0_x2, uint64_t pllaout0_per_fs);                                \
    uint64_t prefix##h_rate_millihz(uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff, uint64_t out0_x2,         \
                                    uint64_t pllaout0_per_fs);                                                         \
    void prefix##h_clock(uint32_t index, uint32_t fields[7]);                                                          \
    uint64_t prefix##h_clock_n_eff(uint32_t index);                                                                    \
    uint64_t prefix##h_clock_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint32_t index);                   \
    uint64_t prefix##h_client_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint32_t index,                   \
                                                  uint32_t client_rate);                                               \
    uint64_t prefix##h_frames_from_ticks_q32(uint64_t ticks, uint64_t ticks_per_frame_q32);                            \
    uint64_t prefix##h_clock_millihz(uint64_t ref_hz, uint32_t index);                                                 \
    uint32_t prefix##h_extern1_divider(uint32_t pllaout0_per_fs);                                                      \
    int32_t prefix##h_sfc_rate_index(uint32_t rate);                                                                   \
    int prefix##h_is_44k_family(uint32_t rate);                                                                        \
    int32_t prefix##h_mvc_linear_db_q8(int32_t millidb);                                                               \
    uint32_t prefix##h_mbdrc_pack_thresh(uint8_t t0, uint8_t t1, uint8_t t2, uint8_t t3);                              \
    uint32_t prefix##h_mbdrc_fast_factor(uint16_t release_q15, uint16_t attack_q15);                                   \
    void prefix##h_hp_stage_split(uint32_t attenuation_mdb, uint8_t *hp_vol_reg, int32_t *mvc_mdb);                    \
    uint32_t prefix##h_transition(uint32_t was, uint32_t now);                                                         \
    int64_t prefix##h_capture_frame(uint64_t t, uint64_t anchor, uint64_t in_frames, uint32_t ring_frames);            \
    void prefix##h_mix(int32_t *dst, const float *src, uint32_t samples, float gain);                                  \
    uint32_t prefix##h_ring_mix(int32_t *ring, uint32_t ring_frames, uint32_t channels, uint64_t first,                \
                                const float *src, uint32_t frames, float gain);                                        \
    void prefix##h_fold(int32_t *dst, const float *src, uint32_t frames, float gain);                                  \
    uint32_t prefix##h_ring_fold(int32_t *ring, uint32_t ring_frames, uint64_t first, const float *src,                \
                                 uint32_t frames, float gain);                                                         \
    float prefix##h_s32_to_float(int32_t v);                                                                           \
    void prefix##h_s32_to_float_buf(float *dst, const int32_t *src, uint32_t samples);                                 \
    void prefix##h_ring_read_s32(float *dst, const int32_t *ring, uint32_t ring_frames, uint64_t first,                \
                                 uint32_t frames, uint64_t anchor, uint64_t in_frames);                                \
    int prefix##h_neon(void);

#ifdef __cplusplus
extern "C" {
#endif

SWITCH_AUDIO_HARNESS_DECLARE(neon_)
SWITCH_AUDIO_HARNESS_DECLARE(scalar_)

#ifdef __cplusplus
}
#endif
