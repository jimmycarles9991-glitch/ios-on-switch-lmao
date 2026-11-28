// SwitchMotionFusion.h on the host: its float bit patterns against the host's floats, the attitude it seeds and
// integrates against closed-form rotations and against CoreMotion's own reading of the report, its gyroscope bias
// estimate (switch_motion_bias_test has the stillness cases), and the device motion report's layout in both builds'
// lengths.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "drivers/SwitchMotion/test/fusion_sim.h"

namespace {
float FromBits(uint32_t bits)
{
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t Bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float PayloadFloat(const uint8_t *payload, size_t at)
{
    uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        bits |= static_cast<uint32_t>(payload[at + i]) << (8 * i);
    return FromBits(bits);
}

void Gravity(const SwitchMotionFusion &fusion, double out[3])
{
    int64_t g[3];
    fusion.gravity(g);
    for (unsigned i = 0; i < 3; ++i)
        out[i] = static_cast<double>(g[i]) / static_cast<double>(kSwitchOne);
}

// CoreMotion 24B119's reading of a type-3 report, written from its disassembly rather than from SwitchMotionFusion.h:
// the gravity it derives from the quaternion (x, y, z, w) and its attitude prediction q * dq(rate * dt)
// (CLSensorFusionService_Predict::applyConstantRotationRate).
void Quaternion(const uint8_t *payload, double q[4])
{
    for (unsigned i = 0; i < 4; ++i)
        q[i] = PayloadFloat(payload, 12 + 4 * i);
}

void CoreMotionGravity(const double q[4], double out[3])
{
    const double x = q[0];
    const double y = q[1];
    const double z = q[2];
    const double w = q[3];
    out[0] = 2 * (w * y - x * z);
    out[1] = -2 * (w * x + y * z);
    out[2] = 2 * (x * x + y * y) - 1;
}

void CoreMotionPredict(const double q[4], const double rate[3], double dt, double out[4])
{
    const double half = std::sqrt(rate[0] * rate[0] + rate[1] * rate[1] + rate[2] * rate[2]) * dt / 2;
    const double scale = half > 0 ? std::sin(half) / (2 * half) * dt : 0;
    const double d[4] = {rate[0] * scale, rate[1] * scale, rate[2] * scale, std::cos(half)};
    out[0] = d[3] * q[0] + d[2] * q[1] - d[1] * q[2] + d[0] * q[3];
    out[1] = -d[2] * q[0] + d[3] * q[1] + d[0] * q[2] + d[1] * q[3];
    out[2] = d[1] * q[0] - d[0] * q[1] + d[3] * q[2] + d[2] * q[3];
    out[3] = -d[0] * q[0] - d[1] * q[1] - d[2] * q[2] + d[3] * q[3];
}

// Feeds `seconds` of samples at 200 Hz: a fixed accelerometer and gyroscope reading.
void Feed(SwitchMotionFusion &fusion, const int32_t accel[3], const int32_t gyro[3], double seconds)
{
    const unsigned samples = static_cast<unsigned>(seconds * 200 + 0.5);
    for (unsigned i = 0; i < samples; ++i) {
        fusion.addAccel(accel);
        fusion.addGyro(gyro, 5000);
    }
}
}  // namespace

TEST(SwitchMotionFusion, FloatBitsMatchTheHostsRounding)
{
    EXPECT_EQ(switchFloatBits(0, 30), 0u);
    EXPECT_EQ(switchFloatBits(kSwitchOne, 30), Bits(1.0f));
    EXPECT_EQ(switchFloatBits(-kSwitchOne / 2, 30), Bits(-0.5f));
    EXPECT_EQ(switchMicroFloatBits(-1000000), Bits(-1.0f));
    std::mt19937_64 random(42);
    for (int i = 0; i < 100000; ++i) {
        const int64_t value = static_cast<int64_t>(random() >> 2) >> (random() % 60);
        const int64_t signedValue = (random() & 1) ? -value : value;
        EXPECT_EQ(switchFloatBits(signedValue, 24),
                  Bits(static_cast<float>(static_cast<double>(signedValue) / 16777216.0)))
            << signedValue;
    }
}

TEST(SwitchMotionFusion, SeedsTheTiltFromTheFirstAccelerometerSample)
{
    const int32_t cases[][3] = {
        {0, 0, -1000000}, {0, -1000000, 0},           {1000000, 0, 0},
        {0, 0, 1000000},  {300000, -400000, -866025}, {1, 0, 999999},
    };
    for (const auto &accel : cases) {
        SwitchMotionFusion fusion;
        fusion.addAccel(accel);
        ASSERT_TRUE(fusion.seeded);
        double g[3];
        Gravity(fusion, g);
        const double n = std::sqrt(static_cast<double>(accel[0]) * accel[0] + static_cast<double>(accel[1]) * accel[1] +
                                   static_cast<double>(accel[2]) * accel[2]);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(g[i], accel[i] / n, 2e-6) << accel[0] << " " << accel[1] << " " << accel[2];
    }
    SwitchMotionFusion faceUp;
    const int32_t down[3] = {0, 0, -1000000};
    faceUp.addAccel(down);
    EXPECT_EQ(faceUp.q[0], kSwitchOne) << "face up is the reference attitude";
}

TEST(SwitchMotionFusion, IntegratesARotationAboutTheVertical)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t spin[3] = {0, 0, 1000000};  // 1 rad/s about +Z
    Feed(fusion, down, spin, 1.0);
    const double half = 0.5;
    EXPECT_NEAR(static_cast<double>(fusion.q[0]) / kSwitchOne, std::cos(half), 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[3]) / kSwitchOne, std::sin(half), 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[1]) / kSwitchOne, 0, 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[2]) / kSwitchOne, 0, 1e-4);
}

TEST(SwitchMotionFusion, TiltingUpByAQuarterTurnAboutXIsUprightPortrait)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    fusion.addAccel(down);
    const int32_t shaken[3] = {0, 0, -2000000};  // 2 g: no correction, the gyroscope alone
    const int32_t tilt[3] = {1570796, 0, 0};     // pi/2 rad over one second
    Feed(fusion, shaken, tilt, 1.0);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[0], 0, 1e-3);
    EXPECT_NEAR(g[1], -1, 1e-3);
    EXPECT_NEAR(g[2], 0, 1e-3);
}

TEST(SwitchMotionFusion, GravityPullsADriftedAttitudeBack)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    fusion.addAccel(down);
    const int32_t shaken[3] = {0, 0, -2000000};
    const int32_t tilt[3] = {349066, 0, 0};  // 20 degrees of drift
    Feed(fusion, shaken, tilt, 1.0);
    const int32_t still[3] = {0, 0, 0};
    Feed(fusion, down, still, 5.0);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[2], -1, 1e-4);
    EXPECT_NEAR(g[1], 0, 1e-2);
}

TEST(SwitchMotionFusion, EstimatesAConstantGyroscopeBias)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t biased[3] = {10000, -5000, 0};  // 0.01 and -0.005 rad/s at rest
    Feed(fusion, down, biased, 10.0);
    EXPECT_NEAR(fusion.biasNanoRad[0] / 1e9, 0.01, 0.0002);
    EXPECT_NEAR(fusion.biasNanoRad[1] / 1e9, -0.005, 0.0002);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[2], -1, 1e-4) << "the bias no longer tilts the attitude";
    int64_t rate[3];
    fusion.rotationRate(biased, rate);
    EXPECT_LT(std::abs(rate[0]), 200) << "rotation rate reported without the bias";
}

// Gravity cannot see rotation about the vertical, and lying face up neither can stillness: a Z offset reads as a turn.
// Rested propped up as well, the offset is learned and the yaw holds.
TEST(SwitchMotionFusion, LearnsTheVerticalBiasFromASecondAttitude)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0, 0, 0.01};
    SimRest(fusion, down, offset, 5.0);
    EXPECT_EQ(fusion.biasNanoRad[2], 0);
    const double tip[3] = {1, 0, 0};
    SimTurn(fusion, down, tip, offset, M_PI / 4);
    SimRest(fusion, down, offset, 2.0);
    const double back[3] = {-1, 0, 0};
    SimTurn(fusion, down, back, offset, M_PI / 4);
    EXPECT_NEAR(fusion.biasNanoRad[2] / 1e9, 0.01, 0.0002);
    const int32_t faceUp[3] = {0, 0, -1000000};
    fusion.restart();
    fusion.addAccel(faceUp);
    const int32_t biased[3] = {0, 0, 10000};
    Feed(fusion, faceUp, biased, 60.0);
    const double yaw = 2 * std::atan2(static_cast<double>(fusion.q[3]), static_cast<double>(fusion.q[0]));
    EXPECT_LT(std::fabs(yaw), 0.02) << "yaw drift in a minute, radians";
}

TEST(SwitchMotionFusion, ABiasLearnedOnceOutlivesTheSession)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t biased[3] = {10000, 0, 0};
    Feed(fusion, down, biased, 5.0);
    fusion.restart();
    EXPECT_FALSE(fusion.seeded);
    EXPECT_NEAR(fusion.biasNanoRad[0] / 1e9, 0.01, 0.0002);
    Feed(fusion, down, biased, 0.4);  // shorter than the stillness the learning needs
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[1], 0, 1e-3) << "no tilt from the offset";
}

TEST(SwitchMotionFusion, ASteadyTurnIsNotTakenForBias)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t turn[3] = {0, 0, 200000};  // 0.2 rad/s about the vertical, past any plausible offset
    Feed(fusion, down, turn, 10.0);
    EXPECT_EQ(fusion.biasNanoRad[2], 0);
}

// The longest gap a sample can stand for (four periods at 12.5 Hz) at full scale, about one axis and about all three:
// the attitude stays a unit quaternion and turns by the closed-form angle.
TEST(SwitchMotionFusion, ALongGapAtFullScaleTurnsByTheClosedFormAngle)
{
    struct Case {
        int32_t rate[3];
        uint32_t micros;
    };

    const int32_t full = 34906585;  // 2000 dps
    for (const Case c :
         {Case{{0, 0, full}, 160000}, Case{{0, 0, full}, switchGyroGapMicros(1000000, 80000)},
          Case{{full, full, full}, switchGyroGapMicros(1000000, 80000)}, Case{{-full, full, -full}, 320000}}) {
        SwitchMotionFusion fusion;
        const int32_t shaken[3] = {0, 0, -2000000};  // 2 g: no gravity correction
        fusion.addAccel(shaken);
        fusion.addGyro(c.rate, c.micros);
        double norm = 0;
        for (const int64_t v : fusion.q)
            norm += static_cast<double>(v) * static_cast<double>(v);
        EXPECT_NEAR(std::sqrt(norm) / static_cast<double>(kSwitchOne), 1, 1e-6);
        double rate = 0;
        for (const int32_t v : c.rate)
            rate += static_cast<double>(v) * v;
        rate = std::sqrt(rate) / 1e6;
        const double half = rate * c.micros / 1e6 / 2;
        const double want[4] = {std::cos(half), std::sin(half) * c.rate[0] / 1e6 / rate,
                                std::sin(half) * c.rate[1] / 1e6 / rate, std::sin(half) * c.rate[2] / 1e6 / rate};
        double error = 0;
        for (unsigned i = 0; i < 4; ++i)
            error = std::max(error, std::fabs(static_cast<double>(fusion.q[i]) / kSwitchOne - want[i]));
        EXPECT_LT(error, 2e-3) << c.micros << " us at " << rate << " rad/s: turned " << 2 * half << " rad";
    }
}

// A conjugated quaternion convention reads as the mirrored tilt: (0, +1, 0) instead of (0, -1, 0) here.
TEST(SwitchMotionFusion, CoreMotionReadsTheRealGravityFromTheReport)
{
    SwitchMotionFusion fusion;
    const int32_t faceUp[3] = {0, 0, -2000000};
    fusion.addAccel(faceUp);
    double down[3] = {0, 0, -1};
    const double none[3] = {};
    const double aboutX[3] = {M_PI / 2, 0, 0};
    const double aboutZ[3] = {0, 0, M_PI / 2};
    const double oblique[3] = {0.3, -0.7, 0.4};
    uint8_t payload[kSwitchDeviceMotion24B119];
    const int32_t still[3] = {};
    for (const double *rate : {aboutX, aboutZ, oblique}) {
        SimTurn(fusion, down, rate, none, 1.0, 2.0, [](const int32_t *) {});
        switchDeviceMotionPayload(fusion, still, payload, sizeof(payload));
        double q[4];
        double g[3];
        Quaternion(payload, q);
        CoreMotionGravity(q, g);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(g[i], down[i], 2e-3) << i;
    }
}

// CoreMotion carries the attitude forward between reports with the report's own rotation rate, the increment on the
// right in the device frame. A quaternion in the other convention, or the increment on the other side, predicts away
// from the next report.
TEST(SwitchMotionFusion, CoreMotionsPredictionLandsOnTheNextReport)
{
    SwitchMotionFusion fusion;
    const int32_t tilted[3] = {300000, -400000, -866025};
    fusion.addAccel(tilted);
    double down[3] = {0.3, -0.4, -0.866025};
    const double rate[3] = {0.9, -0.6, 1.4};
    const double none[3] = {};
    uint8_t payload[kSwitchDeviceMotion24B119];
    double before[4] = {};
    double reportedRate[3] = {};
    unsigned n = 0;
    SimTurn(fusion, down, rate, none, 1.0, 2.0, [&](const int32_t *gyro) {
        if (++n % 10)
            return;
        switchDeviceMotionPayload(fusion, gyro, payload, sizeof(payload));
        double q[4];
        Quaternion(payload, q);
        if (n > 10) {
            double predicted[4];
            CoreMotionPredict(before, reportedRate, 10 * kSimDt, predicted);
            for (unsigned i = 0; i < 4; ++i)
                EXPECT_NEAR(predicted[i], q[i], 1e-4) << "report " << n / 10 << ", component " << i;
        }
        for (unsigned i = 0; i < 4; ++i)
            before[i] = q[i];
        for (unsigned i = 0; i < 3; ++i)
            reportedRate[i] = PayloadFloat(payload, 28 + 4 * i);
    });
}

TEST(SwitchMotionFusion, ReportLayoutInBothBuildsLengths)
{
    SwitchMotionFusion fusion;
    const int32_t accel[3] = {0, -1000000, 0};
    fusion.addAccel(accel);
    const int32_t gyro[3] = {250000, 0, -125000};
    for (const size_t size : {kSwitchDeviceMotion21H461, kSwitchDeviceMotion24B119}) {
        uint8_t payload[kSwitchDeviceMotion24B119];
        std::memset(payload, 0xaa, sizeof(payload));
        switchDeviceMotionPayload(fusion, gyro, payload, size);
        EXPECT_EQ(payload[0], 3);
        EXPECT_EQ(payload[1] | payload[2] << 8, 0x100);
        for (unsigned i = 3; i < 12; ++i)
            EXPECT_EQ(payload[i], 0) << i;
        const double s = std::sqrt(0.5);
        EXPECT_NEAR(PayloadFloat(payload, 12), s, 1e-6);  // x
        EXPECT_NEAR(PayloadFloat(payload, 16), 0, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 20), 0, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 24), s, 1e-6);  // w
        EXPECT_NEAR(PayloadFloat(payload, 28), 0.25, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 36), -0.125, 1e-6);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(PayloadFloat(payload, 40 + 4 * i), 0, 1e-5);
        if (size == kSwitchDeviceMotion24B119) {
            EXPECT_EQ(PayloadFloat(payload, 52), -1.0f);
            EXPECT_EQ(PayloadFloat(payload, 56), 0.0f);
        } else {
            EXPECT_EQ(payload[52], 0xaa) << "nothing past 21H461's 52 bytes";
        }
    }
    SwitchMotionFusion unseeded;
    uint8_t payload[kSwitchDeviceMotion24B119];
    switchDeviceMotionPayload(unseeded, gyro, payload, sizeof(payload));
    EXPECT_EQ(payload[1] | payload[2] << 8, 0) << "not initialized before the first accelerometer sample";
}
