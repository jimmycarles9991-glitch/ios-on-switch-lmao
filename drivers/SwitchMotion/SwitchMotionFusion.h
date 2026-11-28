// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device motion fused in SwitchMotion, as a real iPad's motion coprocessor delivers it: attitude, user acceleration and
// bias-corrected rotation rate in one report per sample (SwitchMotion.cpp file comment; SwitchHID.h has the report).
//
// Kexts build with -mgeneral-regs-only, so everything here is integer: the attitude is a Q30 quaternion and the
// report's floats are written as IEEE-754 bit patterns. Inputs are SwitchMotion's device-frame samples: acceleration as
// the gravity vector in micro-g (lying face up reads z = -1 g), angular velocity in micro-rad/s.
//
// The quaternion rotates device-frame vectors into CoreMotion's reference frame (Z up), so the gravity it implies in the
// device frame is -(2(xz - wy), 2(yz + wx), w^2 - x^2 - y^2 + z^2): identity lying face up, (0, -1, 0) held upright in
// portrait. Both are CoreMotion's own: 24B119 derives gravity from the report's quaternion with that formula and
// predicts the attitude forward as q * dq(rotation rate * dt), the increment in the device frame on the right.
// The filter is Mahony's: the measured gravity direction pulls the attitude through a proportional gain and a
// gyroscope bias integral, only while the acceleration is close to 1 g. Gravity says nothing about rotation about the
// vertical, so the bias is also learned from stillness (SwitchMotionBias.h), and it outlives a session: restart() resets
// only the attitude. The first accelerometer sample of a session seeds the tilt (yaw 0), as CoreMotion's
// xArbitraryZVertical frame allows.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "SwitchMotionBias.h"
#include "SwitchMotionMath.h"

// value / 2^fraction as an IEEE-754 single, rounded to nearest (ties to even).
inline uint32_t switchFloatBits(int64_t value, int fraction)
{
    if (value == 0)
        return 0;
    const uint32_t sign = value < 0 ? 0x80000000u : 0;
    uint64_t magnitude = value < 0 ? uint64_t{0} - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    int top = 63;
    while (!(magnitude >> top))
        --top;
    int exponent = top - fraction;
    uint64_t mantissa = 0;
    if (top > 23) {
        const int shift = top - 23;
        mantissa = magnitude >> shift;
        const uint64_t rest = magnitude & ((uint64_t{1} << shift) - 1);
        const uint64_t half = uint64_t{1} << (shift - 1);
        if (rest > half || (rest == half && (mantissa & 1)))
            ++mantissa;
        if (mantissa >> 24) {
            mantissa >>= 1;
            ++exponent;
        }
    } else {
        mantissa = magnitude << (23 - top);
    }
    return sign | static_cast<uint32_t>(exponent + 127) << 23 | (static_cast<uint32_t>(mantissa) & 0x7fffffu);
}

// A micro-unit (micro-g, micro-rad/s) as a float of the unit.
inline uint32_t switchMicroFloatBits(int64_t micro)
{
    return switchFloatBits(micro * (int64_t{1} << 24) / 1000000, 24);
}

// The time a gyroscope sample stands for: the gap since the previous one (0: none yet this session), within half and
// four times the sample period, so one late sample neither stretches nor vanishes and samples the part overwrote while
// the work loop was late still count.
inline uint32_t switchGyroGapMicros(uint64_t gapMicros, uint32_t periodMicros)
{
    if (!gapMicros)
        return periodMicros;
    const uint64_t low = periodMicros / 2;
    const uint64_t high = uint64_t{periodMicros} * 4;
    return static_cast<uint32_t>(gapMicros < low ? low : gapMicros > high ? high : gapMicros);
}

struct SwitchMotionFusion {
    // Proportional gain 1 rad/s per unit of direction error, bias integral 0.02 /s^2: a tilt error settles in about a
    // second and a gyroscope bias in tens of seconds at rest.
    static constexpr int64_t kKpMicroRad = 1000000;
    static constexpr int64_t kKiMilli = 20;
    static constexpr int64_t kNearOneG = 150000;                  // correct only within 0.15 g of 1 g
    static constexpr int64_t kMaxIntegralNanoRad = 100000000;     // 0.1 rad/s: the largest bias, at least
    static constexpr int64_t kDefaultZeroRateNanoRad = 34906585;  // 2 dps
    static constexpr int64_t kStepPicoRad = 500000000000;         // the largest turn one rotate() takes, 0.5 rad

    int64_t q[4] = {kSwitchOne, 0, 0, 0};  // w, x, y, z
    int64_t biasNanoRad[3] = {};
    SwitchGyroBias learner;
    // The bound of the part's zero-rate offset (initial tolerance and its drift over temperature). Each axis of the
    // bias is held to it, or to 0.1 rad/s if that is more: a part may exceed its typical figure.
    int64_t zeroRateNanoRad = kDefaultZeroRateNanoRad;
    int32_t accel[3] = {};  // the latest accelerometer sample, micro-g
    int64_t accelNorm = 0;  // its magnitude
    bool haveAccel = false;
    bool seeded = false;

    // A new session: the attitude from the next accelerometer sample; the learned bias stays.
    void restart()
    {
        q[0] = kSwitchOne;
        q[1] = q[2] = q[3] = 0;
        haveAccel = seeded = false;
        learner.restart();
    }

    int64_t biasLimit() const
    {
        return zeroRateNanoRad > kMaxIntegralNanoRad ? zeroRateNanoRad : kMaxIntegralNanoRad;
    }

    // The gravity the attitude implies, device frame, Q30.
    void gravity(int64_t out[3]) const
    {
        const int64_t w = q[0];
        const int64_t x = q[1];
        const int64_t y = q[2];
        const int64_t z = q[3];
        out[0] = -2 * ((x * z - w * y) >> kSwitchQ);
        out[1] = -2 * ((y * z + w * x) >> kSwitchQ);
        out[2] = -((w * w - x * x - y * y + z * z) >> kSwitchQ);
    }

    void addAccel(const int32_t sample[3])
    {
        memcpy(accel, sample, sizeof(accel));
        const int64_t n2 = int64_t{accel[0]} * accel[0] + int64_t{accel[1]} * accel[1] + int64_t{accel[2]} * accel[2];
        accelNorm = static_cast<int64_t>(switchIsqrt(static_cast<uint64_t>(n2)));
        haveAccel = true;
        if (!seeded)
            seed();
    }

    // One gyroscope sample standing for `dtMicros` (switchGyroGapMicros).
    void addGyro(const int32_t sample[3], uint32_t dtMicros)
    {
        if (haveAccel)
            learner.add(sample, dtMicros, accel, accelNorm, biasNanoRad, biasLimit());
        else
            learner.restart();
        if (!seeded)
            return;
        int64_t omega[3] = {sample[0], sample[1], sample[2]};
        int64_t down[3];
        if (unitAccel(down)) {
            int64_t estimate[3];
            gravity(estimate);
            const int64_t error[3] = {
                (down[1] * estimate[2] - down[2] * estimate[1]) >> kSwitchQ,
                (down[2] * estimate[0] - down[0] * estimate[2]) >> kSwitchQ,
                (down[0] * estimate[1] - down[1] * estimate[0]) >> kSwitchQ,
            };
            for (unsigned i = 0; i < 3; ++i) {
                // Mahony adds the error's integral to the rate; the bias is that integral's negative.
                int64_t bias = biasNanoRad[i] - ((kKiMilli * error[i] * dtMicros) >> kSwitchQ);
                const int64_t clamp = biasLimit();
                if (bias > clamp)
                    bias = clamp;
                if (bias < -clamp)
                    bias = -clamp;
                biasNanoRad[i] = bias;
                omega[i] += (kKpMicroRad * error[i]) >> kSwitchQ;
            }
        }
        int64_t turn = 0;  // 1e-12 rad: omega (urad/s) * dt (us)
        for (unsigned i = 0; i < 3; ++i) {
            omega[i] -= biasNanoRad[i] / 1000;
            turn += (omega[i] < 0 ? -omega[i] : omega[i]) * static_cast<int64_t>(dtMicros);
        }
        // rotate() is third order in the half angle: steps of at most 0.5 rad keep it exact to 1e-4, where one step over
        // a gap of four periods at full scale (up to 12 rad) would diverge.
        const int64_t steps = turn / kStepPicoRad + 1;
        for (int64_t step = 0; step < steps; ++step) {
            const int64_t micros = dtMicros * (step + 1) / steps - dtMicros * step / steps;
            int64_t half[3];
            for (unsigned i = 0; i < 3; ++i)
                half[i] = (omega[i] * micros / 2000) * kSwitchOne / 1000000000;
            rotate(half);
        }
    }

    // The latest sample's rotation rate without the estimated bias, micro-rad/s.
    void rotationRate(const int32_t gyro[3], int64_t out[3]) const
    {
        for (unsigned i = 0; i < 3; ++i)
            out[i] = gyro[i] - biasNanoRad[i] / 1000;
    }

    // The acceleration that is not gravity, micro-g (CMDeviceMotion: total = gravity + user acceleration).
    void userAcceleration(int64_t out[3]) const
    {
        int64_t g[3];
        gravity(g);
        for (unsigned i = 0; i < 3; ++i)
            out[i] = accel[i] - g[i] * 1000000 / kSwitchOne;
    }

private:
    // The measured gravity direction, Q30, when the acceleration is close enough to 1 g to be gravity alone.
    bool unitAccel(int64_t out[3]) const
    {
        if (!haveAccel || accelNorm < 1000000 - kNearOneG || accelNorm > 1000000 + kNearOneG)
            return false;
        for (unsigned i = 0; i < 3; ++i)
            out[i] = int64_t{accel[i]} * kSwitchOne / accelNorm;
        return true;
    }

    // The shortest rotation taking the measured up direction (-accel) to the reference Z.
    void seed()
    {
        const int64_t norm = accelNorm;
        if (norm < 1000000 / 2)
            return;
        const int64_t up[3] = {-accel[0] * kSwitchOne / norm, -accel[1] * kSwitchOne / norm,
                               -accel[2] * kSwitchOne / norm};
        int64_t raw[4] = {kSwitchOne + up[2], up[1], -up[0], 0};
        if (raw[0] < kSwitchOne / 1000000) {
            raw[0] = 0;
            raw[1] = kSwitchOne;
            raw[2] = 0;
        }
        const uint64_t length2 = static_cast<uint64_t>((raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]) >> 4);
        const int64_t length = static_cast<int64_t>(switchIsqrt(length2)) << 2;
        for (unsigned i = 0; i < 4; ++i)
            q[i] = raw[i] * kSwitchOne / length;
        seeded = true;
    }

    // q = q * (cos|h|, sin|h| h/|h|), third order in |h|, then back to unit length.
    void rotate(const int64_t half[3])
    {
        const int64_t h2 = (half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) >> kSwitchQ;
        const int64_t c = kSwitchOne - h2 / 2;
        const int64_t s = kSwitchOne - h2 / 6;
        const int64_t r[4] = {c, (half[0] * s) >> kSwitchQ, (half[1] * s) >> kSwitchQ, (half[2] * s) >> kSwitchQ};
        const int64_t w = q[0];
        const int64_t x = q[1];
        const int64_t y = q[2];
        const int64_t z = q[3];
        int64_t next[4] = {
            (w * r[0] - x * r[1] - y * r[2] - z * r[3]) >> kSwitchQ,
            (w * r[1] + x * r[0] + y * r[3] - z * r[2]) >> kSwitchQ,
            (w * r[2] - x * r[3] + y * r[0] + z * r[1]) >> kSwitchQ,
            (w * r[3] + x * r[2] - y * r[1] + z * r[0]) >> kSwitchQ,
        };
        const int64_t n2 = (next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]) >> kSwitchQ;
        // One Newton step of 1/sqrt(n2). The series leaves n2 short of 1 by |h|^4/12, about 3e-4 at a 0.25 rad half
        // step, and the step brings |q| back within about 4e-8 of 1.
        const int64_t k = (3 * kSwitchOne - n2) / 2;
        for (unsigned i = 0; i < 4; ++i)
            q[i] = (next[i] * k) >> kSwitchQ;
    }
};

// CoreMotion's 6-axis device motion report (CLSensorFusionServiceSPU::populateDeviceMotion, type 3), little-endian:
// type, status (bit 8: initialized; CoreMotion drops samples without it), client ID (logged only), quaternion x, y, z,
// w, rotation rate (rad/s), user acceleration (g). 21H461 takes exactly these 52 bytes; 24B119 takes 60, the last two
// floats ones it defaults to -1 and 0 itself.
constexpr size_t kSwitchDeviceMotion21H461 = 52;
constexpr size_t kSwitchDeviceMotion24B119 = 60;
constexpr uint8_t kSwitchDeviceMotionSixAxis = 3;
constexpr uint16_t kSwitchDeviceMotionInitialized = 0x100;

inline void switchDeviceMotionPayload(const SwitchMotionFusion &fusion, const int32_t gyro[3], uint8_t *out,
                                      size_t size)
{
    memset(out, 0, size);
    const auto put = [out](size_t at, uint32_t bits) {
        for (unsigned i = 0; i < 4; ++i)
            out[at + i] = static_cast<uint8_t>(bits >> (8 * i));
    };
    out[0] = kSwitchDeviceMotionSixAxis;
    const uint16_t status = fusion.seeded ? kSwitchDeviceMotionInitialized : 0;
    out[1] = static_cast<uint8_t>(status);
    out[2] = static_cast<uint8_t>(status >> 8);
    put(12, switchFloatBits(fusion.q[1], kSwitchQ));
    put(16, switchFloatBits(fusion.q[2], kSwitchQ));
    put(20, switchFloatBits(fusion.q[3], kSwitchQ));
    put(24, switchFloatBits(fusion.q[0], kSwitchQ));
    int64_t rate[3];
    int64_t user[3];
    fusion.rotationRate(gyro, rate);
    fusion.userAcceleration(user);
    for (unsigned i = 0; i < 3; ++i) {
        put(28 + 4 * i, switchMicroFloatBits(rate[i]));
        put(40 + 4 * i, switchMicroFloatBits(user[i]));
    }
    if (size >= kSwitchDeviceMotion24B119)
        put(52, 0xbf800000u);
}
