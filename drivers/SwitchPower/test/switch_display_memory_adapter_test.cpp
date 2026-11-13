// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>
#include "../SwitchDisplayBandwidth.h"
#include "../SwitchDisplayMemoryState.h"
#include "../SwitchEMCGovernor.h"

uint64_t mach_absolute_time()
{
    return 100;
}

struct OSObject {};

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnError = -1;
constexpr int OS_LOG_DEFAULT = 0;

template <class... T> void os_log_error(T...)
{}

std::vector<std::string> gLines;

template <class... T> void os_log(int, const char *format, T... args)
{
    char line[256];
    snprintf(line, sizeof(line), format, args...);
    gLines.emplace_back(line);
}

template <class... T> [[noreturn]] void panic(const char *, T...)
{
    throw std::runtime_error("clock authority");
}

struct Loop {
    template <class Function, class... Args> void runAction(Function action, OSObject *owner, Args... args)
    {
        action(owner, args..., nullptr, nullptr);
    }
};

struct Registers {
    std::array<uint32_t, 1024> values{};
    unsigned writes = 0;

    struct Word {
        Registers *regs;
        unsigned index;

        operator uint32_t() const
        {
            return regs->values[index];
        }

        void operator=(uint32_t value)
        {
            regs->values[index] = value;
            ++regs->writes;
        }
    };

    Word operator[](unsigned index)
    {
        return {this, index};
    }
};

class SwitchEMC : public OSObject {
public:
    Loop object;
    Loop *loop = &object;
    Registers mcRegs;
    switch_display::MemoryState memoryState;
    bool memoryQualified = true, memoryDirty = false, fixed = false;
    uint32_t memoryAppliedKhz = 0;
    unsigned carRate = 0;
    uint32_t khz[3] = {204000, 1331200, 1600000};
    unsigned count = 3, cur = 0, starts = 0, watermarks = 0;
    uint64_t residency[3] = {}, since = 0;
    unsigned moves = 0;
    bool governing = false, refusalLogged = false;
    uint64_t floorBytes() const;
    unsigned memoryFloor(unsigned) const;
    void dropDisplayMemory(const char *);

    bool displayMemoryReady() const
    {
        return memoryQualified;
    }

    bool programMemory(uint32_t, uint32_t, bool = false);
    static IOReturn memoryAction(OSObject *, void *, void *, void *, void *);
    static IOReturn governAction(OSObject *, void *, void *, void *, void *);
    unsigned reconcileRate(uint32_t, bool = false);
    bool moveTo(unsigned, switch_emc_reason);
    void logMove(unsigned, unsigned, unsigned, switch_emc_reason) const;
    int reserveMemory(const switch_display_memory_head &);
    bool useFixedRate(unsigned);
    IOReturn holdFixedRate();

    unsigned carEmcKhz() const
    {
        return carRate;
    }

    void program()
    {
        ++watermarks;
    }

    void startActmon()
    {
        ++starts;
    }
};

namespace {
SwitchEMC *gEMC;
uint64_t gDisplayBytes;
bool gTopHeld;
std::deque<std::pair<int, unsigned>> setResults;
std::vector<unsigned> requestedRates;
std::vector<uint64_t> activeDemandsAtSet;
int getResult;
unsigned getRate;
}

int tegra_bpmp_emc_set(unsigned requested, unsigned *actual)
{
    requestedRates.push_back(requested);
    activeDemandsAtSet.push_back(gEMC->memoryState.owned[0] ? gEMC->memoryState.heads[0].peak_bytes_per_second : 0);
    if (setResults.empty()) {
        *actual = requested;
        return 0;
    }
    const auto answer = setResults.front();
    setResults.pop_front();
    *actual = answer.second;
    return answer.first;
}

int tegra_bpmp_emc_get(unsigned *actual)
{
    *actual = getRate;
    return getResult;
}

#include "../SwitchDisplayMemoryEMC.h"
#include "../SwitchEMCClock.h"
#include "../SwitchEMCFixed.h"

namespace {
using namespace switch_display;

switch_display_memory_head request(uint64_t bytes = 312000000)
{
    switch_display_memory_head head{};
    head.pixel_clock_hz = 78000000;
    head.active_width = 720;
    head.active_height = 1280;
    head.total_width = 800;
    head.total_height = 1320;
    head.peak_bytes_per_second = bytes;
    EXPECT_TRUE(memoryWindow(head.windows[0], bytes, 720, 1280, 720, 1280, false, false, false));
    return head;
}

class MemoryAdapter : public ::testing::Test {
protected:
    SwitchEMC emc;

    void SetUp() override
    {
        gEMC = &emc;
        gDisplayBytes = 312000000;
        setResults.clear();
        requestedRates.clear();
        activeDemandsAtSet.clear();
        gLines.clear();
        getResult = 0;
        getRate = 204000;
    }

    void TearDown() override
    {
        gEMC = nullptr;
    }
};

TEST_F(MemoryAdapter, RefusesUnqualifiedOrMalformedRequestsWithoutPublication)
{
    auto head = request();
    emc.memoryQualified = false;
    EXPECT_EQ(emc.reserveMemory(head), -95);
    emc.memoryQualified = true;
    head.total_width = 0;
    EXPECT_EQ(emc.reserveMemory(head), -28);
    EXPECT_FALSE(emc.memoryState.owned[0]);
    EXPECT_EQ(emc.mcRegs.writes, 0u);
    EXPECT_TRUE(requestedRates.empty());
}

TEST_F(MemoryAdapter, RaisesForNewDemandBeforePublishingIt)
{
    ASSERT_EQ(emc.reserveMemory(request()), 0);
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    ASSERT_EQ(requestedRates.size(), 1u);
    EXPECT_EQ(requestedRates[0], 1331200u);
    EXPECT_EQ(activeDemandsAtSet[0], 312000000u);
    EXPECT_EQ(emc.cur, 1u);
    EXPECT_EQ(emc.memoryFloor(0), 1u);
    EXPECT_EQ(gDisplayBytes, 2000000000u);
}

TEST_F(MemoryAdapter, EachRateMoveLogsOneLine)
{
    ASSERT_EQ(emc.reserveMemory(request()), 0);
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "SwitchEMC: 204000 -> 1331200 kHz, display memory asked 1331200 kHz; display floor 204000 kHz "
                         "for 312 MB/s");
}

TEST_F(MemoryAdapter, WarmIdenticalAdmissionDoesNoRegisterOrClockWork)
{
    const auto head = request();
    ASSERT_EQ(emc.reserveMemory(head), 0);
    const unsigned writes = emc.mcRegs.writes;
    for (unsigned i = 0; i != 100; ++i)
        ASSERT_EQ(emc.reserveMemory(head), 0);
    EXPECT_EQ(emc.mcRegs.writes, writes);
    EXPECT_TRUE(requestedRates.empty());
}

TEST_F(MemoryAdapter, RefusedClockLeavesOldDemandAndPolicy)
{
    const auto old = request();
    ASSERT_EQ(emc.reserveMemory(old), 0);
    setResults.push_back({-5, 0});
    EXPECT_EQ(emc.reserveMemory(request(2000000000)), -28);
    EXPECT_TRUE(emc.memoryState.same(old));
    EXPECT_EQ(emc.cur, 0u);
    EXPECT_EQ(emc.memoryAppliedKhz, 204000u);
    EXPECT_EQ(gDisplayBytes, 312000000u);
}

TEST_F(MemoryAdapter, RingCapacityRefusalDoesNotPublishDemand)
{
    const auto old = request();
    ASSERT_EQ(emc.reserveMemory(old), 0);
    emc.mcRegs.values[0x44c / 4] = 4095;
    const auto before = emc.mcRegs.values;
    EXPECT_EQ(emc.reserveMemory(request(400000000)), -28);
    EXPECT_TRUE(emc.memoryState.same(old));
    EXPECT_EQ(emc.mcRegs.values, before);
}

TEST_F(MemoryAdapter, UnknownSuccessfulRateIsRestoredBeforePublishingNewDemand)
{
    ASSERT_EQ(emc.reserveMemory(request()), 0);
    setResults.push_back({0, 1320000});
    setResults.push_back({0, 1600000});
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    ASSERT_EQ(requestedRates.size(), 2u);
    EXPECT_EQ(requestedRates[1], 1600000u);
    EXPECT_EQ(activeDemandsAtSet[1], 312000000u);
    EXPECT_EQ(emc.cur, 2u);
    EXPECT_EQ(emc.memoryAppliedKhz, 1600000u);
}

TEST_F(MemoryAdapter, UnknownResumeClockReconcilesBeforeActmonStarts)
{
    ASSERT_EQ(emc.reserveMemory(request()), 0);
    getRate = 1300000;
    EXPECT_EQ(SwitchEMC::governAction(&emc, nullptr, nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(emc.cur, 2u);
    EXPECT_EQ(emc.memoryAppliedKhz, 1600000u);
    EXPECT_EQ(emc.starts, 1u);
    EXPECT_TRUE(emc.governing);
}

TEST_F(MemoryAdapter, UnrecoverableClockTurnsDisplayAdmissionOffInsteadOfPanicking)
{
    ASSERT_EQ(emc.reserveMemory(request()), 0);
    getRate = 1300000;
    setResults.push_back({0, 1310000});
    EXPECT_NO_THROW(SwitchEMC::governAction(&emc, nullptr, nullptr, nullptr, nullptr));
    EXPECT_FALSE(emc.displayMemoryReady());
    EXPECT_FALSE(emc.memoryState.owned[0] || emc.memoryState.owned[1]);
    EXPECT_EQ(emc.reserveMemory(request()), -95);
    EXPECT_EQ(switch_emc_display_memory_ready(), 0);
}

TEST_F(MemoryAdapter, ResumeWhoseBpmpRefusesTheTopRateKeepsRunningWithoutAdmission)
{
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    EXPECT_EQ(switch_emc_display_memory_ready(), 1);
    getRate = 204000;
    setResults.push_back({-5, 204000});
    EXPECT_NO_THROW(SwitchEMC::governAction(&emc, &emc, nullptr, nullptr, nullptr));
    EXPECT_FALSE(emc.displayMemoryReady());
    EXPECT_FALSE(emc.memoryState.owned[0]);
}

TEST_F(MemoryAdapter, FailedResumeReadDoesNotUsePreviousRow)
{
    getResult = -5;
    EXPECT_EQ(SwitchEMC::governAction(&emc, nullptr, nullptr, nullptr, nullptr), kIOReturnError);
    EXPECT_FALSE(emc.governing);
    EXPECT_EQ(emc.starts, 0u);
    EXPECT_TRUE(requestedRates.empty());
}

TEST_F(MemoryAdapter, QuiescedResumeRestoresBeforeAnInsufficientKnownRateCanReachDisplay)
{
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    requestedRates.clear();
    getRate = 204000;
    ASSERT_EQ(SwitchEMC::governAction(&emc, &emc, nullptr, nullptr, nullptr), 0);
    ASSERT_EQ(requestedRates.size(), 1u);
    EXPECT_EQ(requestedRates[0], 1600000u);
    EXPECT_EQ(emc.cur, 2u);
    EXPECT_EQ(emc.memoryAppliedKhz, 1600000u);
    EXPECT_EQ(emc.starts, 1u);
}

TEST_F(MemoryAdapter, QuiescedResumeCanRecoverAFailedRateRead)
{
    getResult = -5;
    getRate = 0;
    ASSERT_EQ(SwitchEMC::governAction(&emc, &emc, nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(emc.cur, 2u);
    EXPECT_EQ(emc.starts, 1u);
}
}

TEST_F(MemoryAdapter, T210AdmitsAtTheBootChainsRateAndNeverAsksTheBpmp)
{
    ASSERT_TRUE(emc.useFixedRate(1600000));
    ASSERT_EQ(SwitchEMC::governAction(&emc, nullptr, nullptr, nullptr, nullptr), kIOReturnError);
    emc.carRate = 1600000;
    ASSERT_EQ(SwitchEMC::governAction(&emc, nullptr, nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(emc.starts, 0u);
    EXPECT_TRUE(gTopHeld);
    EXPECT_EQ(emc.reserveMemory(request(2000000000)), 0);
    EXPECT_GT(emc.mcRegs.writes, 0u);
    EXPECT_EQ(emc.reserveMemory(request(20000000000ULL)), -28);
    EXPECT_TRUE(requestedRates.empty());
    EXPECT_TRUE(emc.displayMemoryReady());
}

TEST_F(MemoryAdapter, T210ReplansAtTheRateWarmbootRestores)
{
    emc.carRate = 1600000;
    ASSERT_TRUE(emc.useFixedRate(emc.carRate));
    ASSERT_EQ(emc.reserveMemory(request(2000000000)), 0);
    emc.carRate = 204000;
    ASSERT_EQ(SwitchEMC::governAction(&emc, &emc, nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(emc.khz[0], 204000u);
    EXPECT_EQ(emc.count, 1u);
    EXPECT_FALSE(emc.displayMemoryReady());
    EXPECT_TRUE(requestedRates.empty());
}
