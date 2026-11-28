// <switch/vm_lazy_cow.h>: deferred COW-write grants on nested shared-region submaps (xnu-12377 / iOS 27).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/source_text.h"

typedef int vm_prot_t;

#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04
#define VM_PROT_UEXEC   0x08
#define VM_PROT_COPY    0x10
#define VM_PROT_ALLEXEC (VM_PROT_EXECUTE | VM_PROT_UEXEC)

#include <switch/vm_lazy_cow.h>

using switch_ios::test::Between;
using switch_ios::test::SourceText;

namespace {

constexpr vm_prot_t kRW = VM_PROT_READ | VM_PROT_WRITE;
constexpr vm_prot_t kRWCopy = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY;
constexpr vm_prot_t kRX = VM_PROT_READ | VM_PROT_EXECUTE;
constexpr vm_prot_t kRWX = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
constexpr uint64_t kPage = 0x4000;

TEST(SwitchVmLazyCow, RequestQualificationStrict)
{
    EXPECT_TRUE(switch_vm_lazy_cow_request_qualifies(false, kRWCopy, false, false));

    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(true, kRWCopy, false, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, kRW, false, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, VM_PROT_READ, false, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, kRWX | VM_PROT_COPY, false, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_UEXEC | VM_PROT_COPY,
                                                      false, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, kRWCopy, true, false));
    EXPECT_FALSE(switch_vm_lazy_cow_request_qualifies(false, kRWCopy, false, true));
}

TEST(SwitchVmLazyCow, EntryQualificationStrict)
{
    EXPECT_TRUE(switch_vm_lazy_cow_top_submap_qualifies(true, true, false, VM_PROT_READ, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(false, true, false, VM_PROT_READ, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(true, false, false, VM_PROT_READ, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(true, true, true, VM_PROT_READ, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(true, true, false, kRX, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(true, true, false, VM_PROT_READ, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_top_submap_qualifies(true, true, false, VM_PROT_READ, true, false));

    EXPECT_TRUE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, false, false,
                                                          false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(true, VM_PROT_READ, kRW, false, false, false, false, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, VM_PROT_READ, false, false, false, false,
                                                           false, false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, kRW, kRW, false, false, false, false, false, false,
                                                           false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRWX, false, false, false, false, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, true, false, false, false, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, true, false, false, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, true, false, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, true, false,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, false, true,
                                                           false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, false, false,
                                                           true, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, false, false,
                                                           false, true, true));
    EXPECT_FALSE(switch_vm_lazy_cow_submap_entry_qualifies(false, VM_PROT_READ, kRW, false, false, false, false, false,
                                                           false, false, false));

    EXPECT_TRUE(switch_vm_lazy_cow_private_entry_qualifies(false, false, false, VM_PROT_READ, kRW, false, false, false,
                                                           false, false, false, false, true));
    EXPECT_TRUE(switch_vm_lazy_cow_private_entry_qualifies(false, false, false, kRW, kRW, false, false, false, false,
                                                           false, false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_private_entry_qualifies(false, false, true, VM_PROT_READ, kRW, false, false, false,
                                                            false, false, false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_private_entry_qualifies(false, true, false, VM_PROT_READ, kRW, false, false, false,
                                                            false, false, false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_private_entry_qualifies(false, false, false, VM_PROT_READ, kRWX, false, false, false,
                                                            false, false, false, false, true));
    EXPECT_FALSE(switch_vm_lazy_cow_private_entry_qualifies(false, false, false, VM_PROT_READ, kRW, true, false, false,
                                                            false, false, false, false, true));
}

TEST(SwitchVmLazyCow, DyldNineDataConstRegionsCoalesceAndSinglePageFaultSplits)
{
    struct switch_vm_lazy_rw_table t = {};

    // Real 24B119 .25 / .54 / .77 __DATA_CONST + __AUTH_CONST layout (unslid).
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x1e00e4000ULL, 0x1e1f58000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x1e1f58000ULL, 0x1e8eb4000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x1e8eb4000ULL, 0x1e8f70000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x25f890000ULL, 0x2611ac000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x2611ac000ULL, 0x266cd8000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x266cd8000ULL, 0x266e2c000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x2d0044000ULL, 0x2d2528000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x2d2528000ULL, 0x2d6224000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x2d6224000ULL, 0x2d62c8000ULL));

    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t.ranges[0].start, 0x1e00e4000ULL);
    EXPECT_EQ(t.ranges[0].end, 0x1e8f70000ULL);
    EXPECT_EQ(t.ranges[1].start, 0x25f890000ULL);
    EXPECT_EQ(t.ranges[1].end, 0x266e2c000ULL);
    EXPECT_EQ(t.ranges[2].start, 0x2d0044000ULL);
    EXPECT_EQ(t.ranges[2].end, 0x2d62c8000ULL);

    // Adjacent page outside .25 __DATA_CONST is never granted write permission.
    EXPECT_FALSE(switch_vm_lazy_rw_table_contains_page(&t, 0x1e00e0000ULL, 0x1e00e4000ULL));
    EXPECT_FALSE(switch_vm_lazy_rw_table_contains_page(&t, 0x1e8f70000ULL, 0x1e8f74000ULL));

    // Single write fault in libMobileGestaltExtensions __objc_protorefs materializes only 1 page (16 KiB).
    const uint64_t faultPage = 0x1e1e98000ULL;
    uint64_t sliceStart = 0, sliceEnd = 0;
    ASSERT_TRUE(switch_vm_lazy_rw_table_fault_slice(&t, faultPage, faultPage + kPage, &sliceStart, &sliceEnd));
    EXPECT_EQ(sliceStart, faultPage);
    EXPECT_EQ(sliceEnd, faultPage + kPage);
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, sliceStart, sliceEnd));

    ASSERT_EQ(t.count, 4u);
    EXPECT_EQ(t.ranges[0].start, 0x1e00e4000ULL);
    EXPECT_EQ(t.ranges[0].end, faultPage);
    EXPECT_EQ(t.ranges[1].start, faultPage + kPage);
    EXPECT_EQ(t.ranges[1].end, 0x1e8f70000ULL);
    EXPECT_FALSE(switch_vm_lazy_rw_table_contains_page(&t, faultPage, faultPage + kPage));

    // makeDataConstWritable(false) clears every deferred grant back to empty.
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, 0x1e00e4000ULL, 0x1e1f58000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, 0x1e1f58000ULL, 0x1e8eb4000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, 0x1e8eb4000ULL, 0x1e8f70000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, 0x25f890000ULL, 0x266e2c000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, 0x2d0044000ULL, 0x2d62c8000ULL));
    EXPECT_EQ(t.count, 0u);
}

TEST(SwitchVmLazyCow, SoftCapPreventsOverflowAndLeavesRoomForSplits)
{
    struct switch_vm_lazy_rw_table t = {};
    for (uint32_t i = 0; i < SWITCH_VM_LAZY_RW_SOFT_MAX; i++) {
        const uint64_t base = 0x100000000ULL + static_cast<uint64_t>(i) * 0x100000ULL;
        ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, base, base + 0x40000ULL));
    }
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_SOFT_MAX);

    // A new disjoint range beyond SOFT_MAX is rejected without mutating the table.
    EXPECT_FALSE(switch_vm_lazy_rw_table_add(&t, 0x200000000ULL, 0x200040000ULL));
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_SOFT_MAX);

    // An overlapping or adjacent range does not increase count and still succeeds.
    EXPECT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x100040000ULL, 0x100080000ULL));
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_SOFT_MAX);

    // At SOFT_MAX, an interior fault materializes the shorter half rather than growing count.
    const uint64_t page = 0x100010000ULL;
    uint64_t s = 0, e = 0;
    ASSERT_TRUE(switch_vm_lazy_rw_table_fault_slice(&t, page, page + kPage, &s, &e));
    EXPECT_EQ(s, 0x100000000ULL);
    EXPECT_EQ(e, page + kPage);
    ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, s, e));
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_SOFT_MAX);

    // Even if filled up to MAX via interior subtractions, would_overflow flags the next interior split.
    uint32_t splitIdx = 1;
    while (t.count < SWITCH_VM_LAZY_RW_MAX) {
        ASSERT_LT(splitIdx, t.count);
        const uint64_t mid = t.ranges[splitIdx].start + kPage;
        ASSERT_LT(mid + kPage, t.ranges[splitIdx].end);
        ASSERT_TRUE(switch_vm_lazy_rw_table_subtract(&t, mid, mid + kPage));
        splitIdx += 2;
    }
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_MAX);
    const uint64_t lastStart = t.ranges[t.count - 1].start;
    EXPECT_TRUE(switch_vm_lazy_rw_table_subtract_would_overflow(&t, lastStart + kPage, lastStart + 2 * kPage));
    EXPECT_FALSE(switch_vm_lazy_rw_table_subtract(&t, lastStart + kPage, lastStart + 2 * kPage));
    EXPECT_EQ(t.count, SWITCH_VM_LAZY_RW_MAX);
}

TEST(SwitchVmLazyCow, RegionSliceWalkerCoversEntryWithoutLooping)
{
    struct switch_vm_lazy_rw_table t = {};
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x1e2200000ULL, 0x1e2300000ULL));
    ASSERT_TRUE(switch_vm_lazy_rw_table_add(&t, 0x1e2500000ULL, 0x1e4000000ULL));

    const uint64_t entryStart = 0x1e216c000ULL;
    const uint64_t entryEnd = 0x1e3fe0000ULL;
    std::vector<std::tuple<uint64_t, uint64_t, bool>> slices;

    uint64_t cursor = entryStart;
    while (cursor < entryEnd) {
        uint64_t s = 0, e = 0;
        const bool writable = switch_vm_lazy_rw_table_region_slice(&t, entryStart, entryEnd, cursor, &s, &e);
        ASSERT_EQ(s, cursor);
        ASSERT_GT(e, s);
        ASSERT_LE(e, entryEnd);
        slices.emplace_back(s, e, writable);
        cursor = e;
    }

    ASSERT_EQ(slices.size(), 4u);
    EXPECT_EQ(slices[0], std::make_tuple(0x1e216c000ULL, 0x1e2200000ULL, false));
    EXPECT_EQ(slices[1], std::make_tuple(0x1e2200000ULL, 0x1e2300000ULL, true));
    EXPECT_EQ(slices[2], std::make_tuple(0x1e2300000ULL, 0x1e2500000ULL, false));
    EXPECT_EQ(slices[3], std::make_tuple(0x1e2500000ULL, 0x1e3fe0000ULL, true));

    // Mid-slice lookup reports the enclosing slice boundaries.
    uint64_t s = 0, e = 0;
    EXPECT_TRUE(switch_vm_lazy_rw_table_region_slice(&t, entryStart, entryEnd, 0x1e2208000ULL, &s, &e));
    EXPECT_EQ(s, 0x1e2200000ULL);
    EXPECT_EQ(e, 0x1e2300000ULL);
}

TEST(SwitchVmLazyCowSource, Xnu12377WiredAndXnu10063Untouched)
{
    const std::string map12377 = SourceText("../xnu-12377.121.6/osfmk/vm/vm_map.c");
    const std::string fault12377 = SourceText("../xnu-12377.121.6/osfmk/vm/vm_fault.c");

    EXPECT_NE(map12377.find("#include <switch/vm_lazy_cow.h>"), std::string::npos);
    EXPECT_NE(fault12377.find("#include <switch/vm_lazy_cow.h>"), std::string::npos);

    const std::string protectAll = Between(map12377, "\nvm_map_protect(\n", "\nstatic __attribute__((always_inline");
    const size_t corePos = protectAll.find("\nvm_map_protect_core(\n");
    ASSERT_NE(corePos, std::string::npos);
    const std::string protect = protectAll.substr(0, corePos);
    const std::string protectCore = protectAll.substr(corePos);

    EXPECT_NE(protect.find("switch_vm_lazy_cow_request_qualifies("), std::string::npos);
    EXPECT_NE(protect.find("switch_vm_lazy_rw_try_defer("), std::string::npos);
    EXPECT_NE(protect.find("switch_vm_lazy_rw_materialize_overlap_locked("), std::string::npos);

    const size_t lockCall = protectCore.find("vm_map_range_ex_lock(");
    const size_t clearCall = protectCore.find("switch_vm_lazy_rw_clear_locked(", lockCall);
    ASSERT_NE(lockCall, std::string::npos);
    ASSERT_NE(clearCall, std::string::npos);
    EXPECT_LT(lockCall, clearCall);

    const std::string lookup = Between(fault12377, "\nvm_map_lookup_object_and_lock_entry(\n", "\nkern_return_t\nvm_fault_internal(");
    EXPECT_NE(lookup.find("switch_vm_lazy_rw_fault("), std::string::npos);

    const std::string faultInternal = Between(fault12377, "\nvm_fault_internal(\n", "\nkern_return_t\n");
    EXPECT_NE(faultInternal.find("switch_vm_lazy_rw_page_active("), std::string::npos);

    // Strictly version-contained: xnu-10063 (iOS 17) has no lazy COW hooks.
    const std::string map10063 = SourceText("../xnu-10063.141.1/osfmk/vm/vm_map.c");
    const std::string fault10063 = SourceText("../xnu-10063.141.1/osfmk/vm/vm_fault.c");
    const std::string mapXnu10063 = SourceText("../xnu-10063.141.1/osfmk/vm/vm_map_internal.h");
    EXPECT_EQ(map10063.find("vm_lazy_cow"), std::string::npos);
    EXPECT_EQ(map10063.find("switch_lazy_rw"), std::string::npos);
    EXPECT_EQ(fault10063.find("switch_lazy_rw"), std::string::npos);
    EXPECT_EQ(mapXnu10063.find("switch_lazy_rw"), std::string::npos);
}

}  // namespace
