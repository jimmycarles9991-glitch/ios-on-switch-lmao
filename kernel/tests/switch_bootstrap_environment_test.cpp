#include <gtest/gtest.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../switch/include/switch/bootstrap_rules.h"

namespace {
using user_addr_t = uintptr_t;
constexpr int Z_WAITOK = 1, Z_NOFAIL = 2, UIO_SYSSPACE = 0;
constexpr int VSUID = 04000, VSGID = 02000;
constexpr bool TRUE = true;
bool fail_kalloc_data = false;
char *kalloc_data(size_t size, int flags)
{
    EXPECT_FALSE((flags & Z_NOFAIL) && size > 16384u);
    if (fail_kalloc_data || ((flags & Z_NOFAIL) && size > 16384u)) return nullptr;
    return static_cast<char *>(std::malloc(size));
}
void kfree_data(void *value, size_t) { std::free(value); }
struct vnode_attr {
    unsigned va_mode;
};
struct image_params {
    void *ip_vp = nullptr;
    char *ip_endargv, *ip_strendp;
    int ip_strspace, ip_argspace;
    unsigned ip_envc;
    bool ip_switch_tweaks;
    vnode_attr *ip_vattr = nullptr;
    vnode_attr *ip_origvattr = nullptr;
};
bool bootstrap_ready;
bool switch_bootstrap_target(void *vp) { return vp && bootstrap_ready; }
int exec_add_user_string(image_params *image, user_addr_t value, int, bool)
{
    const auto *text = reinterpret_cast<const char *>(value);
    const size_t length = std::strlen(text) + 1;
    if (length > static_cast<size_t>(image->ip_argspace) || length > static_cast<size_t>(image->ip_strspace)) return E2BIG;
    std::memcpy(image->ip_strendp, text, length);
    image->ip_strendp += length;
    image->ip_strspace -= length;
    image->ip_argspace -= length;
    return 0;
}
#include "bootstrap_environment_17.h"
#include "bootstrap_environment_27.h"

TEST(BootstrapEnvironment, MergesDuplicateVariablesAndKeepsCallerLibraries)
{
    for (auto append : {switch_bootstrap_environment_17, switch_bootstrap_environment_27}) {
        std::array<char, 2048> storage{};
        image_params image{nullptr, storage.data(), storage.data(), storage.size(), storage.size(), 0, true};
        for (const auto *entry : {"A=one", "DYLD_INSERT_LIBRARIES=/caller/a.dylib", "B=two",
                                  "DYLD_INSERT_LIBRARIES=/caller/b.dylib"}) {
            ASSERT_EQ(exec_add_user_string(&image, CAST_USER_ADDR_T(entry), 0, true), 0);
            image.ip_argspace -= sizeof(void *);
            ++image.ip_envc;
        }
        ASSERT_EQ(append(&image, sizeof(void *)), 0);
        std::vector<std::string> values;
        for (const char *entry = storage.data(); entry < image.ip_strendp; entry += std::strlen(entry) + 1)
            values.emplace_back(entry);
        EXPECT_EQ(image.ip_envc, 3u);
        EXPECT_EQ(values, (std::vector<std::string>{"A=one", "B=two",
            "DYLD_INSERT_LIBRARIES=/caller/a.dylib:/caller/b.dylib:" SWITCH_BOOTSTRAP_INJECTOR}));
        EXPECT_EQ(image.ip_argspace, storage.size() - static_cast<size_t>(image.ip_strendp - storage.data()) -
                  image.ip_envc * sizeof(void *));
    }
}

TEST(BootstrapEnvironment, DoesNotDuplicateAnExistingInjector)
{
    for (auto append : {switch_bootstrap_environment_17, switch_bootstrap_environment_27}) {
        std::array<char, 2048> storage{};
        image_params image{nullptr, storage.data(), storage.data(), storage.size(), storage.size(), 1, true};
        const char *entry = "DYLD_INSERT_LIBRARIES=" SWITCH_BOOTSTRAP_INJECTOR;
        ASSERT_EQ(exec_add_user_string(&image, CAST_USER_ADDR_T(entry), 0, true), 0);
        image.ip_argspace -= sizeof(void *);
        ASSERT_EQ(append(&image, sizeof(void *)), 0);
        EXPECT_STREQ(storage.data(), entry);
        EXPECT_EQ(image.ip_envc, 1u);
    }
}

TEST(BootstrapEnvironment, UsesReadinessAtImageActivation)
{
    auto check = [](auto snapshot, auto append) {
        for (const bool ready : {false, true}) {
            std::array<char, 2048> storage{};
            image_params image{storage.data(), storage.data(), storage.data(), storage.size(), storage.size(), 0, false};
            vnode_attr current{0755}, original{0755};
            image.ip_vattr = &current;
            image.ip_origvattr = &original;
            const char *entry = "DYLD_INSERT_LIBRARIES=/caller/a.dylib";
            ASSERT_EQ(exec_add_user_string(&image, CAST_USER_ADDR_T(entry), 0, true), 0);
            image.ip_argspace -= sizeof(void *);
            ++image.ip_envc;
            bootstrap_ready = ready;
            snapshot(&image);
            bootstrap_ready = !ready;
            ASSERT_EQ(append(&image, sizeof(void *)), 0);
            EXPECT_EQ(image.ip_envc, 1u);
            EXPECT_EQ(image.ip_switch_tweaks, ready);
            EXPECT_STREQ(storage.data(), ready ? "DYLD_INSERT_LIBRARIES=/caller/a.dylib:" SWITCH_BOOTSTRAP_INJECTOR
                                               : entry);
        }
    };
    check(switch_bootstrap_snapshot_17, switch_bootstrap_environment_17);
    check(switch_bootstrap_snapshot_27, switch_bootstrap_environment_27);
}

TEST(BootstrapEnvironment, KeepsSetIdImagesUnderNativeExecPolicy)
{
    bootstrap_ready = true;
    for (auto snapshot : {switch_bootstrap_snapshot_17, switch_bootstrap_snapshot_27}) {
        for (const unsigned mode : {0755 | VSUID, 0755 | VSGID}) {
            vnode_attr current{mode}, original{0755};
            image_params image{};
            image.ip_vp = &current;
            image.ip_vattr = &current;
            image.ip_origvattr = &original;
            image.ip_switch_tweaks = true;
            snapshot(&image);
            EXPECT_FALSE(image.ip_switch_tweaks);
            current.va_mode = 0755;
            original.va_mode = mode;
            image.ip_switch_tweaks = true;
            snapshot(&image);
            EXPECT_FALSE(image.ip_switch_tweaks);
        }
    }
}

TEST(BootstrapEnvironment, HandlesLargeEnvironmentAndAllocationFailure)
{
    for (auto append : {switch_bootstrap_environment_17, switch_bootstrap_environment_27}) {
        std::vector<char> storage(64 * 1024, '\0');
        image_params image{nullptr, storage.data(), storage.data(), static_cast<int>(storage.size()),
                           static_cast<int>(storage.size()), 0, true};
        std::string large = "BIG=" + std::string(40 * 1024, 'x');
        ASSERT_EQ(exec_add_user_string(&image, CAST_USER_ADDR_T(large.c_str()), 0, true), 0);
        image.ip_argspace -= sizeof(void *);
        ++image.ip_envc;
        ASSERT_EQ(append(&image, sizeof(void *)), 0);
        EXPECT_EQ(image.ip_envc, 2u);

        fail_kalloc_data = true;
        EXPECT_EQ(append(&image, sizeof(void *)), ENOMEM);
        fail_kalloc_data = false;
    }
}
}
