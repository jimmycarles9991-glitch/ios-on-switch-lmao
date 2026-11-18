// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>

namespace {
struct label { uintptr_t slots[2]{}; };
struct ucred { label mac; };
using kauth_cred_t = ucred *;
using vfs_context_t = ucred *;
struct vnode { bool package = false, imported = false; };
struct proc { bool tweaks = false, imported = false; };
using proc_t = proc *;
struct nameidata { int ni_cnd = 0; };
struct image_params {
    bool ip_switch_package_exec = false;
    int ip_px_smpx = 0;
    vnode *ip_scriptvp = nullptr;
    label *ip_scriptlabelp = nullptr, *ip_execlabelp = nullptr;
    nameidata *ip_ndp;
    unsigned ip_csflags = 0;
    vnode *ip_vp = nullptr;
};
struct fileglob { int type; vnode *data; };
constexpr int DTYPE_VNODE = 1;
#ifndef FREAD
constexpr int FREAD = 1;
#endif
constexpr int VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4;
using mpo_vnode_check_exec_t = int(kauth_cred_t, vnode *, vnode *, label *, label *, label *, int *, unsigned *, void *, size_t);
using MmapHook = int (*)(kauth_cred_t, fileglob *, label *, int, int, uint64_t, int *);
struct Ops { mpo_vnode_check_exec_t *mpo_vnode_check_exec; MmapHook mpo_file_check_mmap; };
struct mac_policy_conf { const char *mpc_name; const Ops *mpc_ops; int *mpc_field_off; };
struct PolicyEntry { mac_policy_conf *mpc; };
struct PolicyList { unsigned staticmax, maxindex; std::array<PolicyEntry, 7> entries; };
PolicyList mac_policy_list;
proc switch_current;
constexpr unsigned SWITCH_BOOTSTRAP_EXEC_CALLER = 1u, SWITCH_BOOTSTRAP_EXEC_LAUNCHER = 2u;
unsigned candidate;
bool native_resources, tweak_read, signed_mapping, dynamic_busy;
int busy_calls, unbusy_calls, validation_error, translated_error;
int sandbox_slot = 1;
std::array<int, 3> results, exec_calls, mmap_calls;

int mac_error_select(int next, int previous) { return next ? next : previous; }
kauth_cred_t vfs_context_ucred(vfs_context_t ctx) { return ctx; }
label *mac_cred_label(kauth_cred_t cred) { return &cred->mac; }
uintptr_t mac_label_get(label *mac, int slot) { return mac->slots[slot]; }
label *mac_vnode_label(vnode *) { return nullptr; }
proc_t current_proc() { return &switch_current; }
bool switch_app_code_image(vnode *vp) { return vp->imported; }
bool switch_bootstrap_image(vnode *vp) { return vp->package; }
bool switch_app_code_process(proc_t p) { return p->imported; }
bool switch_bootstrap_process(proc_t p) { return p->tweaks; }
bool switch_app_code_amfi(const char *name) { return !std::strcmp(name, "AMFI"); }
unsigned switch_bootstrap_exec_preflight(proc_t, image_params *image)
{
    return image->ip_vp ? candidate : 0;
}
bool switch_bootstrap_access(vnode *, int) { return tweak_read; }
bool switch_bootstrap_app_access(vnode *, int) { return native_resources; }
bool switch_bootstrap_exec_mapping(proc_t, vnode *, uint64_t, int) { return signed_mapping; }
int switch_bootstrap_validate(vnode *) { return validation_error; }
int switch_pretranslated_mmap(fileglob *, int, int, int) { return translated_error; }
int mac_policy_list_conditional_busy() { ++busy_calls; return dynamic_busy; }
void mac_policy_list_unbusy() { ++unbusy_calls; }
void *exec_spawnattr_getmacpolicyinfo(int *, const char *, size_t *size)
{
    *size = 0;
    return nullptr;
}
[[noreturn]] void panic(const char *) { throw std::runtime_error("invalid protection increase"); }

template <size_t Index>
int switch_exec_hook(kauth_cred_t, vnode *, vnode *, label *, label *, label *, int *, unsigned *, void *, size_t)
{
    ++exec_calls[Index];
    return results[Index];
}
template <size_t Index>
int switch_mmap_hook(kauth_cred_t, fileglob *, label *, int, int, uint64_t, int *)
{
    ++mmap_calls[Index];
    return results[Index];
}
const Ops sandbox_ops = {switch_exec_hook<0>, switch_mmap_hook<0>};
const Ops amfi_ops = {switch_exec_hook<1>, switch_mmap_hook<1>};
const Ops other_ops = {switch_exec_hook<2>, switch_mmap_hook<2>};
mac_policy_conf sandbox_policy{"Sandbox", &sandbox_ops, &sandbox_slot};
mac_policy_conf amfi_policy{"AMFI", &amfi_ops, nullptr};
mac_policy_conf other_policy{"Other", &other_ops, nullptr};

#define VFS_KERNEL_DEBUG_START1(code, vp) ((void)0)
#define VFS_KERNEL_DEBUG_END1(code, vp) ((void)0)
#define SECURITY_MAC_CHECK_ENFORCE 0
#define DTRACE_MACF3(...) ((void)0)
#define DTRACE_MACF2(...) ((void)0)
#define MAC_CHECK_CALL(...) ((void)0)
#define MAC_CHECK_RSLT(...) ((void)0)
#define FILEGLOB_DTYPE(fg) ((fg)->type)
vnode *fg_get_data(fileglob *fg) { return fg->data; }
#define MAC_POLICY_ITERATE(body) do { \
    const unsigned count = dynamic_busy ? mac_policy_list.maxindex + 1 : mac_policy_list.staticmax; \
    for (unsigned index = 0; index < count; ++index) { \
        const auto *mpc = mac_policy_list.entries[index].mpc; \
        if (!mpc) continue; \
        body \
    } \
} while (0)

#include "app_code_mac_17.h"
#define SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED_17
#include "bootstrap_apps_exec_mac_17.h"
#include "bootstrap_apps_mmap_mac_17.h"
#undef SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED

#include "app_code_mac_27.h"
#define SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED_27
#include "bootstrap_apps_exec_mac_27.h"
#include "bootstrap_apps_mmap_mac_27.h"
#undef SWITCH_BOOTSTRAP_MAC_CHECK_WITH_CRED

using ExecCheck = int (*)(vfs_context_t, vnode *, image_params *);
using MmapCheck = int (*)(kauth_cred_t, fileglob *, int, int, uint64_t, int *);
struct SwitchMacVersion { ExecCheck exec; MmapCheck mmap; };
const SwitchMacVersion versions[] = {
    {mac_vnode_check_exec_17, mac_file_check_mmap_17},
    {mac_vnode_check_exec_27, mac_file_check_mmap_27},
};

class SwitchBootstrapAppsMac : public testing::Test {
  protected:
    void reset(bool dynamic)
    {
        candidate = 0;
        native_resources = tweak_read = signed_mapping = false;
        dynamic_busy = dynamic;
        busy_calls = unbusy_calls = validation_error = translated_error = 0;
        switch_current = {};
        results = {EPERM, 0, 0};
        sandbox_policy.mpc_field_off = &sandbox_slot;
        cred.mac.slots[sandbox_slot] = 1;
        exec_calls = mmap_calls = {};
        mac_policy_list = {};
        mac_policy_list.staticmax = dynamic ? 1 : 4;
        mac_policy_list.maxindex = dynamic ? 4 : 3;
        const unsigned first = dynamic ? 2 : 1;
        mac_policy_list.entries[first].mpc = &sandbox_policy;
        mac_policy_list.entries[first + 1].mpc = &amfi_policy;
        mac_policy_list.entries[first + 2].mpc = &other_policy;
    }

    ucred cred{{{0, 1}}};
    vnode target{true, false};
    nameidata name;
    image_params image{false, 0, nullptr, nullptr, nullptr, &name, 0, &target};
    fileglob file{DTYPE_VNODE, &target};
};

TEST_F(SwitchBootstrapAppsMac, PackageExecOnlySkipsSandboxAndExistingAmfi)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            candidate = SWITCH_BOOTSTRAP_EXEC_LAUNCHER;
            EXPECT_EQ(version.exec(&cred, &target, &image), 0);
            EXPECT_TRUE(image.ip_switch_package_exec);
            EXPECT_EQ(exec_calls, (std::array<int, 3>{0, 0, 1}));
            EXPECT_EQ(unbusy_calls, dynamic ? 1 : 0);

            target.package = false;
            results[1] = EACCES;
            EXPECT_EQ(version.exec(&cred, &target, &image), EACCES);
            EXPECT_EQ(exec_calls[0], 0);
            EXPECT_EQ(exec_calls[1], 1);
            results[2] = ENOSPC;
            EXPECT_EQ(version.exec(&cred, &target, &image), ENOSPC);
            target.package = true;
        }
    }
}

TEST_F(SwitchBootstrapAppsMac, LatchedCandidateNeverSkipsSandboxForALaterInterpreterOrOrdinaryApp)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            image.ip_switch_package_exec = false;
            candidate = SWITCH_BOOTSTRAP_EXEC_LAUNCHER;
            ASSERT_EQ(version.exec(&cred, &target, &image), 0);
            ASSERT_TRUE(image.ip_switch_package_exec);
            candidate = false;
            image.ip_scriptvp = &target;
            EXPECT_EQ(version.exec(&cred, &target, &image), EPERM);
            EXPECT_TRUE(image.ip_switch_package_exec);
            EXPECT_EQ(exec_calls[0], 1);
            target.package = false;
            target.imported = true;
            EXPECT_EQ(version.exec(&cred, &target, &image), EPERM);
            EXPECT_EQ(exec_calls[0], 2);
            EXPECT_EQ(exec_calls[1], 0);
            target = {true, false};
            image.ip_scriptvp = nullptr;
        }
    }
}

TEST_F(SwitchBootstrapAppsMac, PackageCallersOnlySkipSandboxWhenTheRegisteredSlotIsEmpty)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            candidate = SWITCH_BOOTSTRAP_EXEC_CALLER;
            EXPECT_EQ(version.exec(&cred, &target, &image), EPERM);
            EXPECT_TRUE(image.ip_switch_package_exec);
            EXPECT_EQ(exec_calls, (std::array<int, 3>{1, 0, 1}));

            cred.mac.slots[sandbox_slot] = 0;
            EXPECT_EQ(version.exec(&cred, &target, &image), 0);
            EXPECT_EQ(exec_calls, (std::array<int, 3>{1, 0, 2}));

            sandbox_policy.mpc_field_off = nullptr;
            EXPECT_EQ(version.exec(&cred, &target, &image), EPERM);
            EXPECT_EQ(exec_calls, (std::array<int, 3>{2, 0, 3}));

            candidate = SWITCH_BOOTSTRAP_EXEC_LAUNCHER;
            EXPECT_EQ(version.exec(&cred, &target, &image), 0);
            EXPECT_EQ(exec_calls[0], 2);

            candidate = SWITCH_BOOTSTRAP_EXEC_CALLER;
            sandbox_policy.mpc_field_off = &sandbox_slot;
            results[2] = EIO;
            EXPECT_EQ(version.exec(&cred, &target, &image), EIO);
            EXPECT_EQ(exec_calls[0], 2);
        }
    }
}

TEST_F(SwitchBootstrapAppsMac, ReadOnlyNativeResourcesCapMaximumProtection)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            native_resources = true;
            int maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ, MAP_PRIVATE, 0, &maxprot), 0);
            EXPECT_EQ(maxprot, VM_PROT_READ);
            EXPECT_EQ(mmap_calls, (std::array<int, 3>{0, 1, 1}));
            results[2] = EIO;
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ, MAP_SHARED, 0, &maxprot), EIO);
            EXPECT_EQ(mmap_calls[0], 0);
        }
    }
}

TEST_F(SwitchBootstrapAppsMac, NativeReadEligibilityDoesNotAuthorizeWriteOrExecutableMappings)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            native_resources = true;
            for (int protection : {VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_EXECUTE}) {
                int maxprot = 7;
                EXPECT_EQ(version.mmap(&cred, &file, protection, MAP_PRIVATE, 0, &maxprot), EPERM);
                EXPECT_EQ(maxprot, 7);
            }
            EXPECT_EQ(mmap_calls[0], 3);
            native_resources = false;
            int maxprot = 7;
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ, MAP_PRIVATE, 0, &maxprot), EPERM);
            EXPECT_EQ(maxprot, 7);
        }
    }
}

TEST_F(SwitchBootstrapAppsMac, ExistingTweakMappingsRemainSeparateAndRevocationFailsBeforeDispatch)
{
    for (auto version : versions) {
        for (bool dynamic : {false, true}) {
            reset(dynamic);
            tweak_read = true;
            switch_current.tweaks = true;
            int maxprot = 7;
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ | VM_PROT_EXECUTE, MAP_PRIVATE, 0, &maxprot), 0);
            EXPECT_EQ(maxprot, 7);
            EXPECT_EQ(mmap_calls, (std::array<int, 3>{0, 0, 1}));
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ | VM_PROT_WRITE, MAP_SHARED, 0, &maxprot), EPERM);
            validation_error = EPERM;
            const auto previous = mmap_calls;
            EXPECT_EQ(version.mmap(&cred, &file, VM_PROT_READ, MAP_PRIVATE, 0, &maxprot), EPERM);
            EXPECT_EQ(mmap_calls, previous);
        }
    }
}
} // namespace
