#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include "../switch/include/switch/bootstrap_rules.h"
#include "../switch/include/switch/bootstrap_policy_21H461.h"

namespace {
using user_addr_t = uintptr_t;
struct proc { bool enabled; };
bool switch_bootstrap_process(proc *p) { return p && p->enabled; }
int copyin(user_addr_t source, void *target, size_t size)
{
    if (!source) return EFAULT;
    std::memcpy(target, reinterpret_cast<void *>(source), size);
    return 0;
}
int copyout(const void *source, user_addr_t target, size_t size)
{
    if (!target) return EFAULT;
    std::memcpy(reinterpret_cast<void *>(target), source, size);
    return 0;
}
#include "bootstrap_dyld.h"

TEST(BootstrapDyld, UpdatesTheOutputPointerAndPreservesOtherPolicyBits)
{
    proc target{true};
    uint64_t flags = 0x1000;
    struct { uint64_t input; user_addr_t output; } request{0, reinterpret_cast<user_addr_t>(&flags)};
    ASSERT_EQ(switch_bootstrap_dyld_result(&target, "AMFI", 90, reinterpret_cast<user_addr_t>(&request), 0), 0);
    EXPECT_EQ(flags, 0x1043u);
    EXPECT_EQ(request.input, 0u);
    EXPECT_EQ(request.output, reinterpret_cast<user_addr_t>(&flags));
}

TEST(BootstrapDyld, CannotTurnAnAmfiFailureOrAnotherProcessIntoSuccess)
{
    proc enabled{true}, ordinary{false};
    uint64_t flags = 0;
    struct { uint64_t input; user_addr_t output; } request{0, reinterpret_cast<user_addr_t>(&flags)};
    auto argument = reinterpret_cast<user_addr_t>(&request);
    EXPECT_EQ(switch_bootstrap_dyld_result(&enabled, "AMFI", 90, argument, EACCES), EACCES);
    EXPECT_EQ(switch_bootstrap_dyld_result(&ordinary, "AMFI", 90, argument, 0), 0);
    EXPECT_EQ(switch_bootstrap_dyld_result(&enabled, "Sandbox", 90, argument, 0), 0);
    EXPECT_EQ(switch_bootstrap_dyld_result(&enabled, "AMFI", 91, argument, 0), 0);
    EXPECT_EQ(flags, 0u);
    EXPECT_EQ(switch_bootstrap_dyld_result(&enabled, "AMFI", 90, 0, 0), EFAULT);
    request.output = 0;
    EXPECT_EQ(switch_bootstrap_dyld_result(&enabled, "AMFI", 90, argument, 0), EFAULT);
}
}
