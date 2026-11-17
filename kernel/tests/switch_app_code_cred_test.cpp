#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include "../xnu-12377.121.6/osfmk/kern/cs_blobs.h"
#include "../switch/include/switch/app_code_rules.h"
#define SWITCH_APP_CODE_CS_FLAGS (CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED)

namespace {
struct label { uintptr_t slots[2]; };
struct Credential { label policy; unsigned cr_flags = 0; };
using kauth_cred_t = Credential *;
using posix_cred_t = Credential *;
struct proc {};
struct vnode { bool raw; };
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
struct { unsigned staticmax, maxindex; Entry entries[2]; } mac_policy_list;
unsigned destroyed, initialized, amfi_calls, sandbox_calls;
int sandbox_result;
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
bool switch_app_code_image(vnode *image) { return image->raw; }
int mac_policy_list_conditional_busy() { return dynamic_policies; }
void mac_policy_list_unbusy() {}
void *exec_spawnattr_getmacpolicyinfo(void *, const char *, size_t *size) { *size = 0; return nullptr; }
int mac_error_select(int next, int previous) { return next ? next : previous; }
void destroy(label *) { ++destroyed; }
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
    return sandbox_result;
}
#include "app_code_cred_17.h"
#include "app_code_cred_27.h"
}

TEST(SwitchAppCodeCredential, RawExecReplacesParentIdentityAndPreservesSandbox)
{
    for (auto update : {mac_cred_label_update_execve_17, mac_cred_label_update_execve_27}) {
        for (bool dynamic : {false, true}) {
            dynamic_policies = dynamic; raw_image = true;
            destroyed = initialized = amfi_calls = sandbox_calls = 0;
            sandbox_result = 13;
            Credential old{{{111, 222}}}, next{{{111, 222}}}; Context context{&old, {}};
            vnode image{true}; unsigned flags = CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW;
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
        dynamic_policies = raw_image = false; sandbox_result = 0;
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
