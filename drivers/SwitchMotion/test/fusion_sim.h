// A console moving under SwitchMotionFusion on the host: 200 Hz samples of a known rotation, gravity moved with it.
#pragma once
#include <cmath>
#include <cstdint>

#include "drivers/SwitchMotion/SwitchMotionFusion.h"

constexpr double kSimDt = 0.005;

// `seconds` of a console turning at `rate` (rad/s, device frame) whose gyroscope reads `offset` more. `down` is gravity
// in the device frame (g), moved as the console turns (dg/dt = -rate x g). With `accelG` the accelerometer reads that
// many g along it (2: no gravity correction, the gyroscope alone); `each` runs after every sample.
template <typename Each>
void SimTurn(SwitchMotionFusion &fusion, double down[3], const double rate[3], const double offset[3], double seconds,
             double accelG, Each each)
{
    const auto samples = static_cast<unsigned>(std::lround(seconds / kSimDt));
    for (unsigned n = 0; n < samples; ++n) {
        const double moved[3] = {
            rate[1] * down[2] - rate[2] * down[1],
            rate[2] * down[0] - rate[0] * down[2],
            rate[0] * down[1] - rate[1] * down[0],
        };
        double norm = 0;
        for (unsigned i = 0; i < 3; ++i) {
            down[i] -= moved[i] * kSimDt;
            norm += down[i] * down[i];
        }
        norm = std::sqrt(norm);
        int32_t accel[3];
        int32_t gyro[3];
        for (unsigned i = 0; i < 3; ++i) {
            down[i] /= norm;
            accel[i] = static_cast<int32_t>(std::lround(down[i] * accelG * 1e6));
            gyro[i] = static_cast<int32_t>(std::lround((rate[i] + offset[i]) * 1e6));
        }
        fusion.addAccel(accel);
        fusion.addGyro(gyro, 5000);
        each(gyro);
    }
}

inline void SimTurn(SwitchMotionFusion &fusion, double down[3], const double rate[3], const double offset[3],
                    double seconds)
{
    SimTurn(fusion, down, rate, offset, seconds, 1.0, [](const int32_t *) {});
}

inline void SimRest(SwitchMotionFusion &fusion, double down[3], const double offset[3], double seconds)
{
    const double still[3] = {};
    SimTurn(fusion, down, still, offset, seconds);
}
