// SPDX-License-Identifier: BSD-2-Clause-Patent
// Frame-level rate control for an encoder run at a constant QP per picture (AVE on NVENC): the firmware's own
// picture-level RC is another undocumented surface on NVENC 5.0, so the plug-in picks each picture's QP itself from
// the bits the last ones took. Header-only, plain C, so the host tests drive it with a synthetic bits model.
//
// Modes: bit rate (VBR: AverageBitRate, a 1 s virtual buffer), constant bit rate (ConstantBitRate, a 0.5 s buffer and
// faster correction), or constant quality (Quality 0..1, or neither set: 0.6). DataRateLimits caps the bytes in any
// window of the given length on top of either bit-rate mode. Key frames are coded 2 QP finer than P pictures, B
// pictures 3 coarser (NVIDIA's T210 driver's constant QPs: I 23, P 25, B 28).
#ifndef AVE_RATE_CONTROL_H
#define AVE_RATE_CONTROL_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define RC_LIMIT_HISTORY 256

typedef struct {
    double bitrate, fps, buffer, fullness;  // bits/s, frames/s, bits, bits (above target is positive)
    double limitBytes, limitSeconds;        // DataRateLimits (0: none)
    int qp, qpMin, qpMax, constQP;          // constQP >= 0: constant quality, no bit-rate control
    bool cbr;
    double sizes[RC_LIMIT_HISTORY];  // last pictures' bytes, for the data-rate window
    unsigned count;
} RateControl;

static inline int rc_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// bitrate 0: constant quality (`quality` in 0..1, or < 0 for the default).
static inline void rc_init(RateControl *rc, double bitrate, bool cbr, double quality, double fps, uint32_t width,
                           uint32_t height)
{
    *rc = (RateControl){0};
    rc->fps = fps > 0 ? fps : 30;
    rc->qpMin = 10;
    rc->qpMax = 48;
    rc->cbr = cbr;
    if (bitrate <= 0) {
        const double q = quality >= 0 && quality <= 1 ? quality : 0.6;
        rc->constQP = rc_clamp((int)lround(40 - 22 * q), 10, 48);
        rc->qp = rc->constQP;
        return;
    }
    rc->constQP = -1;
    rc->bitrate = bitrate;
    rc->buffer = bitrate * (cbr ? 0.5 : 1.0);
    // Start from bits per pixel: about QP 28 at 0.1 bpp, 6 QP per halving.
    const double bpp = bitrate / rc->fps / ((double)width * height);
    rc->qp = rc_clamp((int)lround(28 - 6 * log2(bpp / 0.1)), 16, 44);
}

static inline void rc_set_limit(RateControl *rc, double bytes, double seconds)
{
    rc->limitBytes = bytes > 0 && seconds > 0 ? bytes : 0;
    rc->limitSeconds = rc->limitBytes ? seconds : 0;
}

static inline int rc_qp(const RateControl *rc, bool key)
{
    return rc_clamp(rc->qp - (key ? 2 : 0), 0, 51);
}

static inline int rc_qp_b(const RateControl *rc)
{
    return rc_clamp(rc->qp + 3, 0, 51);
}

// Bytes in the last `seconds` of pictures (at the nominal frame rate).
static inline double rc_window(const RateControl *rc, double seconds)
{
    unsigned n = (unsigned)ceil(seconds * rc->fps);
    if (n > rc->count)
        n = rc->count;
    if (n > RC_LIMIT_HISTORY)
        n = RC_LIMIT_HISTORY;
    double sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += rc->sizes[(rc->count - 1 - i) % RC_LIMIT_HISTORY];
    return sum;
}

static inline void rc_update(RateControl *rc, double bytes, bool key)
{
    rc->sizes[rc->count++ % RC_LIMIT_HISTORY] = bytes;
    if (rc->constQP >= 0 && !rc->limitBytes)
        return;
    int step = 0;
    if (rc->constQP < 0) {
        const double target = rc->bitrate / rc->fps;
        // A key picture is expected to cost about four P pictures: only the excess counts against the buffer.
        rc->fullness += bytes * 8 - target * (key ? 4 : 1);
        if (rc->fullness < -rc->buffer)
            rc->fullness = -rc->buffer;
        const double level = rc->fullness / rc->buffer;
        step = level > 0.5 ? 2 : level > (rc->cbr ? 0.1 : 0.25) ? 1 : level < -0.5 ? -2 : level < -0.25 ? -1 : 0;
    }
    if (rc->limitBytes && rc_window(rc, rc->limitSeconds) > 0.9 * rc->limitBytes)
        step = step > 2 ? step : 2;
    const int base = rc->constQP >= 0 ? rc->constQP : rc->qpMin;
    rc->qp = rc_clamp(rc->qp + step, base, rc->qpMax);
    if (rc->constQP >= 0 && step == 0 && rc->qp > rc->constQP)
        rc->qp--;  // back to the asked quality once under
}

#endif
