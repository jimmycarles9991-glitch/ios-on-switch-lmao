// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/memory.h"

#include <gtest/gtest.h>

#include <mach/vm_statistics.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace {

constexpr uint32_t kTag = 243;
constexpr uint64_t kMiB = 1024 * 1024;

const switch_vm_tag_row *Row(const switch_vm_tags &tags, uint32_t tag)
{
    for (size_t i = 0; i < tags.kept; ++i) {
        if (tags.rows[i].tag == tag)
            return &tags.rows[i];
    }
    return nullptr;
}

TEST(SwitchMemoryWalkTest, DirtyTaggedMemoryIsChargedUnderItsTag)
{
    uint64_t page = 0;
    ASSERT_TRUE(switch_memory_footprint_mode(&page));
    const size_t bytes = 256 * page;
    void *mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, VM_MAKE_TAG(kTag), 0);
    ASSERT_NE(mapping, MAP_FAILED);
    memset(mapping, 0xa5, bytes);

    switch_vm_tags tags;
    switch_vm_snapshot snap = {};
    ASSERT_TRUE(switch_memory_walk(getpid(), page, &tags, &snap));
    munmap(mapping, bytes);

    const switch_vm_tag_row *row = Row(tags, kTag);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->dirty_bytes + row->swapped_bytes, bytes);
    EXPECT_EQ(row->virtual_bytes, bytes);
    EXPECT_EQ(row->regions, 1u);
}

TEST(SwitchMemoryWalkTest, TheWalkAddsUpToTheFootprintLessPageTables)
{
    uint64_t page = 0;
    ASSERT_TRUE(switch_memory_footprint_mode(&page));
    switch_vm_tags tags;
    switch_vm_snapshot snap = {};
    ASSERT_TRUE(switch_memory_walk(getpid(), page, &tags, &snap));

    const uint64_t walked = tags.total.dirty_bytes + tags.total.swapped_bytes;
    ASSERT_GT(snap.footprint_bytes, 0u);
    EXPECT_LE(walked, snap.footprint_bytes + 2 * kMiB);
    EXPECT_LE(snap.footprint_bytes, walked + 16 * kMiB);
}

}  // namespace
