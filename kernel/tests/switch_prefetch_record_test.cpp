// SPDX-License-Identifier: GPL-2.0-only
// The boot list is one boot's record log: what it read, through cluster_io or decmpfs, and what it first used of the
// pages the replay brought in. Runs the save's pure half (prefetch_naming.h, prefetch_list.h) over two boots, and
// checks the hooks that feed the log in both XNU trees.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "switch/prefetch_list.h"
#include "switch/prefetch_naming.h"
#include "switch/prefetch_pace.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

constexpr uint64_t kVmPage = 16384;
constexpr uint64_t kFileA = 0x101;
constexpr uint64_t kFileB = 0x202;

struct File {
    const void *vp;
    uint32_t vid;
    uint8_t volume;
    uint64_t fileId;
};

switch_record_entry entry(const File &f, uint64_t offset, uint32_t length, uint16_t when)
{
    switch_record_entry e = {};
    e.vp = (struct vnode *)f.vp;
    e.offset = offset;
    e.length = length;
    e.vid = f.vid;
    e.index = f.volume;
    e.when = when;
    return e;
}

switch_named_file named(const File &f)
{
    return {f.vp, f.vid, f.volume, f.fileId, 1u << 20, 1730000000, 0};
}

// prefetch_save.c's order: sort, group, name (replay's files for read-only volumes, XNU's vnodes, then the replay's
// files for the rest), ranges, merge, sort by first need, cap. `inCore` stands in for vnode_iterate.
std::vector<switch_prefetch_run> saveList(std::vector<switch_record_entry> log, std::vector<switch_named_file> files,
                                          const std::vector<File> &inCore, uint64_t *unnamed = nullptr)
{
    std::sort(log.begin(), log.end(), [](const auto &a, const auto &b) {
        return switch_prefetch_compare_log_entries(&a, &b) < 0;
    });
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
        return switch_prefetch_compare_named_files(&a, &b) < 0;
    });
    std::vector<switch_log_group> groups(log.size());
    const uint32_t count = switch_prefetch_group_log(log.data(), (uint32_t)log.size(), 4, groups.data());
    switch_prefetch_name_from_files(groups.data(), count, files.data(), (uint32_t)files.size(), false);
    for (uint32_t g = 0; g < count; ++g) {
        for (const File &f : inCore) {
            if (groups[g].file_id == 0 && groups[g].vp == f.vp && groups[g].vid == f.vid) {
                groups[g].file_id = f.fileId;
                groups[g].file_size = 1u << 20;
                groups[g].mtime_sec = 1730000000;
            }
        }
    }
    switch_prefetch_name_from_files(groups.data(), count, files.data(), (uint32_t)files.size(), true);
    std::vector<switch_prefetch_range> ranges(log.size());
    uint64_t pages = 0;
    uint64_t unnamedBytes = 0;
    const uint32_t n =
        switch_prefetch_log_ranges(log.data(), groups.data(), count, ranges.data(), &pages, &unnamedBytes);
    if (unnamed != nullptr) {
        *unnamed = unnamedBytes;
    }
    std::sort(ranges.begin(), ranges.begin() + n, [](const auto &a, const auto &b) {
        return switch_prefetch_compare_ranges(&a, &b) < 0;
    });
    const uint64_t cap =
        switch_prefetch_run_capacity(n, pages, SWITCH_PREFETCH_MAX_RUN_PAGES, SWITCH_PREFETCH_MAX_GAP_PAGES);
    std::vector<switch_prefetch_run> runs(cap);
    uint32_t listFiles = 0;
    const uint32_t built = switch_prefetch_merge_runs(ranges.data(), n, runs.data(), cap, SWITCH_PREFETCH_MAX_RUN_PAGES,
                                                      SWITCH_PREFETCH_MAX_GAP_PAGES, &listFiles);
    std::sort(runs.begin(), runs.begin() + built, [](const auto &a, const auto &b) {
        return switch_prefetch_compare_runs(&a, &b) < 0;
    });
    runs.resize(switch_prefetch_cap_runs(runs.data(), built, 1u << 20, 1u << 18));
    return runs;
}

uint64_t pagesOf(const std::vector<switch_prefetch_run> &list, uint64_t fileId)
{
    uint64_t pages = 0;
    for (const auto &r : list) {
        if (r.file_id == fileId) {
            pages += r.pages;
        }
    }
    return pages;
}

}  // namespace

TEST(SwitchPrefetchRecord, AReplayedRunStaysOnlyWhenTheBootUsedIt)
{
    const File a1 = {(void *)0xa100, 1, SWITCH_PREFETCH_VOL_ROOT, kFileA};
    const File b1 = {(void *)0xb100, 1, SWITCH_PREFETCH_VOL_DATA, kFileB};
    const auto first = saveList({entry(a1, 0, 4 * kVmPage, 3), entry(b1, 0, 4 * kVmPage, 5)}, {}, {a1, b1});
    EXPECT_EQ(pagesOf(first, kFileA), 16u);
    EXPECT_EQ(pagesOf(first, kFileB), 16u);

    // Boot 2 replays both files, then uses two of A's pages and none of B's: only first uses reach the log.
    const File a2 = {(void *)0xa200, 7, SWITCH_PREFETCH_VOL_ROOT, kFileA};
    const File b2 = {(void *)0xb200, 7, SWITCH_PREFETCH_VOL_DATA, kFileB};
    const auto second =
        saveList({entry(a2, 0, kVmPage, 2), entry(a2, kVmPage, kVmPage, 2)}, {named(a2), named(b2)}, {a2, b2});
    EXPECT_EQ(pagesOf(second, kFileA), 8u);
    EXPECT_EQ(pagesOf(second, kFileB), 0u);
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second.front().when, 2u);
}

TEST(SwitchPrefetchRecord, AMissTheReplayDidNotCoverJoinsTheNextList)
{
    const File a = {(void *)0xa200, 7, SWITCH_PREFETCH_VOL_ROOT, kFileA};
    const File c = {(void *)0xc200, 3, SWITCH_PREFETCH_VOL_SYSTEM, 0x303};
    const auto list = saveList({entry(a, 0, kVmPage, 4), entry(c, 8 * kVmPage, 2 * kVmPage, 9)}, {named(a)}, {c});
    EXPECT_EQ(pagesOf(list, kFileA), 4u);
    EXPECT_EQ(pagesOf(list, 0x303), 8u);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].file_id, kFileA);
    EXPECT_EQ(list[1].page, 32u);
}

TEST(SwitchPrefetchRecord, ARecycledVnodeKeepsItsNameFromTheReplay)
{
    const File root = {(void *)0xa200, 7, SWITCH_PREFETCH_VOL_ROOT, kFileA};
    const File data = {(void *)0xb200, 7, SWITCH_PREFETCH_VOL_DATA, kFileB};
    uint64_t unnamed = 0;
    const auto list =
        saveList({entry(root, 0, kVmPage, 1), entry(data, 0, kVmPage, 1)}, {named(root), named(data)}, {}, &unnamed);
    EXPECT_EQ(pagesOf(list, kFileA), 4u);
    EXPECT_EQ(pagesOf(list, kFileB), 4u);
    EXPECT_EQ(unnamed, 0u);
}

TEST(SwitchPrefetchRecord, AReusedVnodeIsNotTakenForTheReplaysFile)
{
    const File replayed = {(void *)0xa200, 7, SWITCH_PREFETCH_VOL_ROOT, kFileA};
    const File reused = {(void *)0xa200, 8, SWITCH_PREFETCH_VOL_ROOT, 0x999};
    uint64_t unnamed = 0;
    const auto list = saveList({entry(reused, 0, kVmPage, 1)}, {named(replayed)}, {}, &unnamed);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(unnamed, kVmPage);
}

TEST(SwitchPrefetchRecord, DataFilesAreNamedByTheirVnodeFirst)
{
    const File data = {(void *)0xb200, 7, SWITCH_PREFETCH_VOL_DATA, kFileB};
    std::vector<switch_log_group> groups(1);
    const switch_record_entry log[] = {entry(data, 0, kVmPage, 1)};
    ASSERT_EQ(switch_prefetch_group_log(log, 1, 4, groups.data()), 1u);
    const switch_named_file files[] = {named(data)};
    EXPECT_EQ(switch_prefetch_name_from_files(groups.data(), 1, files, 1, false), 0u);
    EXPECT_EQ(groups[0].file_id, 0u);
    EXPECT_EQ(switch_prefetch_name_from_files(groups.data(), 1, files, 1, true), 1u);
    EXPECT_EQ(groups[0].file_id, kFileB);
}

namespace {

std::string between(const std::string &text, const std::string &from, const std::string &to)
{
    const size_t a = text.find(from);
    if (a == std::string::npos) {
        return {};
    }
    const size_t b = text.find(to, a);
    return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

}  // namespace

TEST(SwitchPrefetchRecordSource, DecmpfsRecordsBothReadPathsByTheFilesOffsets)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        SCOPED_TRACE(tree);
        const std::string decmpfs = SourceText(std::string("../") + tree + "/bsd/kern/decmpfs.c");
        const std::string pagein = between(decmpfs, "\ndecmpfs_pagein_compressed(", "\ndecmpfs_read_compressed(");
        const size_t pageinRecord =
            pagein.find("switch_cluster_record_file(vp, f_offset, (uint64_t)MIN((off_t)size, cachedSize - f_offset), "
                        "SWITCH_RECORD_DECMPFS);");
        ASSERT_NE(pageinRecord, std::string::npos);
        EXPECT_LT(pagein.find("VNOP_VERIFY("), pageinRecord);
        EXPECT_LT(pageinRecord, pagein.find("upl_mark_decmp(pl);"));

        const std::string read = between(decmpfs, "\ndecmpfs_read_compressed(", "\ndecmpfs_free_compressed_data(");
        const size_t readRecord =
            read.find("switch_cluster_record_file(vp, uplPos, (uint64_t)uplSize, SWITCH_RECORD_DECMPFS);");
        ASSERT_NE(readRecord, std::string::npos);
        EXPECT_LT(read.find("uplSize = (uplSize + (alignment_size - 1)) & ~(alignment_size - 1);"), readRecord);
        EXPECT_LT(readRecord, read.find("uplRemaining = uplSize;"));

        const std::string cluster = SourceText(std::string("../") + tree + "/bsd/vfs/vfs_cluster.c");
        const std::string file = between(cluster, "\nswitch_cluster_record_file(", "\n}\n");
        EXPECT_THAT(file, HasSubstr("!switch_quiesce_is_armed(&switch_record_gate)"));
        EXPECT_THAT(file, HasSubstr("os_atomic_load(&switch_record_skip_thread, relaxed) == current_thread()"));
        EXPECT_THAT(file, HasSubstr("switch_cluster_volume_index(vnode_mount(vp))"));
    }
}

TEST(SwitchPrefetchRecordSource, AReplayedPagesFirstUseIsRecordedOutsideThePhysicalPageLock)
{
    const struct {
        const char *tree;
        const char *header;
    } trees[] = {{"xnu-10063.141.1", "vm/vm_page.h"}, {"xnu-12377.121.6", "vm/vm_page_internal.h"}};

    for (const auto &t : trees) {
        SCOPED_TRACE(t.tree);
        const std::string header = SourceText(std::string("../") + t.tree + "/osfmk/" + t.header);
        const std::string macro =
            between(header, "#define VM_PAGE_CONSUME_CLUSTERED_WAKE", "#define VM_PAGE_CONSUME_CLUSTERED(");
        const size_t marked = macro.find("__switch_first_use = true;");
        const size_t unlock = macro.find("pmap_unlock_phys_page(__phys_page);");
        const size_t record = macro.find("switch_vm_prefetch_first_use(VM_PAGE_OBJECT(mem),");
        ASSERT_NE(marked, std::string::npos);
        ASSERT_NE(record, std::string::npos);
        EXPECT_LT(macro.find("if (__improbable(mem->vmp_switch_prefetched))"), marked);
        EXPECT_LT(unlock, record);

        const std::string pageout = SourceText(std::string("../") + t.tree + "/osfmk/vm/vm_pageout.c");
        const std::string mark = between(pageout, "\nupl_mark_decmp(upl_t upl)", "\n}\n");
        EXPECT_THAT(mark, HasSubstr("#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO"));
        EXPECT_THAT(mark, HasSubstr("(upl->flags & UPL_SWITCH_PREFETCH) != 0"));
    }
    const std::string vm = SourceText("../switch/vm/prefetch_vm.c");
    const std::string use = between(vm, "void switch_vm_prefetch_first_use(", "\n}\n");
    EXPECT_THAT(use, HasSubstr("object->internal || object->pager == MEMORY_OBJECT_NULL"));
    EXPECT_THAT(use, HasSubstr("vnode_pager_lookup_vnode(object->pager)"));
    EXPECT_THAT(use, HasSubstr("offset + object->paging_offset"));
    EXPECT_THAT(use, HasSubstr("SWITCH_RECORD_FIRST_USE"));
}

TEST(SwitchPrefetchRecordSource, TheSaveCarriesNothingFromTheLastList)
{
    const std::string save = SourceText("../switch/vm/prefetch_save.c");
    const std::string next = between(save, "void switch_prefetch_save_next(", "\n}\n");
    EXPECT_THAT(next, Not(HasSubstr("old_runs")));
    EXPECT_THAT(save, Not(HasSubstr("VFS_VGET")));
    EXPECT_THAT(save, Not(HasSubstr("switch_vm_prefetch_check_run")));
    EXPECT_THAT(next, HasSubstr("switch_prefetch_log_ranges(log, groups, group_count, ranges"));

    const std::string prefetch = SourceText("../switch/vm/prefetch.c");
    const std::string replay = between(prefetch, "static uint32_t switch_prefetch_replay_runs(", "\n}\n");
    const size_t vget = replay.find("switch_prefetch_vget_regular(");
    const size_t kept = replay.find("files[file_count++] = file;");
    ASSERT_NE(kept, std::string::npos);
    EXPECT_LT(vget, kept);
}
