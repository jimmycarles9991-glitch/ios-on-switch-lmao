// kernel/switch/include/switch/kprof_role.h on the host: main-thread vs worker user-sample bucketing, per-CPU merge,
// per-role top-K selection and user/images line formatting.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "switch/kprof_role.h"

namespace {

TEST(XnuKprofRole, MainAndWorkerSamplesAtTheSamePCStaySeparate)
{
    std::vector<switch_kprof_user> table(64);
    const uint64_t loc = SWITCH_KPROF_U_DSC | 0x180000054ull;
    const uint64_t main_key = switch_kprof_user_key(loc, true);
    const uint64_t worker_key = switch_kprof_user_key(loc, false);

    ASSERT_NE(main_key, worker_key);
    EXPECT_TRUE(switch_kprof_user_is_main(main_key));
    EXPECT_FALSE(switch_kprof_user_is_main(worker_key));
    EXPECT_EQ(switch_kprof_user_addr(main_key), 0x180000040ull);
    EXPECT_EQ(switch_kprof_user_addr(worker_key), 0x180000040ull);

    switch_kprof_user *m = switch_kprof_user_slot(table.data(), 6, 30, main_key);
    switch_kprof_user *w = switch_kprof_user_slot(table.data(), 6, 30, worker_key);
    ASSERT_NE(m, nullptr);
    ASSERT_NE(w, nullptr);
    ASSERT_NE(m, w);
    m->samples += 12;
    w->samples += 5;

    EXPECT_EQ(switch_kprof_user_slot(table.data(), 6, 30, main_key)->samples, 12u);
    EXPECT_EQ(switch_kprof_user_slot(table.data(), 6, 30, worker_key)->samples, 5u);

    const uint64_t other_main = switch_kprof_user_key(SWITCH_KPROF_U_OTHER | 0xdeadbeefull, true);
    const uint64_t other_worker = switch_kprof_user_key(SWITCH_KPROF_U_OTHER | 0x1234ull, false);
    EXPECT_NE(other_main, other_worker);
    EXPECT_EQ(switch_kprof_user_kind(other_main), SWITCH_KPROF_U_OTHER);
    EXPECT_EQ(switch_kprof_user_addr(other_main), 0ull);
}

TEST(XnuKprofRole, RegionKeyPreservesThreadRoleAndAlignsToOneMiB)
{
    const uint64_t main_key = switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x192345678ull, true);
    const uint64_t worker_key = switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x192345678ull, false);
    const uint64_t main_reg = switch_kprof_user_region_key(main_key);
    const uint64_t worker_reg = switch_kprof_user_region_key(worker_key);

    EXPECT_NE(main_reg, worker_reg);
    EXPECT_TRUE(switch_kprof_user_is_main(main_reg));
    EXPECT_FALSE(switch_kprof_user_is_main(worker_reg));
    EXPECT_EQ(switch_kprof_user_addr(main_reg), 0x192300000ull);
    EXPECT_EQ(switch_kprof_user_addr(worker_reg), 0x192300000ull);
}

TEST(XnuKprofRole, MergeAcrossCPUsCombinesMatchingRolesAndCountsLost)
{
    std::vector<switch_kprof_user> cpu0(32), cpu1(32), merged(64);
    const uint64_t k_main = switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x180000040ull, true);
    const uint64_t k_work = switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x180000040ull, false);

    switch_kprof_user_slot(cpu0.data(), 5, 42, k_main)->samples = 10;
    switch_kprof_user_slot(cpu0.data(), 5, 42, k_work)->samples = 7;
    switch_kprof_user_slot(cpu1.data(), 5, 42, k_main)->samples = 15;

    uint32_t lost = 0;
    switch_kprof_user_merge(merged.data(), 6, cpu0.data(), 32, &lost);
    switch_kprof_user_merge(merged.data(), 6, cpu1.data(), 32, &lost);

    EXPECT_EQ(lost, 0u);
    EXPECT_EQ(switch_kprof_user_slot(merged.data(), 6, 42, k_main)->samples, 25u);
    EXPECT_EQ(switch_kprof_user_slot(merged.data(), 6, 42, k_work)->samples, 7u);

    // A full 16-slot destination table refuses a 17th distinct key and adds its samples to lost.
    std::vector<switch_kprof_user> tiny(16);
    std::vector<switch_kprof_user> many(64);
    for (unsigned i = 0; i < 20; i++) {
        const uint64_t k = switch_kprof_user_key(SWITCH_KPROF_U_DSC | (0x180000000ull + (uint64_t)i * 64), true);
        switch_kprof_user_slot(many.data(), 6, 42, k)->samples = 3;
    }
    switch_kprof_user_merge(tiny.data(), 4, many.data(), 64, &lost);
    EXPECT_GT(lost, 0u);
}

TEST(XnuKprofRole, FormatUserProcSplitsMainAndWorkerAndHandlesSingleRole)
{
    std::vector<switch_kprof_user> table(64);
    const int32_t pid = 30;
    switch_kprof_user_slot(table.data(), 6, pid, switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x180000040ull, true))
        ->samples = 45;
    switch_kprof_user_slot(table.data(), 6, pid, switch_kprof_user_key(SWITCH_KPROF_U_MAIN | 0x1020ull, true))
        ->samples = 15;
    switch_kprof_user_slot(table.data(), 6, pid, switch_kprof_user_key(SWITCH_KPROF_U_OTHER, true))->samples = 10;
    switch_kprof_user_slot(table.data(), 6, pid, switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x192000040ull, false))
        ->samples = 35;
    switch_kprof_user_slot(table.data(), 6, pid, switch_kprof_user_key(SWITCH_KPROF_U_OTHER, false))->samples = 15;

    char line[512];
    std::string expected = "SpringBoard[30] 120 (main 70, worker 50): ";
    expected += "main: dsc 180000040 45, main+ 1000 15, other 10; ";
    expected += "worker: dsc 192000040 35, other 15;";
    switch_kprof_format_user_proc(line, sizeof(line), "SpringBoard", pid, 120, 70, table.data(), 64, 8);
    EXPECT_EQ(std::string(line), expected);

    // Main-only process omits the worker clause.
    switch_kprof_format_user_proc(line, sizeof(line), "SpringBoard", pid, 70, 70, table.data(), 64, 8);
    EXPECT_EQ(std::string(line).find("worker:"), std::string::npos);
    EXPECT_NE(std::string(line).find("main: dsc 180000040 45"), std::string::npos);

    // Worker-only process omits the main clause.
    switch_kprof_format_user_proc(line, sizeof(line), "backboardd", pid, 50, 0, table.data(), 64, 8);
    EXPECT_EQ(std::string(line).find("main:"), std::string::npos);
    EXPECT_NE(std::string(line).find("worker: dsc 192000040 35"), std::string::npos);

    // Small buffer truncates cleanly without overflowing.
    char tiny[24];
    const size_t n =
        switch_kprof_format_user_proc(tiny, sizeof(tiny), "SpringBoard", pid, 120, 70, table.data(), 64, 8);
    EXPECT_LT(n, sizeof(tiny));
    EXPECT_EQ(tiny[sizeof(tiny) - 1], '\0');
}

TEST(XnuKprofRole, FormatImagesRoleReportsPerRoleRegionsRestAndLost)
{
    std::vector<switch_kprof_user> images(64);
    const int32_t pid = 30;
    const uint64_t r_main_dsc =
        switch_kprof_user_region_key(switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x180012340ull, true));
    const uint64_t r_main_bin =
        switch_kprof_user_region_key(switch_kprof_user_key(SWITCH_KPROF_U_MAIN | 0x1040ull, true));
    const uint64_t r_work_dsc =
        switch_kprof_user_region_key(switch_kprof_user_key(SWITCH_KPROF_U_DSC | 0x192045000ull, false));

    switch_kprof_user_slot(images.data(), 6, pid, r_main_dsc)->samples = 55;
    switch_kprof_user_slot(images.data(), 6, pid, r_main_bin)->samples = 10;
    switch_kprof_user_slot(images.data(), 6, pid, r_work_dsc)->samples = 40;

    char line[512];
    switch_kprof_format_images_role(line, sizeof(line), "SpringBoard", pid, true, 70, 120, images.data(), 64, 24, 2);
    EXPECT_STREQ(line, "SwitchKprof images: SpringBoard[30] main 70/120: dsc 1800 55, main+ 0 10, rest 5; lost 2");

    switch_kprof_format_images_role(line, sizeof(line), "SpringBoard", pid, false, 50, 120, images.data(), 64, 24, 2);
    EXPECT_STREQ(line, "SwitchKprof images: SpringBoard[30] worker 50/120: dsc 1920 40, rest 10; lost 2");
}

}  // namespace
