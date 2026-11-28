#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include "../switch/include/switch/app_code_rules.h"

namespace {
struct Ops { int (*mpo_file_check_mmap)(int); };
struct Policy { const char *mpc_name; const Ops *mpc_ops; int *mpc_field_off; };
struct label { uintptr_t slots[2]; };
struct ucred { label mac; };
using kauth_cred_t = ucred *;
label *mac_cred_label(kauth_cred_t cred) { return &cred->mac; }
uintptr_t mac_label_get(label *mac, int slot) { return mac->slots[slot]; }
int amfi_calls, sandbox_calls, other_calls, other_result;
int sandbox_slot = 1;
int amfi(int) { ++amfi_calls; return 80; }
int sandbox(int result) { ++sandbox_calls; return result; }
int other(int) { ++other_calls; return other_result; }
const Ops amfi_ops = {amfi}, sandbox_ops = {sandbox}, other_ops = {other};
Policy policies[] = {{"AMFI", &amfi_ops, nullptr}, {"Sandbox", &sandbox_ops, &sandbox_slot},
                     {"Other", &other_ops, nullptr}};
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

int check_bootstrap17(kauth_cred_t credential, int sandbox_result)
{
    int error = 0;
    SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED_17(false, credential, false, file_check_mmap, sandbox_result);
    return error;
}

int skip_sandbox_evals;
bool skip_sandbox_result;
bool eval_skip_sandbox()
{
    ++skip_sandbox_evals;
    return skip_sandbox_result;
}

int check_lazy17(int sandbox_result)
{
    int error = 0;
    SWITCH_BOOTSTRAP_MAC_CHECK_17(eval_skip_sandbox(), false, file_check_mmap, sandbox_result);
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

int check_bootstrap27(kauth_cred_t credential, int sandbox_result)
{
    int error = 0;
    SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED_27(false, credential, false, file_check_mmap, sandbox_result);
    return error;
}

int check_lazy27(int sandbox_result)
{
    int error = 0;
    SWITCH_BOOTSTRAP_MAC_CHECK_27(eval_skip_sandbox(), false, file_check_mmap, sandbox_result);
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

TEST(SwitchAppCodeMac, SignedMappingOnlySkipsSandboxForAnEmptyRegisteredSlot)
{
    for (auto check : {check_bootstrap17, check_bootstrap27}) {
        ucred unconfined{{{1, 0}}};
        ucred sandboxed{{{0, 1}}};
        amfi_calls = sandbox_calls = other_calls = 0;
        EXPECT_EQ(check(&unconfined, 13), 80);
        EXPECT_EQ(sandbox_calls, 0);
        EXPECT_EQ(check(&sandboxed, 13), 13);
        EXPECT_EQ(check(nullptr, 13), 13);
        policies[1].mpc_field_off = nullptr;
        EXPECT_EQ(check(&unconfined, 13), 13);
        policies[1].mpc_field_off = &sandbox_slot;
        EXPECT_EQ(sandbox_calls, 3);
        EXPECT_EQ(amfi_calls, 4);
        EXPECT_EQ(other_calls, 4);
        other_result = 22;
        EXPECT_EQ(check(&unconfined, 13), 22);
        other_result = 0;
        EXPECT_EQ(sandbox_calls, 3);
        EXPECT_EQ(amfi_calls, 5);
        EXPECT_EQ(other_calls, 5);
    }
}

TEST(SwitchAppCodeMac, BootstrapMacCheckEvaluatesSkipSandboxLazilyOnDenialOnly)
{
    for (auto check : {check_lazy17, check_lazy27}) {
        amfi_calls = sandbox_calls = other_calls = skip_sandbox_evals = 0;
        skip_sandbox_result = true;
        other_result = 0;
        EXPECT_EQ(check(0), 80);
        EXPECT_EQ(sandbox_calls, 1);
        EXPECT_EQ(skip_sandbox_evals, 0);

        EXPECT_EQ(check(13), 80);
        EXPECT_EQ(sandbox_calls, 2);
        EXPECT_EQ(skip_sandbox_evals, 1);

        skip_sandbox_result = false;
        EXPECT_EQ(check(13), 13);
        EXPECT_EQ(sandbox_calls, 3);
        EXPECT_EQ(skip_sandbox_evals, 2);

        skip_sandbox_result = true;
        other_result = 22;
        EXPECT_EQ(check(13), 22);
        EXPECT_EQ(skip_sandbox_evals, 3);
        other_result = 0;
    }
}
