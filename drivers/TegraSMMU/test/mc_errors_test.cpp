// The memory controller's error interrupt (TegraMCErrors.cpp, TegraMCErrors.h) on a register file: the filter runs with
// interrupts off and takes, then clears, only the error bits (Tegra X1 TRM 18.11.1.1-4); a client that went where it
// may not panics there with one line naming the kind, client, access and address; the dropped swgroup write is logged
// by the action, which publishes the error that GM20B's and the engines' lines read.
#include "mc_errors_stubs.h"
#include "drivers/TegraSMMU/TegraMCErrors.h"
#include "drivers/TegraSMMU/TegraSMMU.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uintptr_t kMc = 0x70019000;
std::map<uintptr_t, uint32_t> gRegisters;
std::vector<std::pair<uint32_t, uint32_t>> gWrites;
std::vector<uint32_t> gReads;

uint32_t &mc(uint32_t offset)
{
    return gRegisters[kMc + offset];
}

unsigned gNubAsks;
bool gIntmaskStuck;
uint32_t gIntmaskDrops;  // INTMASK bits a write does not set
bool gStartReturnedBeforeTheGic;
std::vector<std::string> gLatchedLines;

constexpr uint32_t kDroppedAsidWrite = 1u << 11;

// The start, then (the GIC up) its thread, with a dropped swgroup write latched before either.
IOFilterInterruptEventSource *started()
{
    static IOFilterInterruptEventSource *source = [] {
        mc(0x0) = kDroppedAsidWrite;
        mc(0x8) = 0x6a00006d;
        mc(0xc) = 0x1000;
        EXPECT_EQ(tegra_mc_errors_start(), 0);
        gStartReturnedBeforeTheGic =
            gNubAsks == 0 && IOFilterInterruptEventSource::last() == nullptr && switch_test_threads.size() == 1;
        if (switch_test_threads.size() == 1)
            switch_test_threads[0].first(switch_test_threads[0].second, 0);
        gLatchedLines = switch_test_lines;
        return IOFilterInterruptEventSource::last();
    }();
    return source;
}

// The panic the filter raised for the error now in the registers, or "" when it did not panic.
std::string panicked()
{
    try {
        started()->fire();
    } catch (const switch_test_panic &p) {
        EXPECT_FALSE(switch_test_interrupts_off);
        return p.what();
    }
    return "";
}

void dropped()
{
    mc(0x0) = kDroppedAsidWrite;
    ASSERT_TRUE(started()->fire());
}

class McErrors : public testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(started(), nullptr);
        gRegisters.clear();
        gWrites.clear();
        gReads.clear();
        switch_test_lines.clear();
    }
};

TEST_F(McErrors, TheMaskUnmasksTheErrorsButNotTheArbitrationWarningAndSleepRestoresIt)
{
    EXPECT_TRUE(started()->enabled);
    switch_mc_errors_resume();
    EXPECT_EQ(mc(0x4), 1u << 6 | 1u << 8 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 16 | 1u << 17);
}

// Boot 19's capture: ERR_STATUS 0x20000080 at 0, DECERR_EMEM by client 0x80 csr_sesrd.
TEST_F(McErrors, NoDramAtTheAddressPanicsInTheFilterAfterTheCaptureIsTaken)
{
    mc(0x0) = 1u << 6 | 1u << 9;
    mc(0x8) = 0x20000080;
    mc(0xc) = 0;
    EXPECT_EQ(panicked(), "TegraSMMU: MC error DECERR_EMEM: DECERR_EMEM (no DRAM at the address), a read by csr_sesrd "
                          "(SE) (client 0x80) at 0; INTSTATUS 0x40 ERR_STATUS 0x20000080 ERR_ADR 0");
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0], std::make_pair(0x0u, 1u << 6))
        << "only the error bit is cleared, ARBITRATION_EMEM is not ours";
    EXPECT_EQ(gReads, (std::vector<uint32_t>{0x0, 0x8, 0xc})) << "ERR_STATUS and ERR_ADR before the clear";
    EXPECT_TRUE(switch_test_lines.empty());
}

// The probe log's VIC fault: INVALID_SMMU_PAGE, ERR_STATUS 0x6000006c at 0x4001d400, a read by csr_vicsrd.
TEST_F(McErrors, AnSmmuFaultPanicsNamingThePagesPermissions)
{
    mc(0x0) = 1u << 10;
    mc(0x8) = 0x6a00006c;
    mc(0xc) = 0x4001d400;
    EXPECT_EQ(panicked(), "TegraSMMU: MC error INVALID_SMMU_PAGE: INVALID_SMMU_PAGE (page readable 1 writable 0 "
                          "nonsecure 1), a read by csr_vicsrd (VIC) (client 0x6c) at 0x4001d400; INTSTATUS 0x400 "
                          "ERR_STATUS 0x6a00006c ERR_ADR 0x4001d400");
}

TEST_F(McErrors, AnSdCardWriteToAPageNotMappedForItPanicsWithItsAddress)
{
    mc(0x0) = 1u << 10;
    mc(0x8) = 0x6c110064;
    mc(0xc) = 0x2345000;
    EXPECT_EQ(panicked(), "TegraSMMU: MC error INVALID_SMMU_PAGE: INVALID_SMMU_PAGE (page readable 1 writable 1 "
                          "nonsecure 0), a write by csw_sdmmcwa (SDMMC1) (client 0x64) at 0x102345000; INTSTATUS 0x400 "
                          "ERR_STATUS 0x6c110064 ERR_ADR 0x2345000");
}

TEST_F(McErrors, EveryKindButTheDroppedSwgroupWritePanics)
{
    for (uint32_t bit = 0; bit < 32; ++bit) {
        if (!(switch_mc::kErrors & (1u << bit)))
            continue;
        gRegisters.clear();
        switch_test_lines.clear();
        mc(0x0) = 1u << bit;
        mc(0x8) = 0x6a00006c;
        const std::string text = panicked();
        if (bit == 11) {
            EXPECT_EQ(text, "");
            EXPECT_EQ(switch_test_lines.size(), 1u);
        } else {
            EXPECT_EQ(text.rfind("TegraSMMU: MC error ", 0), 0u) << "INTSTATUS bit " << bit;
        }
    }
}

TEST_F(McErrors, ADroppedSwgroupWriteIsOneLineAndPublished)
{
    dropped();
    ASSERT_EQ(switch_test_lines.size(), 1u);
    EXPECT_EQ(switch_test_lines[0], "TegraSMMU: MC error INVALID_APB_ASID_UPDATE (not captured in MC_ERR_STATUS); "
                                    "INTSTATUS 0x800\n");
    uint32_t intstatus = 0;
    uint32_t status = 0;
    uint32_t address = 0;
    EXPECT_GE(tegra_mc_error_latest(&intstatus, &status, &address), 1u);
    EXPECT_EQ(intstatus, kDroppedAsidWrite);
}

TEST_F(McErrors, AnInterruptWithNoErrorBitTakesNothing)
{
    mc(0x0) = 1u << 9;
    EXPECT_FALSE(started()->fire());
    EXPECT_TRUE(gWrites.empty());
    EXPECT_TRUE(switch_test_lines.empty());
}

// The start waits for nothing: its thread asks for the GIC's nub, attaches, and takes the error latched before it.
TEST(McErrorsStart, ReturnsBeforeTheGicAndItsThreadTakesWhatLatched)
{
    ASSERT_NE(started(), nullptr);
    EXPECT_TRUE(gStartReturnedBeforeTheGic) << "the start asked for the GIC's nub or attached on the caller's thread";
    EXPECT_EQ(gNubAsks, 1u);
    EXPECT_TRUE(started()->enabled);
    ASSERT_EQ(gLatchedLines.size(), 1u);
    EXPECT_NE(gLatchedLines[0].find("INVALID_APB_ASID_UPDATE"), std::string::npos);
}

// A storm of dropped swgroup writes: the first four in full, then one count line at SC7.
TEST_F(McErrors, AStormOfDroppedWritesPrintsFourLinesAndACountAtSc7)
{
    switch_mc_errors_resume();
    switch_test_lines.clear();
    for (uint32_t i = 0; i < 20; ++i)
        dropped();
    EXPECT_EQ(switch_test_lines.size(), 4u);
    switch_mc_errors_resume();
    ASSERT_EQ(switch_test_lines.size(), 5u);
    EXPECT_EQ(switch_test_lines[4].rfind("TegraSMMU: MC error run over: 20 errors of kinds MC_ERR_STATUS does not "
                                         "capture, the first 4 printed, ended by SC7; the last ",
                                         0),
              0u)
        << switch_test_lines[4];
}

switch_mc::Error smmuFault(uint32_t client, uint32_t address)
{
    return {1u << 10, 0x6a000000 | client, address, 0, 0, 0};
}

// A run of one client: its first four in full, then one count line once its swgroup detaches.
TEST(McErrorRuns, AClientsRunPrintsItsFirstFourThenEndsWithItsCount)
{
    switch_mc::Runs runs;
    switch_mc::Runs::Ended ended[switch_mc::Runs::kOpen];
    unsigned count = 0;
    unsigned full = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        full += runs.error(smmuFault(0x7e, 0x2000 + i * 0x1000), ended, count) ? 1 : 0;
        EXPECT_EQ(count, 0u);
    }
    EXPECT_EQ(full, 4u);
    const auto nvjpg = [](uint32_t client) {
        return client == 0x7e || client == 0x7f;
    };
    const auto vic = [](uint32_t client) {
        return client == 0x6c || client == 0x6d;
    };
    EXPECT_EQ(runs.endWhere(vic, switch_mc::Runs::Cause::kDetach, ended), 0u);
    ASSERT_EQ(runs.endWhere(nvjpg, switch_mc::Runs::Cause::kDetach, ended), 1u);
    char line[512];
    const std::string text = switch_mc::describeRun(ended[0], line, sizeof(line));
    EXPECT_EQ(text.rfind("50 errors by csr_nvjpgsrd (NVJPG) (client 0x7e), the first 4 printed, ended by detach; the "
                         "last INVALID_SMMU_PAGE",
                         0),
              0u)
        << text;
    EXPECT_NE(text.find("at 0x33000"), std::string::npos) << "the run's last error";
    EXPECT_TRUE(runs.error(smmuFault(0x7e, 0x1000), ended, count)) << "a new run prints in full";
    EXPECT_EQ(runs.endWhere(nvjpg, switch_mc::Runs::Cause::kDetach, ended), 0u) << "within four lines: no count";
}

// A client goes quiet once 16 errors by others came after its last; SC7 ends every run.
TEST(McErrorRuns, AQuietClientsRunEndsAndSc7EndsTheRest)
{
    switch_mc::Runs runs;
    switch_mc::Runs::Ended ended[switch_mc::Runs::kOpen];
    unsigned count = 0;
    unsigned endedTotal = 0;
    for (uint32_t i = 0; i < 6; ++i)
        runs.error(smmuFault(0x78, 0x1000), ended, count);
    for (uint32_t i = 0; i < 17; ++i) {
        runs.error(smmuFault(0x79, 0x1000), ended, count);
        endedTotal += count;
        if (count)
            EXPECT_EQ(ended[0].client, 0x78u);
    }
    EXPECT_EQ(endedTotal, 1u);
    EXPECT_EQ(ended[0].cause, switch_mc::Runs::Cause::kDisplaced);
    EXPECT_EQ(ended[0].errors, 6u);
    const auto all = [](uint32_t) {
        return true;
    };
    ASSERT_EQ(runs.endWhere(all, switch_mc::Runs::Cause::kSC7, ended), 1u);
    EXPECT_EQ(ended[0].client, 0x79u);
}

TEST(McErrorsDescribe, AKindWithItsOwnCaptureRegistersNamesItsClientAndAddressFromThem)
{
    char line[320];
    EXPECT_STREQ(switch_mc::describe({1u << 12, 0, 0, 1u << 12, 0x0011006c, 0x81000000}, line, sizeof(line)),
                 "DECERR_VPR: DECERR_VPR: a write by csr_vicsrd (VIC) (client 0x6c) at 0x181000000; INTSTATUS 0x1000 "
                 "ERR_VPR_STATUS 0x11006c ERR_VPR_ADR 0x81000000");
    EXPECT_STREQ(switch_mc::describe({1u << 12, 0, 0, 0, 0, 0}, line, sizeof(line)),
                 "DECERR_VPR (not captured in MC_ERR_STATUS); INTSTATUS 0x1000");
}

// MC_ERR_SEC_STATUS 0x67c and ADR 0x680 (TRM 18.11.1.101-102) read before the clear; the client is 6:0 there.
TEST_F(McErrors, ASecCarveoutViolationPanicsWithItsOwnCapture)
{
    mc(0x0) = 1u << 13;
    mc(0x67c) = 0x00000080 | 0x16;
    mc(0x680) = 0x80123000;
    EXPECT_EQ(panicked(),
              "TegraSMMU: MC error SECERR_SEC: SECERR_SEC: a read by csr_host1xdmar (host1x) (client 0x16) at "
              "0x80123000; INTSTATUS 0x2000 ERR_SEC_STATUS 0x96 ERR_SEC_ADR 0x80123000");
    EXPECT_EQ(gReads, (std::vector<uint32_t>{0x0, 0x8, 0xc, 0x67c, 0x680}));
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0], std::make_pair(0x0u, 1u << 13)) << "cleared after every capture was read";
}

TEST_F(McErrors, AnSmmuFaultAndAnMtsViolationTogetherNameBoth)
{
    mc(0x0) = 1u << 10 | 1u << 16;
    mc(0x8) = 0x6a00006c;
    mc(0xc) = 0x4001d400;
    mc(0x9b0) = 0x00010064;
    mc(0x9b4) = 0x90000000;
    EXPECT_EQ(panicked(),
              "TegraSMMU: MC error INVALID_SMMU_PAGE+DECERR_MTS: INVALID_SMMU_PAGE (page readable 1 "
              "writable 0 nonsecure 1), a read by csr_vicsrd (VIC) (client 0x6c) at 0x4001d400; DECERR_MTS: "
              "a write by csw_sdmmcwa (SDMMC1) (client 0x64) at 0x90000000; INTSTATUS 0x10400 ERR_STATUS "
              "0x6a00006c ERR_ADR 0x4001d400 ERR_MTS_STATUS 0x10064 ERR_MTS_ADR 0x90000000");
}

// A write to INTMASK that does not take leaves every confined client's faults unreported: that panics.
// Linux's tegra210 mask leaves bit 17 out: a bit 17 that does not stick is one line, the rest of the mask is required.
TEST_F(McErrors, ACarveoutBitThatDoesNotStickIsOneLineNotAPanic)
{
    gIntmaskDrops = 1u << 17;
    switch_mc_errors_resume();
    gIntmaskDrops = 0;
    EXPECT_EQ(mc(0x4), 0x13d40u);
    ASSERT_EQ(switch_test_lines.size(), 1u);
    EXPECT_EQ(switch_test_lines[0], "TegraSMMU: MC INTMASK keeps 0x13d40 of 0x33d40: DECERR_GENERALIZED_CARVEOUT stays "
                                    "masked\n");
    gIntmaskDrops = 1u << 13;
    try {
        switch_mc_errors_resume();
        ADD_FAILURE() << "no panic";
    } catch (const switch_test_panic &p) {
        EXPECT_EQ(std::string(p.what()),
                  "TegraSMMU: MC error interrupt (INTID 109) masked: INTMASK reads 0x31d40 after "
                  "0x33d40 was written");
    }
    gIntmaskDrops = 0;
}

TEST_F(McErrors, AnIntmaskThatDoesNotTakePanics)
{
    gIntmaskStuck = true;
    try {
        switch_mc_errors_resume();
        ADD_FAILURE() << "no panic";
    } catch (const switch_test_panic &p) {
        EXPECT_EQ(std::string(p.what()), "TegraSMMU: MC error interrupt (INTID 109) masked: INTMASK reads 0 after "
                                         "0x33d40 was written");
    }
    gIntmaskStuck = false;
}

// TegraVIC's MC fault probe: VIC reads a page its directory leaves empty, at 0x08000000 (below its windows).
constexpr uint32_t kVicRead = 0x6c, kFaultLow = 0x08000000, kFaultHigh = 0x08010000;

void vicFaultLatched(uint32_t client = kVicRead, uint32_t address = kFaultLow + 0x40)
{
    mc(0x0) = 1u << 10;
    mc(0x8) = 0x6a000000 | client;
    mc(0xc) = address;
}

class McExpectedFault : public McErrors {
protected:
    void TearDown() override
    {
        switch_test_while_blocked = nullptr;
        switch_test_blocks = 0;
    }
};

TEST_F(McExpectedFault, IsTakenWithoutAPanicOrALineAndCountedAtTheEnd)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    vicFaultLatched();
    EXPECT_EQ(panicked(), "");
    EXPECT_TRUE(switch_test_lines.empty());
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(switch_test_blocks, 0u) << "already delivered: no wait";
    EXPECT_EQ(out.attached, 1u);
    EXPECT_EQ(out.taken, 1u);
    EXPECT_EQ(out.intstatus, 1u << 10);
    EXPECT_EQ(out.status, 0x6a00006cu);
    EXPECT_EQ(out.address, kFaultLow + 0x40);
    uint32_t intstatus = 0, status = 0, address = 0;
    const uint32_t before = tegra_mc_error_latest(&intstatus, &status, &address);
    vicFaultLatched();
    EXPECT_NE(panicked(), "") << "the expectation is over";
    EXPECT_EQ(tegra_mc_error_latest(&intstatus, &status, &address), before) << "the expected one was not published";
}

TEST_F(McExpectedFault, DecerrEmemAlongsideSmmuCaptureIsTakenAsExpected)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    mc(0x0) = 1u << 10 | 1u << 6;
    mc(0x8) = 0x60000000 | kVicRead;
    mc(0xc) = kFaultLow;
    EXPECT_EQ(panicked(), "");
    EXPECT_EQ(mc(0x0), 0u) << "both INVALID_SMMU_PAGE and DECERR_EMEM are cleared";
    EXPECT_TRUE(switch_test_lines.empty());
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(out.taken, 1u);
    EXPECT_EQ(out.intstatus, 1u << 10 | 1u << 6);
    EXPECT_EQ(out.status, 0x6000006cu);
    EXPECT_EQ(out.address, kFaultLow);
}

TEST_F(McExpectedFault, AnyOtherClientOrAddressStillPanics)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    vicFaultLatched(0x6d);
    EXPECT_NE(panicked(), "");
    vicFaultLatched(kVicRead, kFaultHigh);
    EXPECT_NE(panicked(), "");
    mc(0x0) = 1u << 6;
    mc(0x8) = 0x6000006c;
    mc(0xc) = kFaultLow;
    EXPECT_NE(panicked(), "") << "DECERR_EMEM alone without INVALID_SMMU_PAGE";
    mc(0x0) = 1u << 10 | 1u << 6;
    mc(0x8) = 0x2000006c;
    mc(0xc) = kFaultLow;
    EXPECT_NE(panicked(), "") << "DECERR_EMEM captured instead of INVALID_SMMU_PAGE";
    mc(0x0) = 1u << 10 | 1u << 8;
    mc(0x8) = 0x6a00006c;
    mc(0xc) = kFaultLow;
    EXPECT_NE(panicked(), "") << "an SMMU fault together with another kind";
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(out.taken, 0u);
}

TEST_F(McExpectedFault, TheEndWaitsForTheInterruptOnItsWay)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    vicFaultLatched();
    switch_test_while_blocked = [] {
        (void)started()->fire();
    };
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(switch_test_blocks, 1u);
    EXPECT_EQ(out.taken, 1u);
}

TEST_F(McExpectedFault, ASecondFaultLatchedBehindTheFirstIsWaitedForToo)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    vicFaultLatched();
    EXPECT_EQ(panicked(), "");
    vicFaultLatched(kVicRead, kFaultLow + 0x1000);
    switch_test_while_blocked = [] {
        (void)started()->fire();
    };
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(out.taken, 2u);
    EXPECT_EQ(out.address, kFaultLow + 0x40) << "the first";
}

TEST_F(McExpectedFault, ALatchedFaultTheInterruptNeverDeliveredPanicsAsLost)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    vicFaultLatched();
    switch_mc_expected out;
    try {
        switch_mc_errors_expect_end(100, &out);
        ADD_FAILURE() << "no panic";
    } catch (const switch_test_panic &p) {
        EXPECT_EQ(std::string(p.what()),
                  "TegraSMMU: MC error interrupt (INTID 109) lost: INTSTATUS 0x400 still latched "
                  "100 ms after a deliberate SMMU fault, 0 expected errors delivered");
    }
    gRegisters.clear();
    switch_mc_errors_expect_end(0, &out);
}

TEST_F(McExpectedFault, NoFaultAtAllEndsWithNothingTaken)
{
    switch_mc_errors_expect(kVicRead, kFaultLow, kFaultHigh);
    switch_mc_expected out;
    switch_mc_errors_expect_end(100, &out);
    EXPECT_EQ(out.attached, 1u);
    EXPECT_EQ(out.taken, 0u);
}
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    gReads.push_back(static_cast<uint32_t>(address - kMc));
    return gRegisters[address];
}

// INTSTATUS's bits clear where a 1 is written (TRM 18.11.1.1).
void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gWrites.emplace_back(static_cast<uint32_t>(address - kMc), value);
    if (gIntmaskStuck && address == kMc + 0x4)
        return;
    if (address == kMc + 0x4)
        value &= ~gIntmaskDrops;
    gRegisters[address] = address == kMc ? gRegisters[address] & ~value : value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
    return static_cast<uint16_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
    switch_mmio_model_write32(address, value);
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
    return static_cast<uint8_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
    switch_mmio_model_write32(address, value);
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
}

// NOLINTNEXTLINE(misc-use-internal-linkage): the driver under test links to this symbol
IOService *switch_interrupt_nub(uint32_t, bool)
{
    switch_test_may_block("switch_interrupt_nub");
    ++gNubAsks;
    static IOService nub;
    return &nub;
}
