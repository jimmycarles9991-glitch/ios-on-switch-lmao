// UsbStaticMap.h (UsbSmmuSpace.cpp) on the host, with the Switch's banks: the loader's ledger read as the loader
// writes it; the device controller's space, all of RAM in 1023 sections and one table; the host's, the low bank at its
// own address with the firmware image read only and the 64 MiB window below RAM. Every fixed address translates back
// to its own through the tables built; a space that cannot be built is refused.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "../../../boot/loader/ram.h"
#include "../UsbIova.h"
#include "../UsbStaticMap.h"

namespace {

constexpr uint64_t kGiB4 = 1ULL << 32;
// The banks every committed ledger has (kernel/switch/include/switch/ram.h SWITCH_RAM_BANK*).
constexpr UsbRam kRam = {0x8000'0000, 0xffd0'0000, 0x1'0000'0000, 0x1'8000'0000};
constexpr UsbStaticMap kDevice = UsbStaticMap::device(kRam);
constexpr uint64_t kFirmwarePa = 0x9f80'1000;
constexpr uint64_t kFirmwareBytes = 120ULL * 1024;
constexpr UsbStaticMap kHost = UsbStaticMap::host(kRam, kFirmwarePa, kFirmwareBytes);

struct Space {
    std::vector<uint32_t> tables;
    TegraSMMUDomain domain;
    uint32_t sections = 0, tablesInstalled = 0;

    explicit Space(const UsbStaticMap &map) : tables(static_cast<size_t>(1 + map.tables()) * TegraSMMUDomain::kEntries)
    {
        EXPECT_TRUE(domain.init(tables.data(), 0x9000'0000, 1 + map.tables()));
        EXPECT_TRUE(map.install(domain));
        for (uint32_t i = 0; i < TegraSMMUDomain::kEntries; ++i) {
            const uint32_t pde = domain.va[i];
            sections += (pde & TegraSMMUDomain::kRead) != 0u && (pde & TegraSMMUDomain::kNext) == 0u ? 1 : 0;
            tablesInstalled += (pde & TegraSMMUDomain::kNext) != 0u ? 1 : 0;
        }
    }

    bool writable(uint64_t iova)
    {
        const uint32_t *pte = domain.pte(iova, false);
        return pte != nullptr && (*pte & TegraSMMUDomain::kWrite) != 0u;
    }
};

}  // namespace

// fromLedger reads the loader's own struct: the banks of a committed ledger, nothing from any other.
TEST(UsbStaticMap, LedgerIsTheLoadersLayout)
{
    static_assert(sizeof(switch_ram_ledger) == 1584, "the ledger kernel/switch/ram/ram.c reads");
    switch_ram_ledger ledger = {
        SWITCH_RAM_MAGIC, 1, 3, 1, {{0x8000'0000, 0xffd0'0000}, {0x1'0000'0000, 0x1'8000'0000}}, {}};
    UsbRam ram = {};
    ASSERT_TRUE(UsbRam::fromLedger(&ledger, sizeof(ledger), &ram));
    EXPECT_EQ(ram.lowStart, kRam.lowStart);
    EXPECT_EQ(ram.lowEnd, kRam.lowEnd);
    EXPECT_EQ(ram.highStart, kRam.highStart);
    EXPECT_EQ(ram.highEnd, kRam.highEnd);
    EXPECT_FALSE(UsbRam::fromLedger(&ledger, 47, &ram)) << "short";
    ledger.committed = 0;
    EXPECT_FALSE(UsbRam::fromLedger(&ledger, sizeof(ledger), &ram)) << "not committed";
    ledger.committed = 1;
    ledger.magic ^= 1;
    EXPECT_FALSE(UsbRam::fromLedger(&ledger, sizeof(ledger), &ram)) << "not a ledger";
}

// The device controller: the Switch's 4093 MiB in 4 GiB, 512 sections for the high bank from IOVA 0, 511 for the low
// one at its own address, one table for the low bank's last 1 MiB and the 3 MiB window after it.
TEST(UsbStaticMap, DeviceHasAllOfRamInSectionsAndOneTable)
{
    ASSERT_TRUE(kDevice.fits());
    EXPECT_EQ(kDevice.tables(), 1u);
    EXPECT_EQ(kDevice.windowBase, 0xffd0'0000u);
    EXPECT_EQ(kDevice.windowBytes, 3ULL << 20);
    const Space space(kDevice);
    const TegraSMMUDomain &d = space.domain;
    EXPECT_EQ(d.used, d.pages) << "every table the map was sized for, no more";
    EXPECT_EQ(space.sections, 1023u);
    EXPECT_EQ(space.tablesInstalled, 1u);
    for (const uint64_t pa :
         {0x8000'0000ULL, 0x8000'0fffULL, 0x8040'0000ULL, 0x9234'5678ULL, 0xffbf'ffffULL, 0xffc0'0000ULL,
          0xffcf'ffffULL, 0x1'0000'0000ULL, 0x1'0040'0000ULL, 0x1'2345'6789ULL, 0x1'7fff'ffffULL}) {
        const uint64_t iova = kDevice.iova(pa);
        ASSERT_LT(iova, kGiB4) << std::hex << pa;
        EXPECT_EQ(d.translate(iova), pa) << std::hex << pa;
        EXPECT_EQ(kDevice.pa(iova), pa) << std::hex << pa;
    }
    EXPECT_EQ(kDevice.iova(0x1'0000'0000), 0u);
    EXPECT_EQ(kDevice.iova(0x1'7fff'ffff), 0x7fff'ffffu);
    for (const uint64_t pa : {0x7fff'ffffULL, 0xffd0'0000ULL, 0xffff'ffffULL, 0x1'8000'0000ULL})
        EXPECT_EQ(kDevice.iova(pa), UINT64_MAX) << std::hex << pa << ": not RAM";
    for (const uint64_t iova : {0xffd0'0000ULL, 0xffd1'0000ULL, 0xffff'ffffULL}) {
        EXPECT_EQ(d.translate(iova), UINT64_MAX) << std::hex << iova << ": the window, empty";
        EXPECT_EQ(kDevice.pa(iova), UINT64_MAX) << std::hex << iova;
    }
    EXPECT_EQ(kDevice.iova(0xffcf'f000, 4096), 0xffcf'f000u);
    EXPECT_EQ(kDevice.iova(0xffcf'f000, 4097), UINT64_MAX) << "past the low bank";
    EXPECT_EQ(kDevice.iova(0x1'7fff'f000, 8192), UINT64_MAX) << "past the high bank";
}

// The host: the low bank at its own address in 510 sections, a table for its last 1 MiB and one for the 4 MiB that
// holds the firmware image, read only there and writable around it; nothing above 4 GiB at a fixed address; the
// window's 16 tables below RAM, clear of the 16 MiB an AHBSLV client cannot reach, empty.
TEST(UsbStaticMap, HostKeepsTheWindowBelowRamAndTheFirmwareReadOnly)
{
    ASSERT_TRUE(kHost.fits());
    EXPECT_GE(kHost.windowBase, 16ULL << 20);
    EXPECT_EQ(kHost.windowBytes, 64ULL << 20);
    EXPECT_EQ(kHost.tables(), 18u);
    Space space(kHost);
    const TegraSMMUDomain &d = space.domain;
    EXPECT_EQ(d.used, d.pages);
    EXPECT_EQ(space.sections, 510u);
    EXPECT_EQ(space.tablesInstalled, 18u);
    for (const uint64_t pa : {0x8000'0000ULL, 0x9f80'0000ULL, kFirmwarePa, kFirmwarePa + kFirmwareBytes - 1,
                              kFirmwarePa + kFirmwareBytes, 0x9fbf'ffffULL, 0xffcf'ffffULL}) {
        EXPECT_EQ(kHost.iova(pa), pa) << std::hex << pa;
        EXPECT_EQ(d.translate(pa), pa) << std::hex << pa;
    }
    EXPECT_TRUE(space.writable(0x9f80'0000));
    EXPECT_FALSE(space.writable(kFirmwarePa));
    EXPECT_FALSE(space.writable(kFirmwarePa + kFirmwareBytes - 1));
    EXPECT_TRUE(space.writable(kFirmwarePa + kFirmwareBytes));
    EXPECT_TRUE(space.writable(0xffcf'f000));
    for (const uint64_t pa : {0x1'0000'0000ULL, 0x1'2345'6000ULL})
        EXPECT_EQ(kHost.iova(pa, 4096), UINT64_MAX) << std::hex << pa << ": above 4 GiB goes through the window";
    for (const uint64_t iova : {0ULL, 1ULL << 20, 16ULL << 20, (80ULL << 20) - 1, 0x7fff'ffffULL}) {
        EXPECT_EQ(d.translate(iova), UINT64_MAX) << std::hex << iova;
        EXPECT_EQ(kHost.pa(iova), UINT64_MAX) << std::hex << iova;
    }
    EXPECT_EQ(UsbStaticMap::host(kRam, 0, 0).tables(), 17u) << "no firmware image: no table for it";
}

// RAM a 32-bit space cannot hold beside its window is refused (UsbSmmuSpace::init then says so in one line, and DMA
// stays physical).
TEST(UsbStaticMap, RamThatDoesNotFitIsRefused)
{
    EXPECT_FALSE(UsbStaticMap::device({0x8000'0000, 0xffd0'0000, 0x1'0000'0000, 0x2'8000'0000}).fits()) << "8 GiB";
    EXPECT_FALSE(UsbStaticMap::device({0x8000'0000, 0x1'0010'0000, 0x1'0010'0000, 0x1'8000'0000}).fits())
        << "across 4 GiB";
    EXPECT_FALSE(UsbStaticMap::device({0x8000'0000, 0xffd0'0000, 0x1'0040'0000, 0x1'8000'0000}).fits())
        << "a gap at 4 GiB";
    EXPECT_FALSE(UsbStaticMap::device({}).fits());
    EXPECT_TRUE(UsbStaticMap::device({0x8000'0000, 0xc000'0000, 0x1'0000'0000, 0x1'4000'0000}).fits()) << "2 GiB";
    EXPECT_TRUE(UsbStaticMap::host({0x8000'0000, 0xffd0'0000, 0x1'0000'0000, 0x2'8000'0000}, 0, 0).fits())
        << "the host maps no high bank: 8 GiB is fine";
    EXPECT_FALSE(UsbStaticMap::host({0x0200'0000, 0xffd0'0000, 0x1'0000'0000, 0x1'8000'0000}, 0, 0).fits())
        << "RAM over the window";
    EXPECT_FALSE(UsbStaticMap::host(kRam, 0x1'0000'0000, 4096).fits()) << "read-only pages outside the low bank";
}

namespace {

// switch_usb_smmu_map and UsbSmmuSpace::map on the host's space: the pieces checked against 34-bit RAM, the window's
// pages taken, each page's entry written by TegraSMMUDomain::map. Returns the IOVA of the first byte, or UINT64_MAX.
struct Window {
    Space space{kHost};
    std::vector<uint8_t> used = std::vector<uint8_t>(kHost.windowBytes / UsbIovaWindow::kPage);
    UsbIovaWindow window;

    Window()
    {
        window.init(used.data(), static_cast<uint32_t>(used.size()), kHost.windowBase);
    }

    uint64_t map(const std::vector<std::pair<uint64_t, uint64_t>> &pieces, bool writable, int64_t *index)
    {
        const auto segment = [&pieces](uint64_t done, uint64_t *bytes) {
            for (const auto &[pa, length] : pieces) {
                if (done < length) {
                    *bytes = length - done;
                    return pa + done;
                }
                done -= length;
            }
            *bytes = 0;
            return uint64_t{0};
        };
        uint64_t length = 0;
        for (const auto &piece : pieces)
            length += piece.second;
        uint64_t firstPa = 0;
        if (!usb_iova_page_aligned(segment, length, TegraSMMUDomain::kLimit * 4, &firstPa))
            return UINT64_MAX;
        const uint64_t inPage = firstPa & (UsbIovaWindow::kPage - 1);
        const uint32_t pages = UsbIovaWindow::pagesFor(inPage, length);
        *index = window.take(pages, 1);
        if (*index < 0)
            return UINT64_MAX;
        const uint64_t base = window.iova(static_cast<uint32_t>(*index));
        uint32_t mapped = 0;
        const bool ok = usb_iova_map_pages(
            segment, length, pages,
            [this, base, writable](uint32_t i, uint64_t pa) {
                return space.domain.map(base + static_cast<uint64_t>(i) * UsbIovaWindow::kPage, &pa, 1, writable);
            },
            &mapped);
        return ok ? base + inPage : UINT64_MAX;
    }

    uint32_t entry(uint64_t iova)
    {
        const uint32_t *pte = space.domain.pte(iova, false);
        return pte ? *pte : 0;
    }
};

// Tegra X1 TRM 18.6.3.1 (p. 812): a PTE is PA[33:12] in bits 21:0, READABLE 31, WRITABLE 30, NONSECURE 29, bits 27:22
// reserved (zero, or the SMMU faults).
constexpr uint32_t kReadWrite = 0xe000'0000, kReadOnly = 0xa000'0000;

}  // namespace

// A request in the high bank is no fixed address of the host's (xusb_host is an AHBSLV client: 32-bit addresses), so it
// goes through the window: three scattered pages above 4 GiB, the first one entered 0x10 bytes in.
TEST(UsbStaticMap, HostWindowMapsPagesAboveFourGiB)
{
    EXPECT_EQ(kHost.iova(0x1'2345'6010, 4096 - 0x10), UINT64_MAX);
    Window w;
    int64_t index = -1;
    const uint64_t iova =
        w.map({{0x1'2345'6010, 4096 - 0x10}, {0x1'7000'0000, 4096}, {0x1'0000'1000, 100}}, true, &index);
    ASSERT_NE(iova, UINT64_MAX);
    EXPECT_EQ(iova & 0xfff, 0x10u);
    EXPECT_GE(iova, 16ULL << 20);
    EXPECT_LT(iova + 2ULL * 4096 + 100, 80ULL << 20);
    const uint64_t page = iova & ~0xfffULL;
    EXPECT_EQ(w.entry(page), kReadWrite | 0x12'3456u);
    EXPECT_EQ(w.entry(page + 4096), kReadWrite | 0x17'0000u);
    EXPECT_EQ(w.entry(page + 2ULL * 4096), kReadWrite | 0x10'0001u);
    for (const uint64_t at : {page, page + 4096, page + 2ULL * 4096})
        EXPECT_EQ(w.entry(at) & 0x0fc0'0000u, 0u) << "reserved bits at " << std::hex << at;
    EXPECT_EQ(w.space.domain.translate(iova), 0x1'2345'6010u);
    EXPECT_EQ(w.space.domain.translate(iova + 4096 - 0x10), 0x1'7000'0000u);
    EXPECT_EQ(w.space.domain.translate(iova + 2ULL * 4096 - 0x10 + 99), 0x1'0000'1063u);

    int64_t neighbour = -1;
    const uint64_t other = w.map({{0x1'4000'0000, 4096}}, false, &neighbour);
    ASSERT_NE(other, UINT64_MAX);
    EXPECT_EQ(w.entry(other), kReadOnly | 0x14'0000u);
    w.space.domain.unmap(w.window.iova(static_cast<uint32_t>(index)), 3);
    w.window.give(static_cast<uint32_t>(index), 3);
    for (const uint64_t at : {page, page + 4096, page + 2ULL * 4096})
        EXPECT_EQ(w.entry(at), 0u);
    EXPECT_EQ(w.entry(other), kReadOnly | 0x14'0000u) << "unmap reaches only its own pages";
}

// A buffer across the 4 GiB line: its last page in the low bank, its next in the high one, entries for both.
TEST(UsbStaticMap, HostWindowMapsAcrossFourGiB)
{
    Window w;
    int64_t index = -1;
    const uint64_t iova = w.map({{0xffff'f000, 4096}, {0x1'0000'0000, 4096}}, true, &index);
    ASSERT_NE(iova, UINT64_MAX);
    EXPECT_EQ(w.entry(iova), kReadWrite | 0xf'ffffu);
    EXPECT_EQ(w.entry(iova + 4096), kReadWrite | 0x10'0000u);
    EXPECT_EQ(w.space.domain.translate(iova + 4096 + 0x123), 0x1'0000'0123u);
    EXPECT_EQ(w.map({{0x4'0000'0000, 4096}}, true, &index), UINT64_MAX) << "past PA[33:12]";
}
