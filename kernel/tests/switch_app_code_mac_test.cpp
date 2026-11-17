#include <gtest/gtest.h>
#include "../switch/include/switch/app_code_rules.h"

namespace {
struct Ops { int (*mpo_file_check_mmap)(int); };
struct Policy { const char *mpc_name; const Ops *mpc_ops; };
int amfi_calls, sandbox_calls;
int amfi(int) { ++amfi_calls; return 80; }
int sandbox(int result) { ++sandbox_calls; return result; }
const Ops amfi_ops = {amfi}, sandbox_ops = {sandbox};
const Policy policies[] = {{"AMFI", &amfi_ops}, {"Sandbox", &sandbox_ops}};
int mac_error_select(int next, int previous) { return next ? next : previous; }
#define MAC_POLICY_ITERATE(body) do { for (const auto& policy : policies) { const auto *mpc = &policy; body } } while (0)
#define DTRACE_MACF3(...) ((void)0)
#define DTRACE_MACF2(...) ((void)0)
#include "app_code_mac_17.h"

int check17(bool raw, int sandbox_result)
{
    int error = 0;
    SWITCH_MAC_CHECK_17(raw, file_check_mmap, sandbox_result);
    return error;
}
#define MAC_CHECK_CALL(...) ((void)0)
#define MAC_CHECK_RSLT(...) ((void)0)
#include "app_code_mac_27.h"

int check27(bool raw, int sandbox_result)
{
    int error = 0;
    SWITCH_MAC_CHECK_27(raw, file_check_mmap, sandbox_result);
    return error;
}
}

TEST(SwitchAppCodeMac, RawAdmissionNeverBypassesSandbox)
{
    for (auto check : {check17, check27}) {
        amfi_calls = sandbox_calls = 0;
        EXPECT_EQ(check(true, 13), 13);
        EXPECT_EQ(amfi_calls, 0);
        EXPECT_EQ(sandbox_calls, 1);
        EXPECT_EQ(check(true, 0), 0);
        EXPECT_EQ(sandbox_calls, 2);
    }
}

TEST(SwitchAppCodeMac, StockAdmissionStillInvokesBothPolicies)
{
    for (auto check : {check17, check27}) {
        amfi_calls = sandbox_calls = 0;
        EXPECT_EQ(check(false, 0), 80);
        EXPECT_EQ(amfi_calls, 1);
        EXPECT_EQ(sandbox_calls, 1);
    }
}
