/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The plug-in's sample conversions between CoreAudio's float32 and the rings' S32_LE (../SwitchAudioAbi.h), shared with
 * the host test (../test/switch_audio_format_test.cpp through ../test/switch_audio_format_harness.c).
 *
 * Mixing adds an IO cycle's output, times the volume control's gain, into what the ring holds, saturating at full
 * scale, so two IO contexts on the device would sum (the service clears each period it plays). Full scale is 2^31:
 * +1.0 saturates to INT32_MAX, -1.0 is INT32_MIN, NaN is 0 (FCVTZS). With NEON one FMUL, one FCVTZS #31 (scale,
 * truncate and saturate in one) and one SQADD per four samples: about 4.5 Cortex-A57 cycles per stereo frame by
 * llvm-mca, against about 36 for an int16 loop (an FDIV per sample to read the ring back). */
#ifndef SWITCH_AUDIO_CONVERT_H
#define SWITCH_AUDIO_CONVERT_H
#include <stdint.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* dst[i] = saturate(dst[i] + src[i] * gain * 2^31), i < samples. */
static inline void switch_audio_mix_s32(int32_t *dst, const float *src, uint32_t samples, float gain)
{
    uint32_t i = 0;
#if defined(__ARM_NEON)
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= samples; i += 4) {
        const int32x4_t s = vcvtq_n_s32_f32(vmulq_f32(vld1q_f32(src + i), g), 31);
        vst1q_s32(dst + i, vqaddq_s32(vld1q_s32(dst + i), s));
    }
    for (; i < samples; ++i) {
        const int32x2_t s = vcvt_n_s32_f32(vdup_n_f32(src[i] * gain), 31);
        dst[i] = vget_lane_s32(vqadd_s32(vdup_n_s32(dst[i]), s), 0);
    }
#else
    for (; i < samples; ++i) {
        const float v = src[i] * gain * 2147483648.0f;
        const int64_t s = v != v ? 0 : v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int64_t)v;
        const int64_t sum = (int64_t)dst[i] + s;
        dst[i] = (int32_t)(sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : sum);
    }
#endif
}

/* An IO cycle's interleaved output (frames of channels, sample time first) mixed into a ring of ring_frames frames,
 * in the two runs either side of the ring's end; at most one ring's worth (the rest would only overwrite). Returns the
 * frames in the first run (from frame first % ring_frames; the rest start at frame 0), for the caller's cache clean. */
static inline uint32_t switch_audio_ring_mix(int32_t *ring, uint32_t ring_frames, uint32_t channels, uint64_t first,
                                             const float *src, uint32_t frames, float gain)
{
    const uint32_t total = frames < ring_frames ? frames : ring_frames;
    const uint32_t at = (uint32_t)(first % ring_frames);
    const uint32_t run = total < ring_frames - at ? total : ring_frames - at;
    switch_audio_mix_s32(ring + (uint64_t)channels * at, src, channels * run, gain);
    if (run < total)
        switch_audio_mix_s32(ring, src + (uint64_t)channels * run, channels * (total - run), gain);
    return run;
}

/* Four-channel frames (left, left, right, right: J181's speaker) mixed into stereo ones: channels 0 and 2, as
 * switch_audio_mix_s32 mixes them. */
static inline void switch_audio_fold_s32(int32_t *dst, const float *src, uint32_t frames, float gain)
{
    uint32_t i = 0;
#if defined(__ARM_NEON)
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= frames; i += 4) {
        const float32x4x4_t f = vld4q_f32(src + 4 * i);
        int32x4x2_t d = vld2q_s32(dst + 2 * i);
        d.val[0] = vqaddq_s32(d.val[0], vcvtq_n_s32_f32(vmulq_f32(f.val[0], g), 31));
        d.val[1] = vqaddq_s32(d.val[1], vcvtq_n_s32_f32(vmulq_f32(f.val[2], g), 31));
        vst2q_s32(dst + 2 * i, d);
    }
#endif
    for (; i < frames; ++i) {
        switch_audio_mix_s32(dst + 2 * i, src + 4 * i, 1, gain);
        switch_audio_mix_s32(dst + 2 * i + 1, src + 4 * i + 2, 1, gain);
    }
}

/* switch_audio_ring_mix of four-channel frames into the stereo ring (switch_audio_fold_s32). */
static inline uint32_t switch_audio_ring_fold(int32_t *ring, uint32_t ring_frames, uint64_t first, const float *src,
                                              uint32_t frames, float gain)
{
    const uint32_t total = frames < ring_frames ? frames : ring_frames;
    const uint32_t at = (uint32_t)(first % ring_frames);
    const uint32_t run = total < ring_frames - at ? total : ring_frames - at;
    switch_audio_fold_s32(ring + 2 * (uint64_t)at, src, run, gain);
    if (run < total)
        switch_audio_fold_s32(ring, src + 4 * (uint64_t)run, total - run, gain);
    return run;
}

/* A captured S32 sample as float32 (exact for the codec's 24 bits). */
static inline float switch_audio_s32_to_float(int32_t v)
{
    return (float)v * 0x1p-31f;
}

#endif
