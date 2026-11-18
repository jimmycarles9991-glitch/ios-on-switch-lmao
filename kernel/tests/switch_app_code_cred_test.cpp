#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include "../xnu-12377.121.6/osfmk/kern/cs_blobs.h"
#include "../switch/include/switch/app_code_rules.h"
#define SWITCH_APP_CODE_CS_FLAGS (CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED)

namespace {
struct label { uintptr_t slots[3]; };
struct Credential { label policy; unsigned cr_flags = 0; };
using kauth_cred_t = Credential *;
using posix_cred_t = Credential *;
struct proc {};
struct vnode { bool raw; bool bootstrap = false; bool tool = true; };
struct Context { Credential *credential; proc process; };
using vfs_context_t = Context *;
using mpo_cred_label_update_execve_t = int(kauth_cred_t, kauth_cred_t, proc *, vnode *, off_t,
    vnode *, label *, label *, label *, unsigned *, void *, size_t, int *);
struct Operations {
    mpo_cred_label_update_execve_t *mpo_cred_label_update_execve;
    void (*mpo_cred_label_destroy)(label *);
    void (*mpo_cred_label_init)(label *);
};
struct mac_policy_conf { const char *mpc_name; Operations *mpc_ops; int *mpc_field_off; };
struct Entry { mac_policy_conf *mpc; };
struct { unsigned staticmax, maxindex; Entry entries[3]; } mac_policy_list;
unsigned destroyed, initialized, amfi_calls, sandbox_calls;
unsigned sandbox_destroyed, sandbox_references;
int sandbox_result;
bool sandbox_leaves_null;
label *expected_private_label;
bool raw_image, dynamic_policies;
constexpr unsigned CRF_MAC_ENFORCE = 1;
#define VFS_KERNEL_DEBUG_START1(...) ((void)0)
#define VFS_KERNEL_DEBUG_END1(...) ((void)0)
#define SECURITY_MAC_CHECK_ENFORCE 0
Credential *posix_cred_get(Credential *value) { return value; }
Credential *vfs_context_ucred(Context *context) { return context->credential; }
proc *vfs_context_proc(Context *context) { return &context->process; }
label *mac_cred_label(Credential *credential) { return &credential->policy; }
label *mac_vnode_label(vnode *) { return nullptr; }
void mac_label_set(label *value, int slot, uintptr_t content) { value->slots[slot] = content; }
uintptr_t mac_label_get(label *value, int slot) { return value->slots[slot]; }
bool switch_app_code_image(vnode *image) { return image->raw; }
bool switch_bootstrap_tool_image(vnode *image, off_t) { return image->bootstrap && image->tool && !image->raw; }
int mac_policy_list_conditional_busy() { return dynamic_policies; }
void mac_policy_list_unbusy() {}
void *exec_spawnattr_getmacpolicyinfo(void *, const char *, size_t *size) { *size = 0; return nullptr; }
int mac_error_select(int next, int previous) { return next ? next : previous; }
void destroy(label *) { ++destroyed; }
void destroy_sandbox(label *value)
{
    EXPECT_EQ(value, expected_private_label);
    EXPECT_EQ(value->slots[1], 222u);
    value->slots[1] = 0;
    --sandbox_references;
    ++sandbox_destroyed;
}
void initialize(label *value) { ++initialized; EXPECT_EQ(value->slots[0], 0u); }
int amfi(kauth_cred_t, kauth_cred_t next, proc *, vnode *, off_t, vnode *, label *, label *, label *,
         unsigned *flags, void *, size_t, int *)
{
    ++amfi_calls;
    if (raw_image) {
        EXPECT_EQ(next->policy.slots[0], 0u);
        EXPECT_EQ(*flags, SWITCH_APP_CODE_CS_FLAGS | CS_NO_UNTRUSTED_HELPERS);
        next->policy.slots[0] = 77;
        *flags |= CS_PLATFORM_BINARY;
    }
    return 0;
}
int sandbox(kauth_cred_t, kauth_cred_t next, proc *, vnode *, off_t, vnode *, label *, label *, label *,
            unsigned *flags, void *, size_t, int *)
{
    ++sandbox_calls;
    if (raw_image) {
        EXPECT_EQ(next->policy.slots[0], 77u);
        EXPECT_EQ(*flags, SWITCH_APP_CODE_CS_FLAGS);
    }
    EXPECT_EQ(next->policy.slots[1], 222u);
    if (sandbox_leaves_null && !sandbox_result) {
        next->policy.slots[1] = 0;
        --sandbox_references;
    }
    return sandbox_result;
}
int other_policy(kauth_cred_t, kauth_cred_t next, proc *, vnode *, off_t, vnode *, label *, label *, label *,
                 unsigned *, void *, size_t, int *disjoint)
{
    next->policy.slots[2] = 444;
    *disjoint = 0;
    return 0;
}
#include "app_code_cred_17.h"
#include "app_code_cred_27.h"
}

TEST(SwitchAppCodeCredential, RawExecReplacesParentIdentityAndPreservesSandbox)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        for (bool dynamic : {false, true}) {
            dynamic_policies = dynamic; raw_image = true; sandbox_leaves_null = false;
            destroyed = initialized = amfi_calls = sandbox_calls = 0;
            sandbox_result = 13;
            Credential old{{{111, 222}}}, next{{{111, 222}}}; Context context{&old, {}};
            vnode image{true, true}; unsigned flags = CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW;
            int amfi_slot = 0, sandbox_slot = 1, disjoint = 0, error = 0;
            Operations amfi_ops{amfi, destroy, initialize}, sandbox_ops{sandbox, nullptr, nullptr};
            mac_policy_conf policies[] = {{"AMFI", &amfi_ops, &amfi_slot}, {"Sandbox", &sandbox_ops, &sandbox_slot}};
            mac_policy_list = {dynamic ? 0u : 2u, 1, {{&policies[0]}, {&policies[1]}}};
            update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
            EXPECT_EQ(error, 13);
            EXPECT_EQ(destroyed, 1u); EXPECT_EQ(initialized, 1u);
            EXPECT_EQ(amfi_calls, 1u); EXPECT_EQ(sandbox_calls, 1u);
            EXPECT_EQ(flags, SWITCH_APP_CODE_CS_FLAGS);
            EXPECT_EQ(old.policy.slots[0], 111u);
            EXPECT_EQ(next.policy.slots[0], 77u);
        }
    }
}

TEST(SwitchAppCodeCredential, StockExecRetainsNativePolicyArguments)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        dynamic_policies = raw_image = sandbox_leaves_null = false; sandbox_result = 0;
        destroyed = initialized = 0;
        Credential old{{{111, 222}}}, next{{{111, 222}}}; Context context{&old, {}};
        vnode image{false}; unsigned flags = CS_PLATFORM_BINARY | CS_INSTALLER;
        int slot = 0, disjoint = 0, error = 0;
        Operations amfi_ops{amfi, destroy, initialize}, sandbox_ops{sandbox, nullptr, nullptr};
        mac_policy_conf policies[] = {{"AMFI", &amfi_ops, &slot}, {"Sandbox", &sandbox_ops, &slot}};
        mac_policy_list = {2, 1, {{&policies[0]}, {&policies[1]}}};
        update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
        EXPECT_EQ(error, 0); EXPECT_EQ(destroyed, 0u); EXPECT_EQ(initialized, 0u);
        EXPECT_EQ(flags, CS_PLATFORM_BINARY | CS_INSTALLER); EXPECT_EQ(next.policy.slots[0], 111u);
    }
}

TEST(SwitchBootstrapCredential, PackageExecCanOnlyPreserveAnAlreadyUnconfinedParent)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        for (bool dynamic : {false, true}) {
            for (bool parent_unconfined : {false, true}) {
                dynamic_policies = dynamic; raw_image = sandbox_leaves_null = false;
                sandbox_result = 0; sandbox_destroyed = 0; sandbox_references = 2;
                Credential old{{{111, parent_unconfined ? 0u : 222u}}}, next{{{111, 222}}};
                Context context{&old, {}}; expected_private_label = &next.policy;
                vnode image{false, true}; unsigned flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
                int slot = 1, other_slot = 2, disjoint = 0, error = 0;
                Operations ops{sandbox, destroy_sandbox, nullptr}, other_ops{other_policy, nullptr, nullptr};
                mac_policy_conf policies[] = {{"Sandbox", &ops, &slot}, {"Other", &other_ops, &other_slot}};
                mac_policy_list = {dynamic ? 0u : 2u, 1, {{&policies[0]}, {&policies[1]}, {}}};
                update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
                EXPECT_EQ(error, 0);
                EXPECT_EQ(sandbox_destroyed, parent_unconfined ? 1u : 0u);
                EXPECT_EQ(next.policy.slots[1], parent_unconfined ? 0u : 222u);
                EXPECT_EQ(next.policy.slots[2], 444u);
                EXPECT_EQ(disjoint, parent_unconfined ? 1 : 0);
            }
        }
    }
}

TEST(SwitchBootstrapCredential, NativeFailureAndMissingPolicyLifecycleNeverClearTheLabel)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        for (int denial : {1, 2, 3, 4}) {
            dynamic_policies = raw_image = sandbox_leaves_null = false;
            sandbox_result = denial == 1 ? 13 : 0; sandbox_destroyed = 0;
            Credential old{{{111, 0}}}, next{{{111, 222}}}; Context context{&old, {}};
            expected_private_label = &next.policy;
            vnode image{false, true}; unsigned flags = CS_VALID | CS_SIGNED;
            int slot = 1, disjoint = 0, error = 0;
            Operations ops{sandbox, denial == 2 ? nullptr : destroy_sandbox, nullptr};
            mac_policy_conf policy{denial == 4 ? "OtherSandbox" : "Sandbox", &ops, denial == 3 ? nullptr : &slot};
            mac_policy_list = {1, 0, {{&policy}, {}, {}}};
            update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
            EXPECT_EQ(error, sandbox_result);
            EXPECT_EQ(sandbox_destroyed, 0u);
            EXPECT_EQ(next.policy.slots[1], 222u);
            EXPECT_EQ(disjoint, 0);
        }
    }
}

TEST(SwitchBootstrapCredential, NativeSystemServicesAndPackageAppsRetainTheirSelectedSandbox)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        dynamic_policies = raw_image = sandbox_leaves_null = false;
        sandbox_result = 0; sandbox_destroyed = 0;
        Credential old{{{111, 0}}}, next{{{111, 222}}}; Context context{&old, {}};
        vnode image{false}; unsigned flags = CS_VALID | CS_SIGNED;
        int slot = 1, disjoint = 0, error = 0;
        Operations ops{sandbox, destroy_sandbox, nullptr}; mac_policy_conf policy{"Sandbox", &ops, &slot};
        mac_policy_list = {1, 0, {{&policy}, {}, {}}};
        update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
        EXPECT_EQ(error, 0); EXPECT_EQ(sandbox_destroyed, 0u);
        EXPECT_EQ(next.policy.slots[1], 222u); EXPECT_EQ(disjoint, 0);
        image = {false, true, false};
        update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
        EXPECT_EQ(error, 0); EXPECT_EQ(sandbox_destroyed, 0u);
        EXPECT_EQ(next.policy.slots[1], 222u); EXPECT_EQ(disjoint, 0);
    }
}

TEST(SwitchBootstrapCredential, ImportedAppCannotInheritBootstrapUnconfinedAuthority)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        dynamic_policies = false; raw_image = true; sandbox_leaves_null = false;
        sandbox_result = 0; sandbox_destroyed = 0;
        Credential old{{{111, 0}}}, next{{{111, 222}}}; Context context{&old, {}};
        vnode image{true, true}; unsigned flags = CS_VALID | CS_SIGNED;
        int amfi_slot = 0, sandbox_slot = 1, disjoint = 0, error = 0;
        Operations amfi_ops{amfi, destroy, initialize}, sandbox_ops{sandbox, destroy_sandbox, nullptr};
        mac_policy_conf policies[] = {{"AMFI", &amfi_ops, &amfi_slot}, {"Sandbox", &sandbox_ops, &sandbox_slot}};
        mac_policy_list = {2, 1, {{&policies[0]}, {&policies[1]}, {}}};
        update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
        EXPECT_EQ(error, 0); EXPECT_EQ(sandbox_destroyed, 0u);
        EXPECT_EQ(next.policy.slots[1], 222u); EXPECT_EQ(disjoint, 0);
    }
}

TEST(SwitchBootstrapCredential, NaturallyUnconfinedNativeUpdateAvoidsAnExtraReset)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        dynamic_policies = raw_image = false; sandbox_leaves_null = true;
        sandbox_result = 0; sandbox_destroyed = 0; sandbox_references = 2;
        Credential old{{{111, 222}}}, next = old; Context context{&old, {}};
        vnode image{false, true}; unsigned flags = CS_VALID | CS_SIGNED;
        int slot = 1, disjoint = 0, error = 0;
        Operations ops{sandbox, destroy_sandbox, nullptr}; mac_policy_conf policy{"Sandbox", &ops, &slot};
        mac_policy_list = {1, 0, {{&policy}, {}, {}}};
        update(&context, &next, &image, 0, nullptr, nullptr, nullptr, &flags, nullptr, &disjoint, &error);
        EXPECT_EQ(error, 0); EXPECT_EQ(sandbox_destroyed, 0u);
        EXPECT_EQ(next.policy.slots[1], 0u); EXPECT_EQ(disjoint, 0);
    }
}
