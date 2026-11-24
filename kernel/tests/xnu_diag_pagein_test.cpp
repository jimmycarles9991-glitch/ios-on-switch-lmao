// kernel/switch/diag/pagein.h on the host: a window's file pager reads and busy-page waits by process and by file, a
// file named by a read, waits apart from reads, full tables counted, the line; and both trees' file pager and page
// sleep reporting to it, with 10063's fault fast path, which waits for a busy page without vm_page_sleep.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "diag/pagein.h"

using testing::HasSubstr;

namespace {

constexpr uint64_t kMs = 1000000;  // ticks are nanoseconds
constexpr uint64_t kCache = 0xfffffe0011112222ull, kApp = 0xfffffe0033334444ull;

size_t Count(const char *text, const char *needle)
{
    return switch_ios::test::Count(std::string(text), needle);
}

std::unique_ptr<switch_pagein_window> Window()
{
    return std::make_unique<switch_pagein_window>();
}

TEST(XnuDiagPagein, AFileIsNamedByItsFirstRead)
{
    auto w = Window();
    EXPECT_FALSE(switch_pagein_note_read(w.get(), 400, "SpringBoard", kCache, nullptr, 65536, 5 * kMs))
        << "a file not seen yet needs its name";
    EXPECT_EQ(w->reads, 0u);
    EXPECT_TRUE(
        switch_pagein_note_read(w.get(), 400, "SpringBoard", kCache, "dyld_shared_cache_arm64e", 65536, 5 * kMs));
    EXPECT_TRUE(switch_pagein_note_read(w.get(), 659, "Preferences", kCache, nullptr, 16384, 2 * kMs));
    EXPECT_TRUE(switch_pagein_note_read(w.get(), 659, "Preferences", kApp, "Preferences", 16384, 1 * kMs));
    EXPECT_EQ(w->reads, 3u);
    EXPECT_EQ(w->bytes, 98304u);
    const switch_pagein_row *cache = switch_pagein_find(w->file, kCache, false);
    ASSERT_NE(cache, nullptr);
    EXPECT_STREQ(cache->name, "dyld_shared_cache_arm64e");
    EXPECT_EQ(cache->reads, 2u);
    EXPECT_EQ(cache->read, 7 * kMs);
    const switch_pagein_row *preferences = switch_pagein_find(w->process, 660, false);
    ASSERT_NE(preferences, nullptr);
    EXPECT_STREQ(preferences->name, "Preferences");
    EXPECT_EQ(preferences->read, 3 * kMs);
}

TEST(XnuDiagPagein, AWaitForAFileReadIsTheWaitersOnItsFile)
{
    auto w = Window();
    switch_pagein_note_read(w.get(), 400, "SpringBoard", kCache, "dyld_shared_cache_arm64e", 65536, kMs / 10);
    switch_pagein_note_wait(w.get(), 400, "SpringBoard", kCache, 50 * kMs);
    switch_pagein_note_wait(w.get(), 659, "Preferences", kCache, 30 * kMs);
    const switch_pagein_row *cache = switch_pagein_find(w->file, kCache, false);
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(cache->reads, 1u);
    EXPECT_EQ(cache->waits, 2u);
    EXPECT_EQ(cache->wait, 80 * kMs);
    const switch_pagein_row *preferences = switch_pagein_find(w->process, 660, false);
    ASSERT_NE(preferences, nullptr) << "a process that only waited for another's read has its row";
    EXPECT_EQ(preferences->reads, 0u);
    EXPECT_EQ(preferences->wait, 30 * kMs);
    EXPECT_EQ(w->waits, 2u);
    EXPECT_EQ(w->wait, 80 * kMs);
    EXPECT_EQ(w->longest, 50 * kMs);
    EXPECT_EQ(w->longest_pid, 400);
    EXPECT_STREQ(w->longest_process, "SpringBoard");
    EXPECT_STREQ(w->longest_file, "dyld_shared_cache_arm64e");
}

TEST(XnuDiagPagein, AFileFirstMetByAWaitIsNamedByItsNextRead)
{
    auto w = Window();
    switch_pagein_note_wait(w.get(), 400, "SpringBoard", kApp, 4 * kMs);
    const switch_pagein_row *app = switch_pagein_find(w->file, kApp, false);
    ASSERT_NE(app, nullptr);
    EXPECT_STREQ(app->name, "");
    EXPECT_FALSE(switch_pagein_note_read(w.get(), 400, "SpringBoard", kApp, nullptr, 16384, kMs))
        << "a wait never names a file (it runs under the VM object's lock): the next read asks for the name";
    EXPECT_TRUE(switch_pagein_note_read(w.get(), 400, "SpringBoard", kApp, "SpringBoard", 16384, kMs));
    EXPECT_STREQ(app->name, "SpringBoard");
    EXPECT_EQ(app->reads, 1u);
    EXPECT_EQ(app->waits, 1u);
}

TEST(XnuDiagPagein, AWaitThatIsNotForAFileReadIsCountedApart)
{
    auto w = Window();
    switch_pagein_note_wait(w.get(), 400, "SpringBoard", 0, 7 * kMs);
    EXPECT_EQ(w->others, 1u);
    EXPECT_EQ(w->other_wait, 7 * kMs);
    EXPECT_EQ(w->waits, 0u);
    EXPECT_EQ(switch_pagein_find(w->process, 401, false), nullptr);
}

uint32_t Rows(const switch_pagein_row *rows)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < SWITCH_PAGEIN_ROWS; i++)
        n += rows[i].key != 0;
    return n;
}

TEST(XnuDiagPagein, AFullTableCountsWhatItCouldNotTake)
{
    auto w = Window();
    for (uint64_t vnode = 1; vnode <= SWITCH_PAGEIN_ROWS + 3; vnode++)
        switch_pagein_note_read(w.get(), 1, "launchd", vnode * 4096, "file", 4096, kMs);
    switch_pagein_note_wait(w.get(), 1, "launchd", (SWITCH_PAGEIN_ROWS + 9) * 4096, kMs);
    EXPECT_EQ(w->reads, SWITCH_PAGEIN_ROWS + 3);
    EXPECT_EQ(w->waits, 1u);
    EXPECT_EQ(w->untracked, SWITCH_PAGEIN_ROWS + 4 - Rows(w->file)) << "every page-in no file row took is counted";
}

// Keys whose hash lands on the same row: a ninth is refused after eight probes, not after a walk of the table.
TEST(XnuDiagPagein, ACrowdedRowRefusesAfterItsProbes)
{
    auto w = Window();
    std::vector<uint64_t> keys;
    for (uint64_t k = 1; keys.size() < SWITCH_PAGEIN_PROBES + 1; k++)
        if ((((k * 0x9e3779b97f4a7c15ull) >> 58) & (SWITCH_PAGEIN_ROWS - 1)) == 5)
            keys.push_back(k);
    for (uint64_t key : keys)
        switch_pagein_note_read(w.get(), 1, "launchd", key, "file", 4096, kMs);
    EXPECT_EQ(Rows(w->file), SWITCH_PAGEIN_PROBES);
    EXPECT_EQ(w->untracked, 1u);
    EXPECT_EQ(switch_pagein_find(w->file, keys.back(), false), nullptr);
    EXPECT_NE(switch_pagein_find(w->file, keys.front(), false), nullptr);
}

TEST(XnuDiagPagein, TheLineRanksByTimeReadingAndWaiting)
{
    auto w = Window();
    switch_pagein_note_read(w.get(), 400, "SpringBoard", kCache, "dyld_shared_cache_arm64e", 65536, 2 * kMs);
    switch_pagein_note_wait(w.get(), 400, "SpringBoard", kCache, 1044 * kMs);
    switch_pagein_note_read(w.get(), 659, "Preferences", kApp, "Preferences", 16384, 300 * kMs);
    switch_pagein_note_wait(w.get(), 63, "backboardd", 0, 3 * kMs);
    const switch_pagein_xnu xnu = {3400, 120, 900, 25600};
    char line[700];
    switch_pagein_format(line, sizeof(line), w.get(), &xnu, 1, 1);
    EXPECT_EQ(std::string(line),
              "2 reads, 80 KiB asked, 302 ms reading; 1 waits for pages being read, 1044 ms (longest "
              "1044 ms: SpringBoard[400] on dyld_shared_cache_arm64e); 1 other busy-page waits, 3 "
              "ms; untracked 0; XNU page-ins 3400, 120 evicted and paged in again (ghosts found, 900 added, the "
              "last 25600 evictions held); by process (reads/KiB asked/ms reading/waits/ms waiting): SpringBoard[400] "
              "1/64/2/1/1044, Preferences[659] 1/16/300/0/0");
    switch_pagein_format_files(line, sizeof(line), w.get(), 1, 1);
    EXPECT_EQ(std::string(line), " dyld_shared_cache_arm64e 1/64/2/1/1044, Preferences 1/16/300/0/0");
}

// A busy window at the report's 900 bytes: every process row and every file row is whole.
TEST(XnuDiagPagein, AFullWindowFitsItsTwoLines)
{
    auto w = Window();
    const switch_pagein_xnu xnu = {99999, 99999, 99999, 999999};
    for (uint64_t i = 1; i <= 6; i++) {
        const std::string process = "com.apple.WebKit.WebContent" + std::to_string(i);
        const std::string file = "dyld_shared_cache_arm64e.dylddata" + std::to_string(i);
        switch_pagein_note_read(w.get(), 10000 + (int)i, process.c_str(), i * 4096, file.c_str(), 999999999,
                                99999 * kMs);
        switch_pagein_note_wait(w.get(), 10000 + (int)i, process.c_str(), i * 4096, 99999 * kMs);
    }
    switch_pagein_note_wait(w.get(), 99999, "longest", 4096, 999999 * kMs);
    char line[900];
    const size_t prefix = sizeof("SwitchSched page-ins (10000 ms): ") - 1;
    EXPECT_LT(switch_pagein_format(line, sizeof(line) - prefix, w.get(), &xnu, 1, 1), sizeof(line) - prefix - 1);
    EXPECT_EQ(Count(line, "/"), 4u + 6u * 4) << "the legend's four and six whole process rows";
    const size_t file_prefix =
        sizeof("SwitchSched page-ins by file (10000 ms; reads/KiB asked/ms reading/waits/ms waiting):") - 1;
    EXPECT_LT(switch_pagein_format_files(line, sizeof(line) - file_prefix, w.get(), 1, 1),
              sizeof(line) - file_prefix - 1);
    EXPECT_EQ(Count(line, "/"), 6u * 4) << "six whole file rows";
}

TEST(XnuDiagPagein, ACutLineLosesOnlyItsLastRows)
{
    auto w = Window();
    switch_pagein_note_read(w.get(), 400, "SpringBoard", kCache, "dyld_shared_cache_arm64e", 65536, 2 * kMs);
    switch_pagein_note_read(w.get(), 659, "Preferences", kApp, "Preferences", 16384, 300 * kMs);
    char line[220];
    switch_pagein_format(line, sizeof(line), w.get(), nullptr, 1, 1);
    EXPECT_EQ(std::string(line).size(), sizeof(line) - 1);
    EXPECT_THAT(std::string(line), HasSubstr("; untracked 0; by process")) << "the counts come before the rows";
}

TEST(XnuDiagPagein, AnUnnamedFileIsShownAsSuch)
{
    auto w = Window();
    switch_pagein_note_wait(w.get(), 400, "SpringBoard", kApp, 4 * kMs);
    char line[600];
    switch_pagein_format(line, sizeof(line), w.get(), nullptr, 1, 1);
    EXPECT_THAT(std::string(line), HasSubstr("(longest 4 ms: SpringBoard[400] on (unnamed))"));
    switch_pagein_format_files(line, sizeof(line), w.get(), 1, 1);
    EXPECT_EQ(std::string(line), " (unnamed) 0/0/0/1/4");
}

TEST(XnuDiagPageinSource, BothTreesReportEachFilePagerReadAfterIt)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        const std::string source = switch_ios::test::SourceText(std::string("../") + tree + "/osfmk/vm/bsd_vm.c");
        EXPECT_THAT(source, HasSubstr("\n#include <switch/diag.h>\n")) << tree;
        const std::string body = switch_ios::test::Body(source, "\nvnode_pager_data_request(");
        const size_t read =
            body.find("vnode_pager_cluster_read(vnode_object, base_offset, offset, io_streaming, size)");
        const size_t hook =
            body.find("switch_diag_pagein(vnode_object->vnode_handle, (uint64_t)size, mach_absolute_time() - began);");
        ASSERT_NE(read, std::string::npos) << tree;
        ASSERT_NE(hook, std::string::npos) << tree;
        EXPECT_LT(read, hook) << tree;
        // A release kernel times nothing here: the timing and both reports are the debug branch's.
        const size_t debug = body.find("#if DEVELOPMENT || DEBUG\n");
        const size_t release = body.find(
            "#else\n\treturn vnode_pager_cluster_read(vnode_object, base_offset, offset, io_streaming, size);\n#endif");
        ASSERT_NE(debug, std::string::npos) << tree;
        ASSERT_NE(release, std::string::npos) << tree;
        EXPECT_LT(debug, body.find("const uint64_t began = mach_absolute_time();")) << tree;
        EXPECT_LT(hook, release) << tree;
    }
}

TEST(XnuDiagPageinSource, BothTreesReportEachBusyPageWaitAroundItsSleep)
{
    const struct {
        const char *tree, *object, *sleep;
    } trees[] = {
        {"xnu-10063.141.1", "o", "ret = thread_sleep_vm_object(o, m, interruptible);"},
        {"xnu-12377.121.6", "object", "\n#if PAGE_SLEEP_WITH_INHERITOR\n\tret = vm_page_sleep_with_inheritor("},
    };

    for (const auto &t : trees) {
        const std::string source = switch_ios::test::SourceText(std::string("../") + t.tree + "/osfmk/vm/vm_object.c");
        EXPECT_THAT(source, HasSubstr("\n#include <switch/diag.h>\n")) << t.tree;
        const std::string body = switch_ios::test::Body(source, "\nvm_page_sleep(");
        const std::string object = t.object;
        const size_t before = body.find("const struct switch_page_wait switch_wait = switch_diag_page_wait(" + object +
                                        "->internal ? NULL : " + object + "->pager, m->vmp_absent);");
        const size_t sleep = body.find(t.sleep);
        const size_t after = body.find("switch_diag_page_waited(&switch_wait);");
        ASSERT_NE(before, std::string::npos) << t.tree;
        ASSERT_NE(sleep, std::string::npos) << t.tree;
        ASSERT_NE(after, std::string::npos) << t.tree;
        EXPECT_LT(before, sleep) << t.tree << ": the page is read before the sleep (it may be freed while asleep)";
        EXPECT_LT(sleep, after) << t.tree;
    }
    // 10063's vm_fault.c waits for a busy page itself, not through vm_page_sleep, in three places (12377 calls
    // vm_page_sleep in all three): each is reported, so iOS 17's figures count what iOS 27's do.
    const std::string fault = switch_ios::test::SourceText("../xnu-10063.141.1/osfmk/vm/vm_fault.c");
    EXPECT_THAT(fault, HasSubstr("\n#include <switch/diag.h>\n"));
    EXPECT_EQ(switch_ios::test::Count(fault, "PAGE_ASSERT_WAIT("), 3u) << "a new busy wait: report it too";
    for (const char *wait : {"result = PAGE_ASSERT_WAIT(m, interruptible);", "\tPAGE_ASSERT_WAIT(m, interruptible);",
                             "PAGE_ASSERT_WAIT(copy_m, interruptible);"}) {
        const size_t at = fault.find(wait);
        ASSERT_NE(at, std::string::npos) << wait;
        const size_t before = fault.rfind("const struct switch_page_wait switch_wait = switch_diag_page_wait(", at);
        const size_t block = fault.find("thread_block(THREAD_CONTINUE_NULL);", at);
        const size_t after = fault.find("switch_diag_page_waited(&switch_wait);", at);
        ASSERT_NE(before, std::string::npos) << wait;
        EXPECT_LT(at - before, 200u) << wait << ": the wait's own hook, just before it";
        EXPECT_LT(block, after) << wait;
        EXPECT_LT(after - block, 120u) << wait << ": reported right after the block";
    }
    const std::string diag = switch_ios::test::SourceText("../switch/include/switch/diag.h");
    EXPECT_THAT(diag, HasSubstr("static inline void switch_diag_pagein("));
    EXPECT_THAT(diag, HasSubstr("static inline struct switch_page_wait switch_diag_page_wait("));
    EXPECT_THAT(diag, HasSubstr("static inline void switch_diag_page_waited("));
}

}  // namespace
