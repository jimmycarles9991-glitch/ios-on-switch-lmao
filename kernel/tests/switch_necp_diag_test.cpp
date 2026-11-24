// <switch/necp.h>: the first local-network drop of each of SWITCH_NECP_PROBE_PIDS processes is logged; iOS 27's NECP
// reports client drops whether or not their alert is suppressed and socket drops it alerts on, from inside its policy
// lock, with the last skip its evaluation took. A drop dumps the policy table on the first drop of a boot and on the
// first after an add or delete in the Local Network window, four dumps at most, the dropping session's window policies
// always, the rest while the budget lasts; adds and deletes in the window are logged, 256 at most, then counted.
// <switch/cryptex_map.h>: a shared cache refused for its volume is logged, 16 at most. iOS 17's tree has no hook, and
// release compiles every hook to nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/source_text.h"
#include "switch/necp.h"

using testing::ElementsAreArray;
using testing::HasSubstr;
using testing::Not;

namespace {

struct Policy {
    uint32_t id;
    uint32_t session_order;
    uint32_t order;
    bool skip;
    const Policy *next;
};

// The ids a dump prints for `table` (sorted as NECP sorts it), dropping in `drop_session_order`.
std::vector<uint32_t> Printed(std::vector<Policy> &table, uint32_t drop_session_order, switch_necp_table_counts *counts)
{
    for (size_t i = 0; i + 1 < table.size(); i++) {
        table[i].next = &table[i + 1];
    }
    std::vector<uint32_t> printed;
    const switch_necp_table_walker walker = {
        .next = [](const void *policy) -> const void * {
            return static_cast<const Policy *>(policy)->next;
        },
        .row =
            [](const void *policy) {
                const auto *p = static_cast<const Policy *>(policy);
                return switch_necp_row{.session_order = p->session_order, .order = p->order, .skip = p->skip};
            },
        .print =
            [](void *context, const void *policy) {
                static_cast<std::vector<uint32_t> *>(context)->push_back(static_cast<const Policy *>(policy)->id);
            },
        .context = &printed,
    };
    *counts = {};
    switch_necp_table_walk(table.empty() ? nullptr : table.data(), drop_session_order, &walker, counts);
    return printed;
}

TEST(SwitchNecpDiag, EachPidIsClaimedOnce)
{
    int32_t slots[4] = {};
    EXPECT_TRUE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_TRUE(switch_necp_probe_claim(slots, 4, 495));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 495));
}

TEST(SwitchNecpDiag, AFullTableAndTheKernelClaimNothing)
{
    int32_t slots[2] = {};
    EXPECT_TRUE(switch_necp_probe_claim(slots, 2, 1));
    EXPECT_TRUE(switch_necp_probe_claim(slots, 2, 2));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 2, 3));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 2, 1));
    int32_t empty[2] = {};
    EXPECT_FALSE(switch_necp_probe_claim(empty, 2, 0));
    EXPECT_EQ(empty[0], 0);
}

TEST(SwitchNecpTable, TheWindowHoldsTheLocalNetworkRules)
{
    EXPECT_FALSE(switch_necp_in_window(17999));
    EXPECT_TRUE(switch_necp_in_window(18000));
    for (const uint32_t order : {18070u, 18075u, 18080u, 18090u, 18092u, 18104u, 18120u}) {
        EXPECT_TRUE(switch_necp_in_window(order)) << order;
    }
    EXPECT_TRUE(switch_necp_in_window(18200));
    EXPECT_FALSE(switch_necp_in_window(18201));
    EXPECT_FALSE(switch_necp_in_window(130));
}

TEST(SwitchNecpTable, PoliciesFallIntoTheDumpsGroups)
{
    EXPECT_EQ(switch_necp_table_group(2013, 18075, true, 2013), SWITCH_NECP_TABLE_OWN_WINDOW);
    EXPECT_EQ(switch_necp_table_group(2013, 18090, false, 2013), SWITCH_NECP_TABLE_OWN_WINDOW);
    EXPECT_EQ(switch_necp_table_group(1500, 18075, true, 2013), SWITCH_NECP_TABLE_OTHER_WINDOW);
    EXPECT_EQ(switch_necp_table_group(2013, 130, true, 2013), SWITCH_NECP_TABLE_OWN_SKIP);
    EXPECT_EQ(switch_necp_table_group(2013, 120, false, 2013), SWITCH_NECP_TABLE_GROUPS);
    EXPECT_EQ(switch_necp_table_group(1500, 130, true, 2013), SWITCH_NECP_TABLE_GROUPS);
    EXPECT_EQ(switch_necp_table_group(2014, 18201, true, 2013), SWITCH_NECP_TABLE_GROUPS);
}

TEST(SwitchNecpTable, TheDroppingSessionsWindowIsPrintedFirstWhateverComesBeforeIt)
{
    std::vector<Policy> table;
    for (uint32_t i = 0; i < 100; i++) {
        table.push_back({1000 + i, 1500, 18000 + i, false, nullptr});
    }
    for (uint32_t i = 0; i < 40; i++) {
        table.push_back({2000 + i, 2013, 100 + i, i % 4 == 0, nullptr});
    }
    table.push_back({61, 2013, 18070, true, nullptr});
    table.push_back({77, 2013, 18075, true, nullptr});
    table.push_back({79, 2013, 18090, false, nullptr});
    table.push_back({82, 2013, 18092, false, nullptr});
    for (uint32_t i = 0; i < 5; i++) {
        table.push_back({3000 + i, 3000, 18100 + i, false, nullptr});
    }
    switch_necp_table_counts counts;
    const std::vector<uint32_t> printed = Printed(table, 2013, &counts);
    ASSERT_GE(printed.size(), 4u);
    EXPECT_THAT(std::vector<uint32_t>(printed.begin(), printed.begin() + 4), ElementsAreArray({61u, 77u, 79u, 82u}));
    EXPECT_EQ(printed.size(), SWITCH_NECP_TABLE_POLICY_LINES);
    EXPECT_EQ(printed[4], 1000u);
    EXPECT_EQ(counts.walked, table.size());
    EXPECT_EQ(counts.wanted[SWITCH_NECP_TABLE_OWN_WINDOW], 4u);
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OWN_WINDOW], 4u);
    EXPECT_EQ(counts.wanted[SWITCH_NECP_TABLE_OTHER_WINDOW], 105u);
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OTHER_WINDOW], SWITCH_NECP_TABLE_POLICY_LINES - 4u);
    EXPECT_EQ(counts.wanted[SWITCH_NECP_TABLE_OWN_SKIP], 10u);
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OWN_SKIP], 0u);
}

TEST(SwitchNecpTable, TheSessionsSkipsOutsideTheWindowTakeWhatIsLeft)
{
    std::vector<Policy> table;
    for (uint32_t i = 0; i < 10; i++) {
        table.push_back({2000 + i, 2013, 100 + i, i < 3, nullptr});
    }
    table.push_back({77, 2013, 18075, true, nullptr});
    table.push_back({5, 3000, 18080, false, nullptr});
    switch_necp_table_counts counts;
    EXPECT_THAT(Printed(table, 2013, &counts), ElementsAreArray({77u, 5u, 2000u, 2001u, 2002u}));
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OWN_SKIP], 3u);
}

TEST(SwitchNecpTable, TheDroppingSessionsWindowRunsPastADumpsUsualSizeUpToItsOwnCap)
{
    std::vector<Policy> table;
    for (uint32_t i = 0; i < 80; i++) {
        table.push_back({100 + i, 2013, 18000 + i, i % 2 == 0, nullptr});
    }
    table.push_back({5, 3000, 18080, false, nullptr});
    switch_necp_table_counts counts;
    EXPECT_EQ(Printed(table, 2013, &counts).size(), 80u);
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OTHER_WINDOW], 0u);
    EXPECT_EQ(counts.wanted[SWITCH_NECP_TABLE_OTHER_WINDOW], 1u);

    for (uint32_t i = 80; i < 200; i++) {
        table.push_back({100 + i, 2013, 18000 + i, false, nullptr});
    }
    EXPECT_EQ(Printed(table, 2013, &counts).size(), SWITCH_NECP_TABLE_OWN_LINES);
    EXPECT_EQ(counts.wanted[SWITCH_NECP_TABLE_OWN_WINDOW], 200u);
    EXPECT_EQ(counts.printed[SWITCH_NECP_TABLE_OWN_WINDOW], SWITCH_NECP_TABLE_OWN_LINES);
}

TEST(SwitchNecpTable, TheFirstDropOfABootDumpsThenOnlyAfterTheTableChanged)
{
    uint64_t state = 0;
    EXPECT_EQ(switch_necp_dump_claim(&state, 0), 1u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 0), 0u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 0), 0u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 7), 2u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 7), 0u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 8), 3u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 0), 4u);
}

TEST(SwitchNecpTable, ABootDumpsFourTimesAtMost)
{
    uint64_t state = 0;
    for (uint32_t generation = 1; generation <= SWITCH_NECP_TABLE_DUMPS; generation++) {
        EXPECT_EQ(switch_necp_dump_claim(&state, generation), generation);
    }
    EXPECT_EQ(SWITCH_NECP_TABLE_DUMPS, 4u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 100), 0u);
    EXPECT_EQ(switch_necp_dump_claim(&state, 101), 0u);
}

TEST(SwitchNecpTable, OnlyChangesInTheWindowMoveTheGenerationAWaitingDumpSees)
{
    switch_necp_timeline timeline = {};
    uint64_t dumps = 0;
    EXPECT_EQ(switch_necp_dump_claim(&dumps, timeline.generation), 1u);
    uint32_t generation = 0;
    for (const uint32_t order : {100u, 17999u, 18201u, 40000u}) {
        EXPECT_FALSE(switch_necp_policy_account(&timeline, order, &generation)) << order;
    }
    EXPECT_EQ(timeline.generation, 0u);
    EXPECT_EQ(timeline.lines, 0u);
    EXPECT_EQ(switch_necp_dump_claim(&dumps, timeline.generation), 0u);
    EXPECT_TRUE(switch_necp_policy_account(&timeline, 18075, &generation));
    EXPECT_EQ(generation, 1u);
    EXPECT_EQ(switch_necp_dump_claim(&dumps, timeline.generation), 2u);
}

TEST(SwitchNecpTable, TimelineLinesPastTheCapAreCountedAndStillMoveTheGeneration)
{
    switch_necp_timeline timeline = {};
    uint32_t generation = 0;
    for (uint32_t i = 0; i < SWITCH_NECP_POLICY_LINES; i++) {
        EXPECT_TRUE(switch_necp_policy_account(&timeline, 18090, &generation)) << i;
        EXPECT_EQ(generation, i + 1);
    }
    EXPECT_FALSE(switch_necp_policy_account(&timeline, 18075, &generation));
    EXPECT_FALSE(switch_necp_policy_account(&timeline, 18075, &generation));
    EXPECT_FALSE(switch_necp_policy_account(&timeline, 130, &generation));
    EXPECT_EQ(timeline.suppressed, 2u);
    EXPECT_EQ(timeline.generation, SWITCH_NECP_POLICY_LINES + 2u);
    EXPECT_EQ(SWITCH_NECP_POLICY_LINES, 256u);
}

TEST(SwitchNecpTable, LinesStopAtTheirCapAndNeverWrap)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < SWITCH_NECP_POLICY_LINES; i++) {
        EXPECT_TRUE(switch_necp_take(&used, SWITCH_NECP_POLICY_LINES)) << i;
    }
    EXPECT_FALSE(switch_necp_take(&used, SWITCH_NECP_POLICY_LINES));
    EXPECT_EQ(used, SWITCH_NECP_POLICY_LINES);
    uint32_t full = UINT32_MAX;
    EXPECT_FALSE(switch_necp_take(&full, SWITCH_NECP_POLICY_LINES));
    EXPECT_EQ(full, UINT32_MAX);
    EXPECT_EQ(SWITCH_NECP_TABLE_LINES, 64u);
    EXPECT_EQ(SWITCH_NECP_TABLE_POLICY_LINES + 3u, SWITCH_NECP_TABLE_LINES);
}

TEST(SwitchNecpDiagSource, LocalNetworkDropsAreReportedUnderThePolicyLock)
{
    const std::string necp = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.c");
    EXPECT_THAT(necp, HasSubstr("#include <switch/necp.h>"));
    const std::string client = switch_ios::test::Body(necp, "\nnecp_application_find_policy_match_internal(");
    EXPECT_THAT(client, HasSubstr("NECP_KERNEL_POLICY_DROP_FLAG_LOCAL_NETWORK) {\n"
                                  "\t\tSWITCH_NECP_CLIENT_LOCAL_DROP(info, matched_policy, skip_policy_id, proc, "
                                  "effective_proc, responsible_proc, "
                                  "necp_kernel_application_policies_condition_mask);\n"
                                  "\t\tif (!(matched_policy->result_parameter.drop_flags & "
                                  "NECP_KERNEL_POLICY_DROP_FLAG_SUPPRESS_ALERTS)) {\n"));
    const std::string update = switch_ios::test::Body(necp, "\nnecp_socket_find_policy_match(");
    const std::string data = switch_ios::test::Body(necp, "\nnecp_socket_is_allowed_to_send_recv_internal(");
    for (const auto &[body, site] : {std::pair{update, "2"}, std::pair{data, "3"}}) {
        const std::string hook =
            std::string("\t\t\tsend_local_network_denied_event = true;\n\t\t\tSWITCH_NECP_SOCKET_LOCAL_DROP(") + site;
        const size_t at = body.find(hook);
        ASSERT_NE(at, std::string::npos) << site;
        EXPECT_LT(at, body.find("lck_rw_done(&necp_kernel_policy_lock);\n\n\tif (")) << site;
    }
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/necp.h");
    EXPECT_THAT(header, HasSubstr(".policies = LIST_FIRST(&necp_kernel_socket_policies)"));
    const std::string older = switch_ios::test::SourceText("../xnu-10063.141.1/bsd/net/necp.c");
    EXPECT_THAT(older, Not(HasSubstr("SWITCH_NECP_")));
}

TEST(SwitchNecpDiagSource, TheLastSkipAnEvaluationTookRidesOnItsInfo)
{
    const std::string necp = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.c");
    EXPECT_THAT(necp, HasSubstr("\tunsigned __pad_bits : 7;\n\tSWITCH_NECP_INFO_FIELDS\n};\n"));
    const std::string find = switch_ios::test::Body(necp, "\nnecp_socket_find_policy_match_with_info_locked(");
    EXPECT_THAT(find, HasSubstr("\t\t\t\t\tskip_session_order = policy_search_array[i]->session_order + 1;\n"
                                "\t\t\t\t\tSWITCH_NECP_SKIP_TAKEN(info, policy_search_array[i]);\n"));
    EXPECT_EQ(switch_ios::test::Count(necp, "SWITCH_NECP_SKIP_TAKEN("), 1u);
    EXPECT_EQ(switch_ios::test::Count(necp, "necp_socket_find_policy_match_with_info_locked("), 5u);
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/necp.h");
    EXPECT_THAT(header, HasSubstr(".last_skip = (info).switch_last_skip"));
}

TEST(SwitchNecpDiagSource, EverySocketPolicyAddAndDeleteIsSeenUnderTheExclusiveLock)
{
    const std::string necp = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.c");
    const std::string add = switch_ios::test::Body(necp, "\nnecp_kernel_socket_policy_add(");
    EXPECT_THAT(add,
                HasSubstr("\tSWITCH_NECP_POLICY_ADDED(new_kernel_policy);\n"
                          "\tLIST_INSERT_SORTED_TWICE_ASCENDING(&necp_kernel_socket_policies, new_kernel_policy,"));
    const std::string remove = switch_ios::test::Body(necp, "\nnecp_kernel_socket_policy_delete(");
    EXPECT_THAT(remove, HasSubstr("LCK_RW_ASSERT(&necp_kernel_policy_lock, LCK_RW_ASSERT_EXCLUSIVE);"));
    EXPECT_THAT(remove, HasSubstr("\t\tLIST_REMOVE(policy, chain);\n\t\tSWITCH_NECP_POLICY_DELETED(policy);\n"));
    EXPECT_EQ(switch_ios::test::Count(necp, "necp_kernel_socket_policy_add("), 3u);
    const std::string apply = switch_ios::test::Body(necp, "\nnecp_policy_apply_all(");
    EXPECT_THAT(apply, HasSubstr("lck_rw_lock_exclusive(&necp_kernel_policy_lock);"));
}

TEST(SwitchNecpSigningIdSource, SigningIdentifierIsEvaluatedOutsideAppIdAndRetriesOnDrop)
{
    const std::string necp = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.c");
    const std::string header = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.h");
    EXPECT_THAT(header, HasSubstr("#define NECP_POLICY_DROP_FLAG_RETRY_SIGNING_IDENTIFIER  0x08"));
    EXPECT_THAT(header, HasSubstr("#define NECP_KERNEL_POLICY_DROP_FLAG_RETRY_SIGNING_IDENTIFIER   "
                                  "NECP_POLICY_DROP_FLAG_RETRY_SIGNING_IDENTIFIER"));
    const std::string check = switch_ios::test::Body(necp, "\nnecp_socket_check_policy(");
    const std::string app_block = switch_ios::test::Between(
        check, "if (kernel_policy->condition_mask & NECP_KERNEL_CONDITION_APP_ID) {", "\n\t}\n");
    EXPECT_THAT(app_block, Not(HasSubstr("NECP_KERNEL_CONDITION_SIGNING_IDENTIFIER")));
    EXPECT_THAT(
        check, HasSubstr("if (!check_signing_id && !(kernel_policy->condition_mask & NECP_KERNEL_CONDITION_APP_ID)) {\n"
                         "\t\t\tif (skipped_signing_id != NULL) {\n"
                         "\t\t\t\t*skipped_signing_id = true;\n"
                         "\t\t\t}\n"
                         "\t\t\treturn FALSE;\n"
                         "\t\t}"));
    const std::string find = switch_ios::test::Body(necp, "\nnecp_socket_find_policy_match_with_info_locked(");
    EXPECT_THAT(find, HasSubstr("policy_search_array[i]->result == NECP_KERNEL_POLICY_RESULT_DROP &&\n"
                                "\t\t\t\t    skipped_signing_id &&\n"
                                "\t\t\t\t    (policy_search_array[i]->result_parameter.drop_flags & "
                                "NECP_KERNEL_POLICY_DROP_FLAG_RETRY_SIGNING_IDENTIFIER)"));
    EXPECT_THAT(find, HasSubstr("\"RETRY SESSION (Signing Identifier Drop)\""));
    const size_t trace_pos = find.find("\"RETRY SESSION (Signing Identifier Drop)\"");
    const size_t rewind_pos = find.find("i = session_start_index - 1;");
    ASSERT_NE(trace_pos, std::string::npos);
    ASSERT_NE(rewind_pos, std::string::npos);
    EXPECT_LT(trace_pos, rewind_pos);
    const std::string unnecessary = switch_ios::test::Body(necp, "\nnecp_kernel_socket_policy_is_unnecessary(");
    EXPECT_THAT(unnecessary, HasSubstr("compared_policy->condition_mask & NECP_KERNEL_CONDITION_SIGNING_IDENTIFIER"));
    const std::string older = switch_ios::test::SourceText("../xnu-10063.141.1/bsd/net/necp.c");
    EXPECT_THAT(older, Not(HasSubstr("RETRY_SIGNING_IDENTIFIER")));
}

TEST(SwitchCryptexMapSource, ARefusalIsLoggedBeforeItsErrorIsReplaced)
{
    const std::string vm_unix = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/vm/vm_unix.c");
    EXPECT_THAT(vm_unix, HasSubstr("#include <switch/cryptex_map.h>\n"));
    EXPECT_THAT(vm_unix, HasSubstr("\"not on process' root volume nor preboot volume\\n\",\n"
                                   "\t\t\t\t\t(void *)VM_KERNEL_ADDRPERM(current_thread()),\n"
                                   "\t\t\t\t\tproc_getpid(p), p->p_comm,\n"
                                   "\t\t\t\t\t(void *)VM_KERNEL_ADDRPERM(srfmp->vp),\n"
                                   "\t\t\t\t\tsrfmp->vp->v_name));\n"
                                   "\t\t\t\tSWITCH_CRYPTEX_MAP_REFUSED(p, srfmp->vp, preboot_vp, error);\n"
                                   "\t\t\t\terror = EPERM;\n"));
    EXPECT_EQ(switch_ios::test::Count(vm_unix, "SWITCH_CRYPTEX_MAP_REFUSED("), 1u);
    EXPECT_THAT(vm_unix, HasSubstr("#define PREBOOT_CRYPTEX_PATH \"/private/preboot/Cryptexes\"\n"));
    EXPECT_THAT(vm_unix,
                HasSubstr("vnode_lookup(PREBOOT_CRYPTEX_PATH \"/OS\", 0, &cryptex_vp, vfs_context_current())"));
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/cryptex_map.h");
    EXPECT_THAT(header, HasSubstr("#define SWITCH_CRYPTEX_PREBOOT_DIR \"/private/preboot/Cryptexes\"\n"));
    EXPECT_THAT(header, HasSubstr("#define SWITCH_CRYPTEX_MAP_LINES   16u\n"));
    const std::string source = switch_ios::test::SourceText("../switch/diag/cryptex_map.c");
    EXPECT_THAT(source, HasSubstr("vnode_lookup(SWITCH_CRYPTEX_PREBOOT_DIR \"/OS\", 0, &cryptex"));
    EXPECT_THAT(source, HasSubstr(">= SWITCH_CRYPTEX_MAP_LINES) {\n        return;\n"));
    const std::string older = switch_ios::test::SourceText("../xnu-10063.141.1/bsd/vm/vm_unix.c");
    EXPECT_THAT(older, Not(HasSubstr("SWITCH_CRYPTEX")));
}

TEST(SwitchNecpDiagSource, ReleaseKernelsCompileItToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/necp.h");
    EXPECT_THAT(header, HasSubstr("    } while (0)\n#define SWITCH_NECP_CLIENT_LOCAL_DROP(info, policy, skip, "
                                  "caller_proc, effective, responsible_proc, mask) \\\n    do {"));
    EXPECT_THAT(header, HasSubstr("#define SWITCH_NECP_POLICY_ADDED(policy) \\\n    do {"));
    EXPECT_THAT(header, HasSubstr("#define SWITCH_NECP_POLICY_DELETED(policy) \\\n    do {"));
    EXPECT_THAT(header, HasSubstr("#else\n#define SWITCH_NECP_INFO_FIELDS\n"
                                  "#define SWITCH_NECP_SKIP_TAKEN(info, policy) \\\n    do {"));
    const std::string cryptex = switch_ios::test::SourceText("../switch/include/switch/cryptex_map.h");
    EXPECT_THAT(cryptex, HasSubstr("#else\n#define SWITCH_CRYPTEX_MAP_REFUSED(p, cache, preboot, preboot_error) \\\n"
                                   "    do {"));
    for (const char *file : {"../switch/diag/necp.c", "../switch/diag/necp_table.c", "../switch/diag/cryptex_map.c"}) {
        const std::string source = switch_ios::test::SourceText(file);
        EXPECT_THAT(source, HasSubstr("\n#if DEVELOPMENT || DEBUG\n")) << file;
        EXPECT_THAT(source, HasSubstr("\n#endif /* DEVELOPMENT || DEBUG */\n")) << file;
    }
}

}  // namespace
