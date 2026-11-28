// SPDX-License-Identifier: BSD-2-Clause-Patent
// The gyroscope offset SwitchMotionFusion subtracts, learned while the console rests (SwitchMotionFusion.h).
//
// A 6-axis part cannot tell a turn about the vertical from an offset along gravity. A console on a turntable, a swivel
// chair or a stand being panned reads like a console at rest whose gyroscope is off by the turn's rate, at any rate and
// for any length of time. It has no vibration, and its centripetal acceleration (0.13 mg at 0.05 rad/s half a metre from
// the axis) is under the parts' 0.24 mg LSB. So a still window teaches only the offset perpendicular to its gravity: the
// gyroscope's mean less the tilt the accelerometer saw over the window. The component along gravity is learned at
// another attitude, where gravity lies elsewhere in the device frame. A console that only ever rests one way up keeps
// that one component unlearned, and yaw drifts at its offset while it lies that way.
//
// A still window is half a second in which every gyroscope sample stays within 0.01 rad/s of the window's mean (about
// ten times the parts' noise) and every accelerometer axis within 10 mg of the window's first sample, at 1 g within
// 0.05 g. The first offset is learned from two such windows in a row that agree within 5 mrad/s, the component along
// the first rest's gravity from four more at an attitude 30 degrees or more away (whose solve doubles the noise at 30
// degrees). Each such acquisition proposes against the bias at its first window, so the gravity correction's integral,
// which moves the bias meanwhile, does not leak into it; and it takes the tilt its accelerometer saw from its first
// window's mean to its last one's, where that change stands out of its own noise (its square past nine times its
// variance): at rest a tilt estimate is the accelerometer's noise, several times the gyroscope mean's, and the longer
// baseline makes it a fraction of one window's. After that each window moves the offset a sixteenth of the way (a
// quarter while that component is missing), at most 1.25 mrad/s per window, less the tilt from the window before it,
// again only where it stands out; so a smoothly changing linear acceleration (which a tilt estimate takes for a tilt)
// moves it little. A tilt slower than those gates is learned as offset: up to about 0.6 mrad/s at 200 Hz and 1.2 mrad/s
// at 12.5 Hz (switch_motion_bias_test's slow tilts), which the next rest's tracking takes back. A window that turns
// about the vertical faster than any offset the part can have is a turn, and is skipped: its centripetal acceleration
// tilts the measured gravity and leaks the turn into the perpendicular part.
//
// A known limit: a slower steady turn under a steady sideways acceleration, such as a car's curve, is still by these
// rules. The acceleration tilts the measured gravity by a/g, and ω·sin(a/g) is learned as offset: about 9 mrad/s in 20 s
// on a 0.06 rad/s curve at 25 m/s. The gravity correction's integral (SwitchMotionFusion.h) settles on the same offset
// more slowly, and the next real rest pulls both back. Nothing a 6-axis part reads tells that curve from a rest.
#pragma once
#include <stdint.h>
#include <string.h>
#include "SwitchMotionMath.h"

struct SwitchGyroBias {
    static constexpr int64_t kStillMicroRad = 10000;
    static constexpr int32_t kStillMicroG = 10000;
    static constexpr int64_t kStillNearOneG = 50000;
    static constexpr uint32_t kWindowMicros = 500000;
    static constexpr int64_t kAgreeNanoRad = 5000000;
    static constexpr int64_t kStepNanoRad = 1250000;
    static constexpr int64_t kSecondAttitude = kSwitchOne / 4;  // sin^2(30 degrees)
    static constexpr unsigned kFirstWindows = 2;
    static constexpr unsigned kSecondWindows = 4;

    enum class Learned : uint8_t {
        Nothing,
        AllButGravity,  // all but the component along `unlearned`, the first rest's gravity
        All,
    };

    Learned learned = Learned::Nothing;
    int64_t unlearned[3] = {};  // Q30 unit vector

    void restart()
    {
        window = {};
        acquisition = {};
        previous = {};
    }

    // One gyroscope sample (micro-rad/s) standing for `dtMicros`, with the latest accelerometer sample (micro-g) and its
    // magnitude. `bias` (nano-rad/s) moves, each axis within +-`limit`.
    void add(const int32_t gyro[3], uint32_t dtMicros, const int32_t accel[3], int64_t accelNorm, int64_t bias[3],
             int64_t limit)
    {
        if (window.samples && (!steady(accel, accelNorm) || !nearMean(gyro)))
            restart();
        if (!window.samples) {
            memcpy(window.firstAccel, accel, sizeof(window.firstAccel));
            if (!steady(accel, accelNorm))
                return;
        }
        window.micros += dtMicros;
        const unsigned half = window.micros <= kWindowMicros / 2 ? 0 : 1;
        for (unsigned i = 0; i < 3; ++i) {
            window.gyroSum[i] += gyro[i];
            window.accelSum[half][i] += accel[i];
            window.accelSquares[half][i] += int64_t{accel[i]} * accel[i];
        }
        window.timeSum[half] += window.micros;
        ++window.count[half];
        ++window.samples;
        if (window.micros < kWindowMicros)
            return;
        const Window done = window;
        window = {};
        if (done.count[0] < 2 || done.count[1] < 2) {
            acquisition = {};
            previous = {};
            return;
        }
        Measured measured;
        measure(done, measured);
        stillWindow(measured, bias, limit);
        previous.valid = true;
        memcpy(previous.accelSum, measured.accelSum, sizeof(previous.accelSum));
        previous.samples = measured.samples;
        previous.sinceCenter = measured.micros - measured.center;
        memcpy(previous.spread, measured.spread, sizeof(previous.spread));
    }

private:
    struct Window {
        int64_t gyroSum[3];
        int64_t accelSum[2][3];  // each half of the window
        int64_t accelSquares[2][3];
        int64_t timeSum[2];  // micros into the window
        uint32_t count[2];
        int32_t firstAccel[3];
        uint32_t samples, micros;
    };

    // A still window: its gyroscope mean and the tilt its halves saw (micro-rad/s), whether that tilt stands out of the
    // accelerometer's noise, its gravity direction (Q30) and magnitude (micro-g), its accelerometer sum (micro-g), the
    // spread of its samples about each half's mean (micro-g squared), and when its samples came (micros into it).
    struct Measured {
        int64_t gyro[3];
        int64_t tilt[3];
        bool tiltSeen;
        int64_t down[3];
        int64_t norm;
        int64_t accelSum[3];
        int64_t spread[3];
        uint32_t samples;
        uint32_t micros;
        int64_t center;
    };

    // Still windows in a row whose proposed offsets agree. Each proposes against `reference`, the bias at the first of
    // them: the gravity correction's integral moves the bias while they come (SwitchMotionFusion.h), and they agree on
    // the offset alone. Their tilt is taken from the first one's accelerometer mean to the last one's, a baseline the
    // windows' own halves are a fraction of.
    struct Acquisition {
        unsigned windows;
        int64_t reference[3];
        int64_t candidate[3];  // the first window's proposal, nano-rad/s
        int64_t gyro[3];       // the windows' gyroscope means, summed
        int64_t down[3];       // the first window's gravity
        int64_t norm;
        int64_t firstAccel[3], lastAccel[3];
        uint32_t firstSamples, lastSamples;
        int64_t firstCenter, lastCenter;  // micros from the first window's start
        int64_t spread[3];
        int64_t dof;
        int64_t elapsed;
    };

    // The still window just before this one, when there was one: tracking takes its tilt from that window's
    // accelerometer mean to this one's.
    struct Previous {
        bool valid;
        int64_t accelSum[3];
        uint32_t samples;
        int64_t sinceCenter;  // micros from its samples' mean time to its end
        int64_t spread[3];
    };

    Window window = {};
    Acquisition acquisition = {};
    Previous previous = {};

    static int64_t dot(const int64_t a[3], const int64_t b[3])
    {
        return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) >> kSwitchQ;
    }

    static bool within(const int64_t a[3], const int64_t b[3], int64_t bound)
    {
        for (unsigned i = 0; i < 3; ++i) {
            if (a[i] - b[i] > bound || b[i] - a[i] > bound)
                return false;
        }
        return true;
    }

    static int64_t clamp(int64_t value, int64_t bound)
    {
        if (value > bound)
            return bound;
        return value < -bound ? -bound : value;
    }

    bool steady(const int32_t accel[3], int64_t norm) const
    {
        if (norm < 1000000 - kStillNearOneG || norm > 1000000 + kStillNearOneG)
            return false;
        for (unsigned i = 0; i < 3; ++i) {
            const int32_t moved = accel[i] - window.firstAccel[i];
            if (moved > kStillMicroG || moved < -kStillMicroG)
                return false;
        }
        return true;
    }

    bool nearMean(const int32_t gyro[3]) const
    {
        for (unsigned i = 0; i < 3; ++i) {
            const int64_t mean = window.gyroSum[i] / window.samples;
            if (gyro[i] - mean > kStillMicroRad || mean - gyro[i] > kStillMicroRad)
                return false;
        }
        return true;
    }

    // Whether a change of the accelerometer's mean (micro-g) between n0 samples and n1 is a tilt rather than noise: its
    // square past nine times its variance, which the samples' own spread (`dof` degrees of freedom) gives.
    static bool significant(const int64_t change[3], const int64_t spread[3], int64_t dof, int64_t n0, int64_t n1)
    {
        int64_t variance = 0;
        int64_t moved = 0;
        for (unsigned i = 0; i < 3; ++i) {
            variance += spread[i] / dof;
            moved += change[i] * change[i];
        }
        return moved > 9 * (variance * (n0 + n1) / (n0 * n1));
    }

    // The rotation (micro-rad/s) that turns the measured down direction by `change` (micro-g, of gravity `norm` micro-g
    // long) over `micros`: down turns as dg/dt = -w x g, so the rotation perpendicular to it is (dg/dt) x g.
    static void turnRate(const int64_t change[3], const int64_t down[3], int64_t norm, int64_t micros, int64_t rate[3])
    {
        int64_t turned[3];
        for (unsigned i = 0; i < 3; ++i)
            turned[i] = change[i] * kSwitchOne / norm;
        const int64_t tilt[3] = {
            (turned[1] * down[2] - turned[2] * down[1]) >> kSwitchQ,
            (turned[2] * down[0] - turned[0] * down[2]) >> kSwitchQ,
            (turned[0] * down[1] - turned[1] * down[0]) >> kSwitchQ,
        };
        for (unsigned i = 0; i < 3; ++i)
            rate[i] = (tilt[i] * 1000000 / micros) * 1000000 >> kSwitchQ;
    }

    static void measure(const Window &w, Measured &out)
    {
        int64_t mean[3];
        int64_t change[3];
        for (unsigned i = 0; i < 3; ++i) {
            out.accelSum[i] = w.accelSum[0][i] + w.accelSum[1][i];
            mean[i] = out.accelSum[i] / w.samples;
            change[i] = w.accelSum[1][i] / w.count[1] - w.accelSum[0][i] / w.count[0];
            out.spread[i] = 0;
            for (unsigned h = 0; h < 2; ++h) {
                const int64_t squares = w.accelSquares[h][i] - w.accelSum[h][i] * w.accelSum[h][i] / w.count[h];
                out.spread[i] += squares > 0 ? squares : 0;
            }
            out.gyro[i] = w.gyroSum[i] / w.samples;
        }
        out.tiltSeen = significant(change, out.spread, w.samples - 2, w.count[0], w.count[1]);
        out.norm = static_cast<int64_t>(
            switchIsqrt(static_cast<uint64_t>(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2])));
        for (unsigned i = 0; i < 3; ++i)
            out.down[i] = mean[i] * kSwitchOne / out.norm;
        turnRate(change, out.down, out.norm, w.timeSum[1] / w.count[1] - w.timeSum[0] / w.count[0], out.tilt);
        out.samples = w.samples;
        out.micros = w.micros;
        out.center = (w.timeSum[0] + w.timeSum[1]) / w.samples;
    }

    bool acquiring(const int64_t down[3]) const
    {
        if (learned == Learned::Nothing)
            return true;
        const int64_t cosine = dot(unlearned, down);
        return learned == Learned::AllButGravity && kSwitchOne - ((cosine * cosine) >> kSwitchQ) >= kSecondAttitude;
    }

    // The part of the rate's miss against `base` (micro-rad/s) perpendicular to gravity.
    static void perpendicular(const int64_t rate[3], const int64_t down[3], const int64_t base[3], int64_t across[3])
    {
        int64_t miss[3];
        for (unsigned i = 0; i < 3; ++i)
            miss[i] = rate[i] - base[i] / 1000;
        const int64_t missAlong = dot(miss, down);
        for (unsigned i = 0; i < 3; ++i)
            across[i] = miss[i] - ((missAlong * down[i]) >> kSwitchQ);
    }

    // The offset a rate at rest proposes against `base`: perpendicular to its gravity what it measured, the rest
    // `base`'s; at the second attitude only `unlearned`'s component is the rate's (the miss perpendicular to gravity is
    // that component times the part of `unlearned` gravity sees).
    void propose(const int64_t rate[3], const int64_t down[3], const int64_t base[3], int64_t proposed[3]) const
    {
        int64_t across[3];
        perpendicular(rate, down, base, across);
        if (learned == Learned::Nothing) {
            for (unsigned i = 0; i < 3; ++i)
                proposed[i] = base[i] + across[i] * 1000;
            return;
        }
        const int64_t cosine = dot(unlearned, down);
        const int64_t sine2 = kSwitchOne - ((cosine * cosine) >> kSwitchQ);
        const int64_t component = dot(across, unlearned) * kSwitchOne / sine2;
        for (unsigned i = 0; i < 3; ++i)
            proposed[i] = base[i] + ((component * unlearned[i]) >> kSwitchQ) * 1000;
    }

    void stillWindow(const Measured &m, int64_t bias[3], int64_t limit)
    {
        const bool acquire = acquiring(m.down);
        // An acquisition takes its tilt over all its windows (finish), so its windows keep theirs.
        int64_t tilt[3] = {};
        if (!acquire)
            trackedTilt(m, tilt);
        int64_t rate[3];
        for (unsigned i = 0; i < 3; ++i)
            rate[i] = m.gyro[i] - tilt[i];
        const int64_t along = dot(rate, m.down);
        if (along > limit / 1000 || along < -limit / 1000) {
            acquisition = {};
            return;
        }
        if (acquire) {
            gather(m, rate, bias, limit);
            return;
        }
        acquisition = {};
        int64_t across[3];
        perpendicular(rate, m.down, bias, across);
        const int64_t gain = learned == Learned::All ? 16 : 4;
        for (unsigned i = 0; i < 3; ++i)
            bias[i] = clamp(bias[i] + clamp(across[i] * 1000 / gain, kStepNanoRad), limit);
    }

    // A tracking window's tilt, where it stands out of the noise: from the still window before it to it when there is
    // one, a baseline twice its own halves' and so a third of their noise, else between its halves.
    void trackedTilt(const Measured &m, int64_t tilt[3]) const
    {
        if (!previous.valid) {
            if (m.tiltSeen)
                memcpy(tilt, m.tilt, sizeof(m.tilt));
            return;
        }
        int64_t change[3];
        int64_t spread[3];
        for (unsigned i = 0; i < 3; ++i) {
            change[i] = m.accelSum[i] / m.samples - previous.accelSum[i] / previous.samples;
            spread[i] = m.spread[i] + previous.spread[i];
        }
        if (significant(change, spread, m.samples + previous.samples - 4, previous.samples, m.samples))
            turnRate(change, m.down, m.norm, previous.sinceCenter + m.center, tilt);
    }

    void gather(const Measured &m, const int64_t rate[3], int64_t bias[3], int64_t limit)
    {
        Acquisition &a = acquisition;
        int64_t proposed[3];
        if (a.windows) {
            propose(rate, m.down, a.reference, proposed);
            if (!within(proposed, a.candidate, kAgreeNanoRad))
                a = {};
        }
        if (!a.windows) {
            memcpy(a.reference, bias, sizeof(a.reference));
            propose(rate, m.down, a.reference, a.candidate);
            memcpy(a.down, m.down, sizeof(a.down));
            a.norm = m.norm;
            memcpy(a.firstAccel, m.accelSum, sizeof(a.firstAccel));
            a.firstSamples = m.samples;
            a.firstCenter = m.center;
        }
        memcpy(a.lastAccel, m.accelSum, sizeof(a.lastAccel));
        a.lastSamples = m.samples;
        a.lastCenter = a.elapsed + m.center;
        a.elapsed += m.micros;
        for (unsigned i = 0; i < 3; ++i) {
            a.gyro[i] += rate[i];
            a.spread[i] += m.spread[i];
        }
        a.dof += m.samples - 2;
        ++a.windows;
        if (a.windows >= (learned == Learned::Nothing ? kFirstWindows : kSecondWindows))
            finish(bias, limit);
    }

    // The acquisition's offset: its windows' mean rate, less the tilt between its first and last windows when that
    // stands out of their noise, against its reference.
    void finish(int64_t bias[3], int64_t limit)
    {
        const Acquisition &a = acquisition;
        int64_t mean[3];
        int64_t change[3];
        for (unsigned i = 0; i < 3; ++i) {
            mean[i] = a.gyro[i] / a.windows;
            change[i] = a.lastAccel[i] / a.lastSamples - a.firstAccel[i] / a.firstSamples;
        }
        if (significant(change, a.spread, a.dof, a.firstSamples, a.lastSamples)) {
            int64_t tilt[3];
            turnRate(change, a.down, a.norm, a.lastCenter - a.firstCenter, tilt);
            for (unsigned i = 0; i < 3; ++i)
                mean[i] -= tilt[i];
        }
        int64_t proposed[3];
        propose(mean, a.down, a.reference, proposed);
        for (unsigned i = 0; i < 3; ++i)
            bias[i] = clamp(proposed[i], limit);
        if (learned == Learned::Nothing) {
            learned = Learned::AllButGravity;
            memcpy(unlearned, a.down, sizeof(unlearned));
        } else {
            learned = Learned::All;
        }
        acquisition = {};
    }
};
