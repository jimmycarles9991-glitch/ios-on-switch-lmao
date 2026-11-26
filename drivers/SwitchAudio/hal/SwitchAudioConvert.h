/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Sample conversions between CoreAudio's float32 and the rings' S32_LE (../SwitchAudioAbi.h), shared with
 * the host test (../test/switch_audio_format_test.cpp through ../test/switch_audio_format_harness.c).
 *
 * Mixing adds an IO cycle's output, times gain, into what the ring holds, rounding to nearest-even and saturating
 * at full scale (+-2^31: +1.0 -> INT32_MAX, -1.0 -> INT32_MIN, NaN -> 0; FCVTNS). NEON and scalar paths match bit
 * for bit by multiplying by (gain * 2^31) in the same order before rounding. */
#ifndef SWITCH_AUDIO_CONVERT_H
#define SWITCH_AUDIO_CONVERT_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if !defined(__ARM_NEON)
static inline int32_t switch_audio_f32_to_s32_scalar(float v)
{
    if (v != v)
        return 0;
    if (v >= 2147483648.0f)
        return INT32_MAX;
    if (v <= -2147483648.0f)
        return INT32_MIN;
    const int64_t r = (int64_t)rintf(v);
    return (int32_t)(r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : r);
}
#endif

/* dst[i] = saturate(dst[i] + round_even(src[i] * (gain * 2^31))), i < samples. */
static inline void switch_audio_mix_s32(int32_t *dst, const float *src, uint32_t samples, float gain)
{
    const float scale = gain * 2147483648.0f;
    uint32_t i = 0;
#if defined(__ARM_NEON)
    const float32x4_t sv = vdupq_n_f32(scale);
    for (; i + 4 <= samples; i += 4) {
        const int32x4_t s = vcvtnq_s32_f32(vmulq_f32(vld1q_f32(src + i), sv));
        vst1q_s32(dst + i, vqaddq_s32(vld1q_s32(dst + i), s));
    }
    for (; i < samples; ++i) {
        const int32x2_t s = vcvtn_s32_f32(vdup_n_f32(src[i] * scale));
        dst[i] = vget_lane_s32(vqadd_s32(vdup_n_s32(dst[i]), s), 0);
    }
#else
    for (; i < samples; ++i) {
        const int64_t s = switch_audio_f32_to_s32_scalar(src[i] * scale);
        const int64_t sum = (int64_t)dst[i] + s;
        dst[i] = (int32_t)(sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : sum);
    }
#endif
}

static inline void switch_audio_write_s32(int32_t *dst, const float *src, uint32_t samples)
{
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= samples; i += 4)
        vst1q_s32(dst + i, vcvtq_n_s32_f32(vld1q_f32(src + i), 31));
#endif
    for (; i < samples; ++i) {
        const float v = src[i] * 0x1p31f;
        dst[i] = v != v ? 0 : v >= 0x1p31f ? INT32_MAX : v <= -0x1p31f ? INT32_MIN : (int32_t)v;
    }
}

static inline void switch_audio_write_s16(int16_t *dst, const float *src, uint32_t samples)
{
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= samples; i += 4) {
        const int16x4_t sample = vqmovn_s32(vcvtq_n_s32_f32(vld1q_f32(src + i), 15));
        vst1_s16(dst + i, sample);
    }
#endif
    for (; i < samples; ++i) {
        const float value = src[i] * 32768.0f;
        const int32_t sample = value != value       ? 0
                               : value >= 32767.0f  ? INT16_MAX
                               : value <= -32768.0f ? INT16_MIN
                                                    : (int32_t)value;
        dst[i] = (int16_t)sample;
    }
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
    const float32x4_t sv = vdupq_n_f32(gain * 2147483648.0f);
    for (; i + 4 <= frames; i += 4) {
        const float32x4x4_t f = vld4q_f32(src + 4 * i);
        int32x4x2_t d = vld2q_s32(dst + 2 * i);
        d.val[0] = vqaddq_s32(d.val[0], vcvtnq_s32_f32(vmulq_f32(f.val[0], sv)));
        d.val[1] = vqaddq_s32(d.val[1], vcvtnq_s32_f32(vmulq_f32(f.val[2], sv)));
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

static inline void switch_audio_s32_to_float_buf(float *dst, const int32_t *src, uint32_t samples)
{
    uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 4 <= samples; i += 4)
        vst1q_f32(dst + i, vcvtq_n_f32_s32(vld1q_s32(src + i), 31));
#endif
    for (; i < samples; ++i)
        dst[i] = switch_audio_s32_to_float(src[i]);
}

static inline void switch_audio_ring_read_s32(float *dst, const int32_t *ring, uint32_t ring_frames, uint64_t first,
                                              uint32_t frames, uint64_t anchor, uint64_t in_frames)
{
    if (!frames)
        return;
    if (anchor == ~(uint64_t)0 || in_frames <= anchor || !ring_frames || first >= in_frames ||
        (anchor > first && anchor - first >= frames)) {
        memset(dst, 0, (size_t)frames * sizeof(float));
        return;
    }
    const uint32_t lead = first < anchor ? (uint32_t)(anchor - first) : 0u;
    const uint64_t start = first + lead;
    const uint32_t rem = frames - lead;
    const uint64_t avail = in_frames - start;
    const uint32_t count = avail < rem ? (uint32_t)avail : rem;
    const uint32_t trail = rem - count;
    if (lead)
        memset(dst, 0, (size_t)lead * sizeof(float));
    uint32_t at = (uint32_t)((start - anchor) % ring_frames);
    uint32_t done = 0;
    while (done < count) {
        const uint32_t left = count - done;
        const uint32_t run = left < ring_frames - at ? left : ring_frames - at;
        switch_audio_s32_to_float_buf(dst + lead + done, ring + at, run);
        done += run;
        at = 0;
    }
    if (trail)
        memset(dst + lead + count, 0, (size_t)trail * sizeof(float));
}

#endif
