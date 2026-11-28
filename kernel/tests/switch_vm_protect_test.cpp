// <switch/vm_protect.h>: when vm_map_protect can skip clipping and unnesting a nested submap entry.
#include <gtest/gtest.h>

#include <cstdint>

typedef int vm_prot_t;

#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04
#define VM_PROT_UEXEC   0x08
#define VM_PROT_ALLEXEC (VM_PROT_EXECUTE | VM_PROT_UEXEC)

#include <switch/vm_protect.h>

namespace {

constexpr vm_prot_t kRX = VM_PROT_READ | VM_PROT_EXECUTE;
constexpr vm_prot_t kRW = VM_PROT_READ | VM_PROT_WRITE;
constexpr vm_prot_t kRWX = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;

TEST(SwitchVmProtect, SkipsOnlyNestedSubmapNoops)
{
    EXPECT_TRUE(switch_vm_protect_submap_noop(true, true, kRX, kRX, kRX, false));
    EXPECT_TRUE(switch_vm_protect_submap_noop(true, true, VM_PROT_READ, kRW, VM_PROT_READ, false));
    EXPECT_TRUE(switch_vm_protect_submap_noop(true, true, VM_PROT_READ, kRW, kRW, true));
    EXPECT_TRUE(switch_vm_protect_submap_noop(true, true, kRX, kRX, kRX, true));

    EXPECT_FALSE(switch_vm_protect_submap_noop(false, true, kRX, kRX, kRX, false));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, false, kRX, kRX, kRX, false));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, kRX, kRX, VM_PROT_READ, false));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, kRW, kRW, VM_PROT_READ, false));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, VM_PROT_READ, kRW, VM_PROT_READ, true));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, kRX, kRX, VM_PROT_READ, true));
}

TEST(SwitchVmProtect, NeverSkipsWriteExecuteRequests)
{
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, kRWX, kRWX, kRWX, false));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, kRWX, kRWX, kRWX, true));
    EXPECT_FALSE(switch_vm_protect_submap_noop(true, true, VM_PROT_WRITE | VM_PROT_UEXEC, VM_PROT_WRITE | VM_PROT_UEXEC,
                                               VM_PROT_WRITE | VM_PROT_UEXEC, false));
}

}  // namespace
