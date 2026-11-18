#include <gtest/gtest.h>
#include <cstdint>

namespace {
using kern_return_t = int;
using vm_prot_t = int;
using pmap_t = int;
using vm_map_size_t = uint64_t;
using vm_map_offset_t = uint64_t;
using vm_object_fault_info_t = void *;
constexpr int KERN_SUCCESS = 0, KERN_CODESIGN_ERROR = 50;
constexpr int VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4;
constexpr pmap_t kernel_pmap = 0;
struct vnode { bool bootstrap = true; };
struct vm_object { bool internal = false, code_signed = true; vnode *pager; };
struct vm_page {
    uint8_t validated = 0, tainted = 0, nx = 0, matching = 0xf;
    bool vmp_wpmapped = false;
};
using vm_object_t = vm_object *;
using vm_page_t = vm_page *;
unsigned validation_calls;
uint8_t switch_test_mask(vm_map_size_t size, vm_map_offset_t offset)
{
    return size == 16384 ? 0xf : static_cast<uint8_t>(1u << (offset / 4096));
}
bool switch_test_all(uint8_t flags, vm_map_size_t size, vm_map_offset_t offset)
{
    const uint8_t mask = switch_test_mask(size, offset);
    return (flags & mask) == mask;
}
#define VMP_CS_VALIDATED(page, size, offset) switch_test_all((page)->validated, size, offset)
#define VMP_CS_TAINTED(page, size, offset) (((page)->tainted & switch_test_mask(size, offset)) != 0)
#define VMP_CS_NX(page, size, offset) (((page)->nx & switch_test_mask(size, offset)) != 0)
vnode *vnode_pager_lookup_vnode(vnode *pager) { return pager; }
bool switch_bootstrap_signed(vnode *vp) { return vp->bootstrap; }
void vm_object_lock_assert_exclusive(vm_object_t) {}
bool vm_fault_cs_need_validation(pmap_t, vm_page_t page, vm_object_t, vm_map_size_t size, vm_map_offset_t offset)
{
    return !VMP_CS_TAINTED(page, size, offset) &&
        (!VMP_CS_VALIDATED(page, size, offset) || page->vmp_wpmapped);
}
void vm_page_validate_cs(vm_page_t page, vm_map_size_t size, vm_map_offset_t offset)
{
    ++validation_calls;
    const uint8_t mask = switch_test_mask(size, offset);
    page->validated |= mask;
    page->tainted |= mask & ~page->matching;
}
#include "bootstrap_fault_17.h"
#include "bootstrap_fault_27.h"

class BootstrapFault : public testing::Test {
protected:
    vnode file;
    vm_object object{false, true, &file};
    vm_page page;
    void SetUp() override { validation_calls = 0; }
};

TEST_F(BootstrapFault, OriginalFilePagesFailHashValidationDespiteDebugOrBypassPolicy)
{
    for (auto check : {vm_fault_cs_check_violation_17, vm_fault_cs_check_violation_27}) {
        for (bool bypass : {false, true}) {
            page = {};
            page.matching = 0;
            validation_calls = 0;
            EXPECT_EQ(check(bypass, &object, &page, 1, VM_PROT_READ | VM_PROT_EXECUTE,
                VM_PROT_EXECUTE, 16384, 0, nullptr, false, false, nullptr), KERN_CODESIGN_ERROR);
            EXPECT_EQ(validation_calls, 1u);
        }
    }
}

TEST_F(BootstrapFault, ValidOriginalCodeIsHashedOnceAndCanExecute)
{
    for (auto check : {vm_fault_cs_check_violation_17, vm_fault_cs_check_violation_27}) {
        page = {};
        validation_calls = 0;
        for (unsigned repeat = 0; repeat < 2; ++repeat)
            EXPECT_EQ(check(true, &object, &page, 1, VM_PROT_EXECUTE, VM_PROT_EXECUTE,
                16384, 0, nullptr, false, false, nullptr), KERN_SUCCESS);
        EXPECT_EQ(validation_calls, 1u);
    }
}

TEST_F(BootstrapFault, TaintedNonExecutableAndWritableOriginalPagesAreRejected)
{
    for (auto check : {switch_bootstrap_fault_page_17, switch_bootstrap_fault_page_27}) {
        page.validated = 0xf;
        page.tainted = 1;
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 16384, 0), KERN_CODESIGN_ERROR);
        page.tainted = 0;
        page.nx = 1;
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 16384, 0), KERN_CODESIGN_ERROR);
        page.nx = 0;
        page.vmp_wpmapped = true;
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 16384, 0), KERN_CODESIGN_ERROR);
        page.vmp_wpmapped = false;
    }
}

TEST_F(BootstrapFault, CowTextPatchesAndAnonymousRwToRxTrampolinesKeepNativePolicy)
{
    for (auto check : {vm_fault_cs_check_violation_17, vm_fault_cs_check_violation_27}) {
        page.tainted = 0xf;
        object.internal = true;
        EXPECT_EQ(check(true, &object, &page, 1, VM_PROT_EXECUTE, VM_PROT_EXECUTE,
            16384, 0, nullptr, false, false, nullptr), KERN_SUCCESS);
        object.pager = nullptr;
        object.code_signed = false;
        EXPECT_EQ(check(true, &object, &page, 1, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_WRITE,
            16384, 0, nullptr, false, false, nullptr), KERN_SUCCESS);
        EXPECT_EQ(check(true, &object, &page, 1, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_EXECUTE,
            16384, 0, nullptr, false, false, nullptr), KERN_SUCCESS);
    }
    EXPECT_EQ(validation_calls, 0u);
}

TEST_F(BootstrapFault, ChecksOnlyTheRequestedCodeSubpage)
{
    for (auto check : {switch_bootstrap_fault_page_17, switch_bootstrap_fault_page_27}) {
        page = {};
        page.matching = 0xd;
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 4096, 0), KERN_SUCCESS);
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 4096, 4096), KERN_CODESIGN_ERROR);
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 4096, 8192), KERN_SUCCESS);
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 16384, 0), KERN_CODESIGN_ERROR);
    }
}

TEST_F(BootstrapFault, OrdinaryFilesAndDataReadsRetainExistingPolicy)
{
    page.matching = 0;
    for (auto check : {switch_bootstrap_fault_page_17, switch_bootstrap_fault_page_27}) {
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_READ, 16384, 0), KERN_SUCCESS);
        EXPECT_EQ(check(&object, &page, kernel_pmap, VM_PROT_EXECUTE, 16384, 0), KERN_SUCCESS);
        file.bootstrap = false;
        EXPECT_EQ(check(&object, &page, 1, VM_PROT_EXECUTE, 16384, 0), KERN_SUCCESS);
        file.bootstrap = true;
    }
    EXPECT_EQ(validation_calls, 0u);
}
}
