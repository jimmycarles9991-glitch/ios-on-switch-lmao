// switch_smmu_publish (TegraSMMUPublish.h) with the real TegraSMMU.cpp on a model of the MC's registers. A change to
// page entries cleans its table bytes and then flushes only their PTE groups from the PTC and only their IOVAs' 4 MiB
// sections from the ASID's TLB (Tegra X1 TRM 18.6.3.3; PTC_FLUSH 18.11.1.11, PTC_FLUSH_1 18.11.1.122, TLB_FLUSH
// 18.11.1.10), so other ASIDs keep their cached translations. A directory change flushes everything, as before; no
// change flushes nothing. Under the MC lock nothing locks, sleeps, waits or allocates, and every MC access holds it.
#include "smmu_flush_stubs.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <ostream>
#include <string>
#include <vector>

void switch_test_dcache(const char *op, uintptr_t line);
void switch_test_barrier();
#define SWITCH_DCACHE_OP(op, address) switch_test_dcache(op, address)
#define SWITCH_DCACHE_BARRIER()       switch_test_barrier()
#include "drivers/TegraSMMU/SwitchSmmuRegisters.h"
#include "drivers/TegraSMMU/TegraSMMUPublish.h"

namespace {
constexpr uintptr_t kMc = 0x70019000;
constexpr uint32_t kPtbAsid = 0x1c, kTlbFlush = 0x30, kPtcFlush = 0x34, kPtcFlushHi = 0x9b8;
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_VIC, kAsidMatch = (1u << 31) | (kAsid << 24), kSectionMatch = 2;
constexpr uint64_t kTablesPa = 0xe1000000, kWindow = 0x10000000, kWindowBytes = 128ULL << 20, kSection = 4ULL << 20;
constexpr uint32_t kTables = 1 + 32 + 2;  // the directory, the window's 32 tables (pages 1..32), two more

struct Event {
    std::string kind;  // "write"/"read" (MC offset), "cvac"/"civac" (cache line), "dsb"
    uint64_t at;
    uint32_t value;

    bool operator==(const Event &) const = default;
};

void PrintTo(const Event &e, std::ostream *os)
{
    *os << e.kind << " " << std::hex << e.at << " " << e.value;
}

std::vector<Event> gEvents;
std::map<uint64_t, uint32_t> gRegisters;  // by MC offset
unsigned gUnlocked;                       // MC accesses made without the MC lock
alignas(4096) uint32_t gTables[kTables * 1024];

std::vector<Event> mcAccesses()
{
    std::vector<Event> out;
    std::copy_if(gEvents.begin(), gEvents.end(), std::back_inserter(out), [](const Event &e) {
        return e.kind == "write" || e.kind == "read";
    });
    return out;
}

Event mcWrite(uint32_t offset, uint32_t value)
{
    return {"write", offset, value};
}

Event mcRead(uint32_t offset)
{
    return {"read", offset, 0};
}

uint32_t section(uint64_t iova)
{
    return kAsidMatch | static_cast<uint32_t>(iova >> 12) | kSectionMatch;
}

class SmmuFlushRange : public testing::Test {
protected:
    TegraSMMUDomain domain;

    // Set up as an engine does: tables installed, cleaned and attached (the attach flushes all).
    void SetUp() override
    {
        ASSERT_TRUE(domain.init(gTables, kTablesPa, kTables));
        ASSERT_TRUE(domain.reserve(kWindow, kWindowBytes));
        uintptr_t low = 0;
        uintptr_t high = 0;
        domain.takeDirty(&low, &high);
        gEvents.clear();
        gRegisters.clear();
        gRegisters[kPtcFlushHi] = 3;  // whatever wrote it last
        gUnlocked = 0;
        switch_test_under_spinlock.clear();
    }

    uintptr_t entryVa(uint64_t iova)
    {
        return reinterpret_cast<uintptr_t>(domain.pte(iova, false));
    }
};

// 768 entries 2 MiB into the window: entries 512..1023 of table 1 and 0..255 of table 2, one run of 3072 bytes.
TEST_F(SmmuFlushRange, AThreeMiBMapFlushesItsPteGroupsAndTwoSectionsOnly)
{
    std::vector<uint64_t> pages(768);
    for (uint32_t i = 0; i < pages.size(); ++i)
        pages[i] = 0x90000000ULL + i * 4096ULL;
    const uint64_t iova = kWindow + (2ULL << 20);
    ASSERT_TRUE(domain.map(iova, pages.data(), 768, true));
    const uintptr_t low = entryVa(iova), high = entryVa(iova + 767 * 4096ULL) + 4;
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);

    const uint64_t first = kTablesPa + 4096 + 512 * 4, end = kTablesPa + 2 * 4096 + 256 * 4;
    ASSERT_EQ(end - first, 768u * 4);
    std::vector<Event> expected = {mcWrite(kPtcFlushHi, 0)};
    for (uint64_t at = first; at < end; at += 32)
        expected.push_back(mcWrite(kPtcFlush, static_cast<uint32_t>(at) | 1));
    expected.push_back(mcWrite(kTlbFlush, section(kWindow)));
    expected.push_back(mcWrite(kTlbFlush, section(kWindow + kSection)));
    expected.push_back(mcRead(kPtbAsid));
    EXPECT_EQ(mcAccesses(), expected);
    EXPECT_EQ(expected.size(), 1u + 96u + 2u + 1u);
    for (const Event &e : gEvents)
        EXPECT_FALSE(e.kind == "write" && e.at == kPtcFlush && !(e.value & 1)) << "a PTC flush of TYPE ALL";

    std::vector<Event> cleaned;
    for (uintptr_t line = low & ~uintptr_t{63}; line < high; line += 64)
        cleaned.push_back({"cvac", line, 0});
    cleaned.push_back({"dsb", 0, 0});
    ASSERT_GE(gEvents.size(), cleaned.size());
    EXPECT_EQ(std::vector<Event>(gEvents.begin(), gEvents.begin() + static_cast<long>(cleaned.size())), cleaned)
        << "the table bytes reach memory before the first flush";
}

// The USB host's and the display's choice: clean and invalidate; one entry is one PTE group and one section.
TEST_F(SmmuFlushRange, AnUnmapCleansAndInvalidatesAndFlushesItsOneGroup)
{
    const uint64_t page = 0x90000000, iova = kWindow + (5ULL << 20);
    ASSERT_TRUE(domain.map(iova, &page, 1, true));
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);
    gEvents.clear();
    domain.unmap(iova, 1);
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN_INVALIDATE);
    const std::vector<Event> expected = {
        {"civac", entryVa(iova) & ~uintptr_t{63}, 0},
        {"dsb", 0, 0},
        mcWrite(kPtcFlushHi, 0),
        mcWrite(kPtcFlush, static_cast<uint32_t>(kTablesPa + 2 * 4096 + 256 * 4) | 1),
        mcWrite(kTlbFlush, section(kWindow + kSection)),
        mcRead(kPtbAsid),
    };
    EXPECT_EQ(gEvents, expected);
}

// A table installed for a section that had none: the directory changed, so the whole PTC and the ASID's TLB go.
TEST_F(SmmuFlushRange, ADirectoryChangeStillFlushesEverything)
{
    const uint64_t page = 0x90000000;
    ASSERT_TRUE(domain.map(0x40000000, &page, 1, true));
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);
    const std::vector<Event> expected = {mcWrite(kPtcFlush, 1u << 3), mcRead(kPtbAsid), mcWrite(kTlbFlush, kAsidMatch),
                                         mcRead(kPtbAsid)};
    EXPECT_EQ(mcAccesses(), expected);
}

TEST_F(SmmuFlushRange, NothingChangedFlushesNothing)
{
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN_INVALIDATE);
    domain.unmap(kWindow, 16);
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN_INVALIDATE);
    EXPECT_TRUE(gEvents.empty()) << gEvents.size() << " cache or register accesses";

    const uint64_t page = 0x90000000;
    ASSERT_TRUE(domain.map(kWindow, &page, 1, true));
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);
    gEvents.clear();
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);
    EXPECT_TRUE(gEvents.empty()) << "a change published twice";
}

TEST_F(SmmuFlushRange, UnderTheMcLockNothingLocksSleepsWaitsOrAllocates)
{
    std::vector<uint64_t> pages(1024);
    for (uint32_t i = 0; i < pages.size(); ++i)
        pages[i] = 0x90000000ULL + i * 4096ULL;
    ASSERT_TRUE(domain.map(kWindow + (3ULL << 20), pages.data(), 1024, true));
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);
    domain.unmap(kWindow + (3ULL << 20), 1024);
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN_INVALIDATE);
    ASSERT_TRUE(domain.map(0x40000000, pages.data(), 1, true));
    switch_smmu_publish(domain, kAsid, SWITCH_DCACHE_CLEAN);

    std::string calls;
    for (const std::string &call : switch_test_under_spinlock)
        calls += call + " ";
    EXPECT_TRUE(switch_test_under_spinlock.empty()) << "under the MC lock: " << calls;
    EXPECT_EQ(gUnlocked, 0u) << "MC accesses without the MC lock";
    EXPECT_EQ(switch_test_spinlocks_held, 0u);
    EXPECT_GT(mcAccesses().size(), 128u + 2u);
}

// The stand-ins catch what they are for: each call that can block, made under a simple lock, is named.
TEST(SmmuFlushStubs, ABlockingCallUnderASpinlockIsNamed)
{
    switch_test_under_spinlock.clear();
    IOSimpleLock spin{};
    IOLock lock{};
    IOSimpleLockLock(&spin);
    IOFree(IOMalloc(16), 16);
    IOLockLock(&lock);
    IOLockUnlock(&lock);
    IOSleep(1);
    IODelay(1);
    IOSimpleLockLock(&spin);
    IOSimpleLockUnlock(&spin);
    IOSimpleLockUnlock(&spin);
    const std::vector<std::string> expected = {"IOMalloc", "IOFree",  "IOLockLock",      "IOLockUnlock",
                                               "IOSleep",  "IODelay", "IOSimpleLockLock"};
    EXPECT_EQ(switch_test_under_spinlock, expected);
    EXPECT_EQ(switch_test_spinlocks_held, 0u);
    switch_test_under_spinlock.clear();
}

// The fields on their own: PTC_FLUSH_1 holds PA[33:32] and PTC_FLUSH_ADR PA[31:4] of each 32-byte group the range
// touches; the TLB's section match carries VA[31:22] in VA_GROUP's bits 19:10, up to the last section below 4 GiB.
TEST(SmmuFlushRangeFields, HighPhysicalBitsAndTheLastSection)
{
    struct Recorder {
        std::vector<Event> events;

        uint32_t read(uint32_t offset)
        {
            events.push_back({"read", offset, 0});
            return 0;
        }

        void write(uint32_t offset, uint32_t value)
        {
            events.push_back({"write", offset, value});
        }
    } io;

    switch_smmu::flushRange(io, 122, 0x240000010ULL, 40, 0xffc00000ULL, kSection);
    const std::vector<Event> expected = {
        mcWrite(kPtcFlushHi, 2),
        mcWrite(kPtcFlush, 0x40000000u | 1),
        mcWrite(kPtcFlush, 0x40000020u | 1),
        mcWrite(kTlbFlush, (1u << 31) | (122u << 24) | 0xffc00u | kSectionMatch),
        mcRead(kPtbAsid),
    };
    EXPECT_EQ(io.events, expected);
}

// An entry in the upper half of a 64-byte group: the group's own address is flushed too, in case a PTE group is the
// 16 entries 18.6.3.3 names rather than the 32-byte atom.
TEST(SmmuFlushRangeFields, AnEntryInAGroupsUpperHalfFlushesTheGroupsBase)
{
    struct Recorder {
        std::vector<Event> events;

        uint32_t read(uint32_t offset)
        {
            events.push_back({"read", offset, 0});
            return 0;
        }

        void write(uint32_t offset, uint32_t value)
        {
            events.push_back({"write", offset, value});
        }
    } io;

    switch_smmu::flushRange(io, 127, 0x80001024ULL, 4, 0x10000000ULL, 4096);
    const std::vector<Event> expected = {
        mcWrite(kPtcFlushHi, 0),
        mcWrite(kPtcFlush, 0x80001000u | 1),
        mcWrite(kPtcFlush, 0x80001020u | 1),
        mcWrite(kTlbFlush, (1u << 31) | (127u << 24) | 0x10000u | kSectionMatch),
        mcRead(kPtbAsid),
    };
    EXPECT_EQ(io.events, expected);
}
}  // namespace

void switch_test_dcache(const char *op, uintptr_t line)
{
    gEvents.push_back({op, line, 0});
}

void switch_test_barrier()
{
    gEvents.push_back({"dsb", 0, 0});
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    if (!switch_test_spinlocks_held)
        ++gUnlocked;
    const uint64_t offset = address - kMc;
    gEvents.push_back(mcRead(static_cast<uint32_t>(offset)));
    return gRegisters[offset];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    if (!switch_test_spinlocks_held)
        ++gUnlocked;
    const uint64_t offset = address - kMc;
    gEvents.push_back(mcWrite(static_cast<uint32_t>(offset), value));
    gRegisters[offset] = value;
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

void switch_mc_errors_resume()
{}

void switch_mc_errors_detached(uint32_t)
{}
}
