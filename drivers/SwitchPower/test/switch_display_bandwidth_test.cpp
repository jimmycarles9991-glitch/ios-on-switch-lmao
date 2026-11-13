#include <gtest/gtest.h>
#include <vector>
#include "../SwitchDisplayBandwidth.h"
using namespace switch_display;

TEST(DisplayBandwidth, StaticUiAndOverlapKeepPeakService)
{
    Demand d[] = {{0, 0, 720, 1280, 312000000}, {0, 0, 720, 640, 156000000}, {0, 640, 720, 640, 312000000}};
    EXPECT_EQ(peak(d, 1), 312000000u);
    EXPECT_EQ(peak(d, 3), 624000000u);
    d[2].y = 639;
    EXPECT_EQ(peak(d, 3), 780000000u);
}

TEST(DisplayBandwidth, EmptyTouchingAndDisjointRectsDoNotAdd)
{
    Demand d[] = {{0, 0, 10, 10, 1}, {10, 0, 10, 10, 2}, {0, 10, 10, 10, 3}};
    EXPECT_EQ(peak(d, 3), 3u);
    d[2].height = 0;
    EXPECT_EQ(peak(d, 3), 2u);
    EXPECT_EQ(peak(nullptr, 0), 0u);
}

TEST(DisplayBandwidth, ScalingAndOverflowAreConservative)
{
    EXPECT_EQ(scaled(312000000, 1920, 720), 832000000u);
    EXPECT_EQ(scaled(11, 3, 2), 17u);
    EXPECT_EQ(scaled(11, 2, 3), 11u);
    EXPECT_EQ(scaled(11, 1, 0), UINT64_MAX);
    EXPECT_EQ(scaled(UINT64_MAX, 2, 1), UINT64_MAX);
    Demand d[] = {{0, 0, 1, 1, UINT64_MAX}, {0, 0, 1, 1, 1}};
    EXPECT_EQ(peak(d, 2), UINT64_MAX);
}

TEST(DisplayBandwidth, FloorIsL4TsLpddr4IsochronousRequest)
{
    EXPECT_EQ(floorKhz(312000000), 34822u);
    EXPECT_EQ(floorKhz(1), 2u);
    EXPECT_EQ(isoEfficiencyPercent(4999), 56u);
    EXPECT_EQ(isoEfficiencyPercent(649999), 56u);
    EXPECT_EQ(isoEfficiencyPercent(650000), 49u);
    EXPECT_EQ(isoEfficiencyPercent(700000), 45u);
    EXPECT_EQ(floorKhz(10400000000ULL), 1326531u);
    EXPECT_EQ(floorKhz(11200000000ULL), 1555556u);
    EXPECT_EQ(floorKhz(UINT64_MAX), UINT32_MAX);
}

TEST(DisplayBandwidth, FloorFitsVerifiedRatesAndRefusesOvercommit)
{
    const uint32_t rates[] = {204000, 1331200, 1600000};
    EXPECT_EQ(row(rates, 3, 312000000), 0u);
    EXPECT_EQ(row(rates, 3, 1600000000), 0u);
    EXPECT_EQ(row(rates, 3, 1827840000), 0u);
    EXPECT_EQ(row(rates, 3, 1827840001), 1u);
    EXPECT_EQ(row(rates, 3, 10400000000ULL), 1u);
    EXPECT_EQ(row(rates, 3, 11520000000ULL), 2u);
    EXPECT_EQ(row(rates, 3, 11520000001ULL), 3u);
    EXPECT_EQ(row(rates, 3, UINT64_MAX), 3u);
}

TEST(DisplayBandwidth, AdmissionRaisesOnceAndRetirementAloneMayLower)
{
    Reservation reservation;
    std::vector<uint64_t> requests;
    const auto apply = [&](uint64_t bytes) {
        requests.push_back(bytes);
        return 0;
    };
    EXPECT_TRUE(reservation.raise(300, apply));
    EXPECT_TRUE(reservation.raise(700, apply));
    EXPECT_TRUE(reservation.raise(500, apply));
    EXPECT_EQ(reservation.bytes(), 700u);
    EXPECT_EQ(requests, (std::vector<uint64_t>{300, 700}));
    EXPECT_TRUE(reservation.retire(500, apply));
    EXPECT_EQ(reservation.bytes(), 500u);
    EXPECT_TRUE(reservation.retire(500, apply));
    EXPECT_EQ(requests, (std::vector<uint64_t>{300, 700, 500}));
}

TEST(DisplayBandwidth, FailedAdmissionOrRetirementPreservesCommittedService)
{
    Reservation reservation;
    EXPECT_TRUE(reservation.raise(300, [](uint64_t) {
        return 0;
    }));
    EXPECT_FALSE(reservation.raise(700, [](uint64_t) {
        return -5;
    }));
    EXPECT_EQ(reservation.bytes(), 300u);
    EXPECT_FALSE(reservation.retire(100, [](uint64_t) {
        return -5;
    }));
    EXPECT_EQ(reservation.bytes(), 300u);
}

TEST(DisplayBandwidth, AdmissionEnvelopeKeepsInheritedAndOldPhysicalStreams)
{
    AdmissionEnvelope envelope;
    uint64_t admitted = 0;
    const auto reserve = [&](uint64_t bytes) {
        admitted = bytes;
        return true;
    };
    const uint64_t first[4] = {200, 0, 0, 0}, second[4] = {0, 300, 0, 0};
    const uint64_t active[2] = {100, 0};
    ASSERT_TRUE(envelope.admit(first, active, 50, reserve));
    EXPECT_EQ(admitted, 350u);
    ASSERT_TRUE(envelope.admit(second, active, 50, reserve));
    EXPECT_EQ(admitted, 550u);
    const uint64_t small[4] = {1, 0, 0, 0};
    ASSERT_TRUE(envelope.admit(small, active, 50, reserve));
    EXPECT_EQ(admitted, 550u);
    envelope.clear();
    ASSERT_TRUE(envelope.admit(small, active, 50, reserve));
    EXPECT_EQ(admitted, 151u);
}

TEST(DisplayBandwidth, RefusedAdmissionCannotPoisonTheNextEnvelope)
{
    AdmissionEnvelope envelope;
    const uint64_t huge[4] = {UINT64_MAX, 0, 0, 0}, small[4] = {10, 0, 0, 0}, active[2] = {};
    ASSERT_FALSE(envelope.admit(huge, active, 50, [](uint64_t bytes) {
        EXPECT_EQ(bytes, UINT64_MAX);
        return false;
    }));
    EXPECT_TRUE(envelope.admit(small, active, 50, [](uint64_t bytes) {
        EXPECT_EQ(bytes, 60u);
        return true;
    }));
}

TEST(DisplayBandwidth, EveryCrossFrameBankMixtureFitsTheAcceptedEnvelope)
{
    AdmissionEnvelope envelope;
    uint64_t bound = 0;
    const uint64_t active[2] = {125, 250};
    const uint64_t updates[][4] = {{400, 0, 0, 0}, {0, 300, 0, 0}, {0, 0, 500, 0}, {0, 0, 0, 200}};
    for (const auto &updated : updates)
        ASSERT_TRUE(envelope.admit(updated, active, 50, [&](uint64_t bytes) {
            bound = bytes;
            return true;
        }));
    const uint64_t candidates[] = {125, 250, 400, 300, 500, 200};
    for (unsigned a = 0; a != 6; ++a)
        for (unsigned b = 0; b != 6; ++b)
            if (a != b)
                EXPECT_LE(50 + candidates[a] + candidates[b], bound);
    EXPECT_EQ(bound, 950u);
}
