// SwitchMotionBias.h's gyroscope offset learning through SwitchMotionFusion on the host: an offset at rest is learned
// perpendicular to gravity at once and along it from a second attitude; turns about the vertical, at any rate, for any
// time, before or after a rest, and slow tilts are not.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "drivers/SwitchMotion/test/fusion_sim.h"

namespace {
double Bias(const SwitchMotionFusion &fusion, unsigned axis)
{
    return static_cast<double>(fusion.biasNanoRad[axis]) / 1e9;
}

constexpr double kSt = 12 * M_PI / 180;  // LSM6DS3H: 10 dps typical, plus drift
constexpr double kTip = M_PI / 4;

void UseStBound(SwitchMotionFusion &fusion)
{
    fusion.zeroRateNanoRad = static_cast<int64_t>(kSt * 1e9);
}

// Turned about the device's X axis by `angle` at 1 rad/s.
void Tip(SwitchMotionFusion &fusion, double down[3], const double offset[3], double angle)
{
    const double rate[3] = {angle > 0 ? 1.0 : -1.0, 0, 0};
    SimTurn(fusion, down, rate, offset, std::fabs(angle));
}

// Rests flat, propped up 45 degrees, and flat again: every component of `offset` seen perpendicular to gravity. The
// second rest holds the four windows the vertical component needs. The last is long because the gravity correction,
// while the vertical component was unknown, built an attitude error whose correction its integral puts into the bias:
// 0.57 mrad/s just after the tip back for a 0.05 rad/s offset (AnOffsetPastThePartsTypicalBoundIsLearned), which
// tracking at a sixteenth per window brings to 0.16 mrad/s in 10 s.
void LearnAll(SwitchMotionFusion &fusion, double down[3], const double offset[3])
{
    SimRest(fusion, down, offset, 1.5);
    Tip(fusion, down, offset, kTip);
    SimRest(fusion, down, offset, 2.5);
    Tip(fusion, down, offset, -kTip);
    SimRest(fusion, down, offset, 10.0);
}

void ExpectBias(const SwitchMotionFusion &fusion, const double offset[3], double tolerance, const char *what)
{
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_NEAR(Bias(fusion, i), offset[i], tolerance) << what << ", axis " << i;
}

// The parts at rest: white noise at the datasheet's floor, quantised to the LSB, at the sample rate given.
struct Floor {
    double gyroSigma, accelSigma;  // rad/s, g
    double gyroLsb, accelLsb;
    uint32_t periodMicros;
};

constexpr Floor kIcm200 = {0.0009, 0.0005, 1064.225e-6, 1.0 / 4096, 5000};
constexpr Floor kIcm12 = {0.0005, 0.0003, 1064.225e-6, 1.0 / 4096, 80000};
constexpr Floor kSt208 = {0.0011, 0.0006, 1221.73e-6, 0.000244, 4808};
constexpr double kNoisyWorst = 0.00075;  // rad/s over every seed, axis and case (0.26-0.60 measured)

// `seconds` of a console turning at `rate` (rad/s; none: at rest) as the parts read it at `floor`.
void NoisyTurn(SwitchMotionFusion &fusion, double down[3], const double rate[3], const double offset[3], double seconds,
               const Floor &floor, std::mt19937 &random)
{
    std::normal_distribution<double> unit(0.0, 1.0);
    const auto quantise = [](double value, double lsb) {
        return std::lround(value / lsb) * lsb;
    };
    const double dt = floor.periodMicros * 1e-6;
    const auto samples = static_cast<unsigned>(seconds / dt);
    for (unsigned n = 0; n < samples; ++n) {
        const double moved[3] = {
            rate[1] * down[2] - rate[2] * down[1],
            rate[2] * down[0] - rate[0] * down[2],
            rate[0] * down[1] - rate[1] * down[0],
        };
        double norm = 0;
        for (unsigned i = 0; i < 3; ++i) {
            down[i] -= moved[i] * dt;
            norm += down[i] * down[i];
        }
        norm = std::sqrt(norm);
        int32_t accel[3];
        int32_t gyro[3];
        for (unsigned i = 0; i < 3; ++i) {
            down[i] /= norm;
            accel[i] = static_cast<int32_t>(
                std::lround(quantise(down[i] + floor.accelSigma * unit(random), floor.accelLsb) * 1e6));
            gyro[i] = static_cast<int32_t>(
                std::lround(quantise(rate[i] + offset[i] + floor.gyroSigma * unit(random), floor.gyroLsb) * 1e6));
        }
        fusion.addAccel(accel);
        fusion.addGyro(gyro, floor.periodMicros);
    }
}

void NoisyRest(SwitchMotionFusion &fusion, double down[3], const double offset[3], double seconds, const Floor &floor,
               std::mt19937 &random)
{
    const double still[3] = {};
    NoisyTurn(fusion, down, still, offset, seconds, floor, random);
}
}  // namespace

TEST(SwitchMotionBias, ASlowSteadyTurnAboutTheVerticalIsNotLearned)
{
    for (const bool st : {false, true}) {
        for (const double seconds : {2.0, 10.0}) {
            SwitchMotionFusion fusion;
            if (st)
                UseStBound(fusion);
            double down[3] = {0, 0, -1};
            const double turn[3] = {0, 0, 0.05};
            const double none[3] = {};
            SimTurn(fusion, down, turn, none, seconds);
            EXPECT_LT(std::fabs(Bias(fusion, 2)), 0.002) << seconds << " s at 0.05 rad/s, ST bound " << st;
        }
    }
}

// The turn that came before the first rest is not the offset either, even at the largest offset the part can have.
TEST(SwitchMotionBias, ATurnBeforeTheFirstRestIsNotLearned)
{
    SwitchMotionFusion fusion;
    UseStBound(fusion);
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.002, -0.001, 0};
    const double turn[3] = {0, 0, 0.15};
    SimTurn(fusion, down, turn, offset, 2.0);
    SimRest(fusion, down, offset, 10.0);
    ExpectBias(fusion, offset, 0.0005, "after the turn and 10 s of rest");
}

TEST(SwitchMotionBias, ASlowTiltIsNotLearned)
{
    SwitchMotionFusion fusion;
    UseStBound(fusion);
    double down[3] = {0, 0, -1};
    const double tilt[3] = {0.05, 0, 0};
    const double none[3] = {};
    SimTurn(fusion, down, tilt, none, 2.0);
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_LT(std::fabs(Bias(fusion, i)), 0.002) << i;
}

// A tilt slow enough to pass the stillness test is what the accelerometer saw, not the offset: in either direction
// about either tilt axis.
TEST(SwitchMotionBias, ATiltTheAccelerometerSeesIsTakenOffTheWindow)
{
    for (const unsigned axis : {0u, 1u}) {
        for (const double rate : {0.01, -0.01}) {
            SwitchMotionFusion fusion;
            UseStBound(fusion);
            double down[3] = {0, 0, -1};
            const double offset[3] = {0.002, -0.001, 0};
            double tilt[3] = {};
            tilt[axis] = rate;
            SimTurn(fusion, down, tilt, offset, 3.0);
            for (unsigned i = 0; i < 2; ++i)
                EXPECT_NEAR(Bias(fusion, i), offset[i], 0.0005)
                    << "tilt about " << axis << " at " << rate << ", axis " << i;
        }
    }
}

// Once the console has rested, a steady turn about the vertical is told apart from the offset however long it lasts,
// and a turn about a tilt axis breaks the stillness.
TEST(SwitchMotionBias, ASlowTurnAfterRestingLeavesTheLearnedOffset)
{
    struct Case {
        unsigned axis;
        double rate, seconds;
        bool st;
    };

    for (const Case c :
         {Case{0, 0.05, 10, true}, Case{2, 0.05, 10, true}, Case{2, 0.03, 21, false}, Case{2, 0.10, 21, true}}) {
        SwitchMotionFusion fusion;
        if (c.st)
            UseStBound(fusion);
        double down[3] = {0, 0, -1};
        const double offset[3] = {0.002, -0.001, 0.003};
        LearnAll(fusion, down, offset);
        double turn[3] = {};
        turn[c.axis] = c.rate;
        SimTurn(fusion, down, turn, offset, c.seconds);
        SimRest(fusion, down, offset, 15.0);
        ExpectBias(fusion, offset, 0.0005, "after the turn");
    }
}

TEST(SwitchMotionBias, ASlowRampAboutTheVerticalIsNotFollowed)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.002, -0.001, 0.003};
    LearnAll(fusion, down, offset);
    for (int step = 0; step < 600; ++step) {
        const double turn[3] = {0, 0, 0.0025 * step * 0.1};
        SimTurn(fusion, down, turn, offset, 0.1);
    }
    ExpectBias(fusion, offset, 0.0005, "after 60 s ramping to 0.15 rad/s");
}

// On a stand panned or a turntable the console turns about the true vertical whatever way up it rests.
TEST(SwitchMotionBias, ATurnAboutTheVerticalWhileProppedUpIsNotLearned)
{
    SwitchMotionFusion fusion;
    UseStBound(fusion);
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.002, -0.001, 0.003};
    LearnAll(fusion, down, offset);
    Tip(fusion, down, offset, kTip);
    double turn[3];
    for (unsigned i = 0; i < 3; ++i)
        turn[i] = -0.05 * down[i];
    SimTurn(fusion, down, turn, offset, 30.0);
    ExpectBias(fusion, offset, 0.0005, "after 30 s turning about the vertical at 45 degrees");
}

TEST(SwitchMotionBias, AnOffsetAtRestIsLearnedWithinTheFirstSecondAndAlongGravityFromASecondAttitude)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.01, -0.005, 0.02};
    SimRest(fusion, down, offset, 1.1);
    EXPECT_EQ(fusion.learner.learned, SwitchGyroBias::Learned::AllButGravity);
    EXPECT_NEAR(Bias(fusion, 0), offset[0], 0.0002);
    EXPECT_NEAR(Bias(fusion, 1), offset[1], 0.0002);
    EXPECT_EQ(fusion.biasNanoRad[2], 0) << "lying flat, the vertical offset is a turn as far as anyone can tell";
    Tip(fusion, down, offset, kTip);
    SimRest(fusion, down, offset, 2.1);
    EXPECT_EQ(fusion.learner.learned, SwitchGyroBias::Learned::All);
    ExpectBias(fusion, offset, 0.0002, "propped up, two seconds after the tip");
}

// An offset as large as the part's bound passes for a turn while the console lies flat: it is learned only once the
// console has rested another way up.
TEST(SwitchMotionBias, AnOffsetAsLargeAsThePartsBoundIsLearnedOnlyFromASecondAttitude)
{
    SwitchMotionFusion fusion;
    UseStBound(fusion);
    double down[3] = {0, 0, -1};
    const double offset[3] = {0, 0, 0.15};  // 8.6 dps
    SimRest(fusion, down, offset, 3.0);
    EXPECT_EQ(fusion.biasNanoRad[2], 0);
    Tip(fusion, down, offset, kTip);
    SimRest(fusion, down, offset, 3.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.15, 0.001);
}

// The datasheet's figure is typical: a part past it still has its offset learned, up to 0.1 rad/s.
TEST(SwitchMotionBias, AnOffsetPastThePartsTypicalBoundIsLearned)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.05, 0, 0.05};  // 2.9 dps, past the ICM parts' 2
    LearnAll(fusion, down, offset);
    ExpectBias(fusion, offset, 0.0005, "learned");
}

// One tilt axis past the largest offset is held there, and does not keep the vertical axis from being learned.
TEST(SwitchMotionBias, ATiltAxisPastTheBoundDoesNotBlockTheVertical)
{
    for (const bool st : {false, true}) {
        SwitchMotionFusion fusion;
        if (st)
            UseStBound(fusion);
        const double limit = static_cast<double>(fusion.biasLimit()) / 1e9;
        double down[3] = {0, 0, -1};
        const double offset[3] = {limit + 0.04, 0, 0.01};
        LearnAll(fusion, down, offset);
        EXPECT_NEAR(Bias(fusion, 0), limit, 1e-9) << "ST bound " << st;
        EXPECT_NEAR(Bias(fusion, 2), 0.01, 0.0005) << "ST bound " << st;
    }
}

// About a tilt axis gravity sees an offset of any size: the correction's integral absorbs one past the part's bound.
TEST(SwitchMotionBias, ATiltAxisOffsetPastThePartsBoundIsAbsorbedByTheGravityCorrection)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.0524, 0, 0};  // 3 dps, past the ICM parts' 2
    SimRest(fusion, down, offset, 300.0);
    EXPECT_NEAR(Bias(fusion, 0), 0.0524, 0.003);
    int64_t g[3];
    fusion.gravity(g);
    EXPECT_NEAR(static_cast<double>(g[1]) / kSwitchOne, 0, 0.005) << "no standing tilt";
}

TEST(SwitchMotionBias, ADriftingOffsetIsFollowedWhereGravitySeesIt)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double cold[3] = {0.003, 0.002, 0.003};
    LearnAll(fusion, down, cold);
    const double warm[3] = {0.007, 0.002, 0.007};
    SimRest(fusion, down, warm, 30.0);
    EXPECT_NEAR(Bias(fusion, 0), 0.007, 0.0002) << "perpendicular to gravity: followed";
    EXPECT_NEAR(Bias(fusion, 2), 0.003, 0.0002) << "along gravity: a turn could read the same";
    for (int cycle = 0; cycle < 4; ++cycle) {
        Tip(fusion, down, warm, kTip);
        SimRest(fusion, down, warm, 30.0);
        Tip(fusion, down, warm, -kTip);
        SimRest(fusion, down, warm, 30.0);
    }
    ExpectBias(fusion, warm, 0.0005, "after resting both ways up");
}

// After the offset is known, a vertical turn while flat, for longer than any stillness, leaves it; resting another way
// up afterwards still finds the true offset.
TEST(SwitchMotionBias, AChangedOffsetIsFollowedOnlyWhereGravitySeesIt)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double before[3] = {0, 0, 0.003};
    LearnAll(fusion, down, before);
    const double after[3] = {0, 0, 0.02};
    SimRest(fusion, down, after, 25.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.003, 0.0002) << "flat: a 0.017 rad/s turn reads the same";
    for (int cycle = 0; cycle < 4; ++cycle) {
        Tip(fusion, down, after, kTip);
        SimRest(fusion, down, after, 30.0);
        Tip(fusion, down, after, -kTip);
        SimRest(fusion, down, after, 30.0);
    }
    ExpectBias(fusion, after, 0.0015, "after resting both ways up");
}

TEST(SwitchMotionBias, MovementBetweenFlatRestsDoesNotTeachTheVertical)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double before[3] = {0, 0, 0.003};
    LearnAll(fusion, down, before);
    const double after[3] = {0, 0, 0.02};
    const double shake[3] = {0.5, 0, 0};
    const double back[3] = {-0.5, 0, 0};
    for (int i = 0; i < 6; ++i) {
        SimRest(fusion, down, after, 5.0);
        SimTurn(fusion, down, shake, after, 0.1);
        SimTurn(fusion, down, back, after, 0.1);  // face up again: the gravity correction still cannot see Z
    }
    EXPECT_NEAR(Bias(fusion, 2), 0.003, 0.0002);
}

// At the parts' noise floors, quantised, the windows still agree and the offset is learned, at the fastest and the
// slowest rate, propped up at 30 degrees (where the second attitude's solve doubles the noise), 32 and 45, over 20
// seeds: rested flat, propped up and flat again, as a console set down on a table, a stand and the table again.
TEST(SwitchMotionBias, ANoisyRestIsLearned)
{
    struct Case {
        const Floor *floor;
        bool st;
        double degrees;
    };

    for (const Case c :
         {Case{&kIcm200, false, 30}, Case{&kIcm200, false, 32}, Case{&kIcm200, false, 45}, Case{&kIcm12, false, 30},
          Case{&kIcm12, false, 32}, Case{&kIcm12, false, 45}, Case{&kSt208, true, 32}}) {
        double worst = 0;
        for (unsigned seed = 1; seed <= 20; ++seed) {
            SwitchMotionFusion fusion;
            if (c.st)
                UseStBound(fusion);
            std::mt19937 random(seed);
            double down[3] = {0, 0, -1};
            const double offset[3] = {0.01, -0.005, 0.02};
            NoisyRest(fusion, down, offset, 3.0, *c.floor, random);
            EXPECT_EQ(fusion.learner.learned, SwitchGyroBias::Learned::AllButGravity) << seed;
            Tip(fusion, down, offset, c.degrees * M_PI / 180);
            NoisyRest(fusion, down, offset, 3.0, *c.floor, random);
            EXPECT_EQ(fusion.learner.learned, SwitchGyroBias::Learned::All) << seed;
            Tip(fusion, down, offset, -c.degrees * M_PI / 180);
            NoisyRest(fusion, down, offset, 3.0, *c.floor, random);
            for (unsigned i = 0; i < 3; ++i)
                worst = std::max(worst, std::fabs(Bias(fusion, i) - offset[i]));
        }
        std::printf("noisy rest, %u us, %.0f degrees: worst %.3f mrad/s over 20 seeds\n", c.floor->periodMicros,
                    c.degrees, worst * 1e3);
        EXPECT_LT(worst, kNoisyWorst) << c.floor->periodMicros << " us, " << c.degrees << " degrees";
    }
}

// A console settling slowly as it rests (a soft surface, a stand) tilts too slowly for one window to tell from noise.
// Learned as offset is what the gates let through, a fraction of a window's own threshold: the bounds below, over 20
// seeds, for tilts from 0.5 to 8 mrad/s while the first offset is learned and tracked, and while the vertical component
// is learned propped up.
TEST(SwitchMotionBias, ASlowNoisyTiltLeaksLittle)
{
    struct Case {
        const Floor *floor;
        bool st;
        double flat, propped;  // rad/s: the worst error allowed
    };

    for (const Case c : {Case{&kIcm200, false, 0.0007, 0.0005}, Case{&kIcm12, false, 0.0014, 0.0007},
                         Case{&kSt208, true, 0.0007, 0.0005}}) {
        for (const double rate : {0.0005, 0.001, 0.002, 0.005, 0.008}) {
            double flat = 0;
            double propped = 0;
            for (unsigned seed = 1; seed <= 20; ++seed) {
                std::mt19937 random(seed);
                const double tilt[3] = {rate, 0, 0};
                {
                    SwitchMotionFusion fusion;
                    if (c.st)
                        UseStBound(fusion);
                    double down[3] = {0, 0, -1};
                    const double offset[3] = {0.002, -0.001, 0};
                    NoisyTurn(fusion, down, tilt, offset, 3.0, *c.floor, random);
                    flat = std::max(flat, std::fabs(Bias(fusion, 0) - offset[0]));
                }
                SwitchMotionFusion fusion;
                if (c.st)
                    UseStBound(fusion);
                double down[3] = {0, 0, -1};
                const double offset[3] = {0.002, -0.001, 0.003};
                NoisyRest(fusion, down, offset, 3.0, *c.floor, random);
                Tip(fusion, down, offset, kTip);
                NoisyTurn(fusion, down, tilt, offset, 3.0, *c.floor, random);
                for (unsigned i = 0; i < 3; ++i)
                    propped = std::max(propped, std::fabs(Bias(fusion, i) - offset[i]));
            }
            std::printf("slow tilt, %u us, %.1f mrad/s: worst %.3f flat, %.3f propped (mrad/s)\n",
                        c.floor->periodMicros, rate * 1e3, flat * 1e3, propped * 1e3);
            EXPECT_LT(flat, c.flat) << c.floor->periodMicros << " us, " << rate << " rad/s";
            EXPECT_LT(propped, c.propped) << c.floor->periodMicros << " us, " << rate << " rad/s";
        }
    }
}

TEST(SwitchMotionBias, GapsBetweenGyroscopeSamples)
{
    EXPECT_EQ(switchGyroGapMicros(0, 5000), 5000u) << "the first sample of a session";
    EXPECT_EQ(switchGyroGapMicros(5200, 5000), 5200u);
    EXPECT_EQ(switchGyroGapMicros(15000, 5000), 15000u) << "two samples the part overwrote";
    EXPECT_EQ(switchGyroGapMicros(100, 5000), 2500u);
    EXPECT_EQ(switchGyroGapMicros(1000000, 5000), 20000u);
}

TEST(SwitchMotionBias, ARotationIsIntegratedOverTheRealGaps)
{
    SwitchMotionFusion fusion;
    const int32_t shaken[3] = {0, 0, -2000000};  // 2 g: no gravity correction
    const int32_t faceUp[3] = {0, 0, -1000000};
    fusion.addAccel(faceUp);
    fusion.addAccel(shaken);
    const int32_t spin[3] = {0, 0, 1000000};
    for (int i = 0; i < 100; ++i)
        fusion.addGyro(spin, i % 2 ? 2500 : 7500);  // the same second at uneven gaps
    const double yaw = 2 * std::atan2(static_cast<double>(fusion.q[3]), static_cast<double>(fusion.q[0]));
    EXPECT_NEAR(yaw, 0.5, 1e-4);
}
