#include <gtest/gtest.h>
#include <cstddef>

namespace {
struct mount {
    mount *covered = nullptr;
};
using mount_t = mount *;
struct vnode {
    mount_t v_mount;
    mount_t translated = nullptr;
};
using vnode_t = vnode *;

mount_t native_owner_cryptex_mount[2];
mount_t native_owner_cryptex_host;

mount_t bindfs_boot_union_host(mount_t mp)
{
    return mp->covered;
}

mount_t switch_pretranslated_volume(vnode_t vp)
{
    return vp->translated;
}

#include "switch_vfs_identity.h"
}

TEST(SwitchVfsIdentity, ResolvesMountChainsAndPreservesOrdinaryFallback)
{
    mount system, preboot, os_cryptex, app_cryptex, translated;
    mount direct{&system}, nested{&direct}, cryptex_union{&os_cryptex};
    native_owner_cryptex_mount[0] = &os_cryptex;
    native_owner_cryptex_mount[1] = &app_cryptex;
    native_owner_cryptex_host = &preboot;

    EXPECT_EQ(switch_vfs_volume_host(nullptr), nullptr);
    EXPECT_EQ(switch_vfs_volume_host(&system), nullptr);
    EXPECT_EQ(switch_vfs_volume_host(&direct), &system);
    EXPECT_EQ(switch_vfs_volume_host(&nested), &system);
    EXPECT_EQ(switch_vfs_volume_host(&os_cryptex), &preboot);
    EXPECT_EQ(switch_vfs_volume_host(&app_cryptex), &preboot);
    EXPECT_EQ(switch_vfs_volume_host(&cryptex_union), &preboot);

    preboot.covered = &nested;
    EXPECT_EQ(switch_vfs_volume_host(&cryptex_union), &system);
    vnode overlaid{&cryptex_union, &translated};
    EXPECT_EQ(switch_vfs_statfs_mount(&overlaid), &system);

    vnode ordinary{&system}, pretranslated{&system, &translated}, unmounted{nullptr, &translated};
    EXPECT_EQ(switch_vfs_statfs_mount(&ordinary), &system);
    EXPECT_EQ(switch_vfs_statfs_mount(&pretranslated), &translated);
    EXPECT_EQ(switch_vfs_statfs_mount(&unmounted), nullptr);

    native_owner_cryptex_host = nullptr;
    EXPECT_EQ(switch_vfs_volume_host(&os_cryptex), nullptr);
}
