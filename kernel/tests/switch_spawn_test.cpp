// <switch/spawn.h>: posix_spawn's attribute block from iOS 27's libsystem_kernel read into the 12377 kernel's struct
// _posix_spawnattr, a block of the kernel's own size copied as before, any other size refused, and the hook in 12377's
// posix_spawn.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "spawnattr_12377.h"

typedef uint64_t user_addr_t;

static int copyin(user_addr_t uaddr, void *kaddr, size_t len)
{
    memcpy(kaddr, reinterpret_cast<const void *>(uaddr), len);
    return 0;
}

#include <switch/spawn.h>

using testing::HasSubstr;

namespace {

constexpr size_t kPorts = offsetof(struct _posix_spawnattr, psa_ports);

// Where 24B119's libsystem_kernel stores each field: posix_spawnattr_init mallocs 0x100 bytes and posix_spawn sends
// that as attr_size; set_use_sec_transition_shims_np and set_task_security_policy_np write a uint32_t at 0xac,
// set_use_sanitizers_np a byte at 0xb0, set_crash_count_np 0xb4/0xb8, set_kqworklooplimit_ext 0xbc/0xc0,
// set_conclavememlimit_ext 0xc4, setexceptionports_np's port actions go to 0xc8.
struct Darwin27Block {
    uint8_t bytes[0x100];

    template <typename T> void put(size_t offset, T value)
    {
        memcpy(bytes + offset, &value, sizeof(value));
    }
};

Darwin27Block LaunchdRelaunch()
{
    Darwin27Block block;
    memset(block.bytes, 0xa5, sizeof(block.bytes));
    block.put<uint16_t>(0x00, 0x4000);
    block.put<int32_t>(0x24, 0x300);
    block.put<int32_t>(0x44, 3);
    block.put<int32_t>(0x48, 300);
    block.put<int32_t>(0x4c, 50);
    block.put<uint64_t>(0x50, 4);
    block.put<int32_t>(0x58, 2);
    block.put<int32_t>(0x5c, 512);
    block.put<uint32_t>(0x80, 0x24);
    block.put<uint32_t>(0x84, 1000);
    block.put<uint32_t>(0x88, 2000);
    block.put<uint32_t>(0x94, 7);
    block.put<uint64_t>(0xa0, 99);
    block.put<uint8_t>(0xa8, 2);
    block.put<uint32_t>(0xac, 0x10000 | 0x201);
    block.put<uint8_t>(0xb0, 1);
    block.put<uint32_t>(0xb4, 3);
    block.put<uint32_t>(0xb8, 10);
    block.put<uint32_t>(0xbc, 4000);
    block.put<uint32_t>(0xc0, 8000);
    block.put<uint32_t>(0xc4, 64);
    return block;
}

std::string Text(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(SwitchSpawn, Darwin27FieldsLandInThisKernelsFields)
{
    Darwin27Block block = LaunchdRelaunch();
    struct _posix_spawnattr psa = {};

    ASSERT_EQ(switch_spawn_copyin_attr(reinterpret_cast<user_addr_t>(block.bytes), sizeof(block.bytes), &psa, kPorts),
              0);

    EXPECT_EQ(psa.psa_flags, 0x4000);
    EXPECT_EQ(psa.psa_apptype, 0x300);
    EXPECT_EQ(psa.psa_priority, 3);
    EXPECT_EQ(psa.psa_memlimit_active, 300);
    EXPECT_EQ(psa.psa_memlimit_inactive, 50);
    EXPECT_EQ(psa.psa_qos_clamp, 4u);
    EXPECT_EQ(psa.psa_darwin_role, 2);
    EXPECT_EQ(psa.psa_thread_limit, 512);
    EXPECT_EQ(psa.psa_options, 0x24u);
    EXPECT_EQ(psa.psa_port_soft_limit, 1000u);
    EXPECT_EQ(psa.psa_port_hard_limit, 2000u);
    EXPECT_EQ(psa.psa_crash_behavior, 7u);
    EXPECT_EQ(psa.psa_crash_behavior_deadline, 99u);
    EXPECT_EQ(psa.psa_launch_type, 2);
    EXPECT_EQ(psa.psa_sec_flags, 0x201);
    EXPECT_EQ(psa.psa_crash_count, 3u);
    EXPECT_EQ(psa.psa_throttle_timeout, 10u);
    EXPECT_EQ(psa.psa_kqworkloop_soft_limit, 4000u);
    EXPECT_EQ(psa.psa_kqworkloop_hard_limit, 8000u);
    EXPECT_EQ(psa.psa_conclave_mem_limit, 64u);
}

TEST(SwitchSpawn, NothingPastTheBlockBeforeItsPortsIsRead)
{
    Darwin27Block block = LaunchdRelaunch();
    memset(block.bytes + SWITCH_SPAWN_DARWIN27_PORTS, 0xee, sizeof(block.bytes) - SWITCH_SPAWN_DARWIN27_PORTS);
    struct _posix_spawnattr psa;
    memset(&psa, 0, sizeof(psa));

    ASSERT_EQ(switch_spawn_copyin_attr(reinterpret_cast<user_addr_t>(block.bytes), sizeof(block.bytes), &psa, kPorts),
              0);

    const uint8_t *tail = reinterpret_cast<const uint8_t *>(&psa) + kPorts;
    for (size_t i = 0; i < sizeof(psa) - kPorts; i++) {
        EXPECT_EQ(tail[i], 0) << i;
    }
}

TEST(SwitchSpawn, ThisKernelsOwnLayoutIsCopiedAsBefore)
{
    uint8_t own[sizeof(struct _posix_spawnattr)];
    for (size_t i = 0; i < sizeof(own); i++) {
        own[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    struct _posix_spawnattr psa = {};

    ASSERT_EQ(switch_spawn_copyin_attr(reinterpret_cast<user_addr_t>(own), sizeof(own), &psa, kPorts), 0);

    EXPECT_EQ(memcmp(&psa, own, kPorts), 0);
}

TEST(SwitchSpawn, AnUnknownLayoutIsRefused)
{
    uint8_t block[0x108] = {};
    struct _posix_spawnattr psa = {};

    EXPECT_EQ(switch_spawn_copyin_attr(reinterpret_cast<user_addr_t>(block), sizeof(block), &psa, kPorts), EINVAL);
    EXPECT_EQ(switch_spawn_copyin_attr(reinterpret_cast<user_addr_t>(block), 0xf0, &psa, kPorts), EINVAL);
}

TEST(SwitchSpawn, PosixSpawnReadsTheAttributesThroughTheHook)
{
    const std::string source = Text(KERN_EXEC_12377);
    EXPECT_THAT(source, HasSubstr("#include <switch/spawn.h>\n"));
    EXPECT_THAT(source,
                HasSubstr("switch_spawn_copyin_attr(px_args.attrp, px_args.attr_size, &px_sa, px_sa_offset)) != 0) {"));
    EXPECT_THAT(source, testing::Not(HasSubstr("copyin(px_args.attrp, &px_sa, px_sa_offset)")));
}

}  // namespace
