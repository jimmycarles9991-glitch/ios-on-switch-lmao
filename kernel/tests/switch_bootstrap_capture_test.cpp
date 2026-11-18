#include <gtest/gtest.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
#define SWITCH_IPSW_BUILD "TEST"
#define SWITCH_BOOTSTRAP_BASE "/private/var/Switch/bootstrap/" SWITCH_IPSW_BUILD
constexpr unsigned MNT_RDONLY = 1, MNT_IGNORE_OWNERSHIP = 2;
struct mount { unsigned flags = 0; };
struct vnode {
    mount *volume;
    bool root = true, supported = true;
    int attribute_error = 0, holds = 0;
    uint64_t id = 2;
    uint64_t size = 0;
    bool regular = false;
};
using vnode_t = vnode *;
constexpr vnode_t NULLVP = nullptr;
struct vnode_attr { uint64_t va_fileid, va_data_size; bool supported; };
#define VATTR_INIT(va) (*(va) = {})
#define VATTR_WANTED(...) ((void)0)
#define VATTR_IS_SUPPORTED(va, field) ((va)->supported)
mount *switch_bootstrap_data;
vnode_t data_vnode;
uint64_t switch_bootstrap_data_id;
int switch_bootstrap_recovery, switch_bootstrap_ready;
int data_lookup_error;
bool recovery_requested;
bool packaged, pending;
std::map<std::string, vnode_t> files;
std::vector<std::string> messages;
int printf(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    messages.emplace_back(line);
    return size;
}
bool PE_parse_boot_argn(const char *, uint32_t *value, size_t)
{
    *value = recovery_requested;
    return recovery_requested;
}
int switch_bootstrap_lookup_path(const char *path, vnode_t directory, vnode_t *out)
{
    EXPECT_EQ(directory, nullptr);
    if (data_lookup_error) return data_lookup_error;
    if (!strcmp(path, "/private/var")) *out = data_vnode;
    else if (files.count(path)) *out = files.at(path);
    else return ENOENT;
    ++(*out)->holds;
    return 0;
}
bool vnode_isvroot(vnode_t node) { return node->root; }
bool vnode_isreg(vnode_t node) { return node->regular; }
mount *vnode_mount(vnode_t node) { return node->volume; }
unsigned vfs_flags(mount *volume) { return volume->flags; }
int vfs_context_kernel() { return 0; }
int vnode_getattr(vnode_t node, vnode_attr *va, int)
{
    va->supported = node->supported;
    va->va_fileid = node->id;
    va->va_data_size = node->size;
    return node->attribute_error;
}
void vnode_put(vnode_t node) { --node->holds; }
void switch_migration_home_prepare(bool has_seed, bool has_pending)
{
    packaged = has_seed;
    pending = has_pending;
}
#define IOLog printf
#include "bootstrap_capture.h"
#undef IOLog

class BootstrapCapture : public testing::Test {
protected:
    mount data_mount;
    vnode data{&data_mount};
    void SetUp() override
    {
        data_vnode = &data;
        switch_bootstrap_data = nullptr;
        switch_bootstrap_ready = switch_bootstrap_recovery = data_lookup_error = 0;
        recovery_requested = false;
        packaged = pending = false;
        files.clear();
        messages.clear();
    }
    void reject(const char *step, int error)
    {
        switch_bootstrap_capture();
        EXPECT_EQ(switch_bootstrap_ready, 0);
        EXPECT_EQ(switch_bootstrap_data, nullptr);
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0], std::string("SwitchBootstrap: ") + step + " failed: " + std::to_string(error) + "\n");
        EXPECT_EQ(data.holds, 0);
        messages.clear();
    }
};
TEST_F(BootstrapCapture, MigrationHintsAreTypedBoundedFilesOnRealData)
{
    vnode manifest{&data_mount}, archive{&data_mount}, marker{&data_mount};
    manifest.regular = archive.regular = marker.regular = true;
    manifest.size = 32;
    archive.size = 4096;
    files = {
        {"/private/var/Switch/overlay/usr/share/Switch/bootstrap/manifest.json", &manifest},
        {"/private/var/Switch/overlay/usr/share/Switch/bootstrap/payload.tar", &archive},
        {SWITCH_BOOTSTRAP_BASE "/migration.pending", &marker},
    };
    switch_bootstrap_capture();
    EXPECT_TRUE(packaged);
    EXPECT_TRUE(pending);
    marker.size = 4097;
    archive.regular = false;
    switch_bootstrap_capture();
    EXPECT_FALSE(packaged);
    EXPECT_FALSE(pending);
    mount foreign;
    marker.size = 0;
    marker.volume = &foreign;
    archive.regular = true;
    manifest.size = 0;
    switch_bootstrap_capture();
    EXPECT_FALSE(packaged);
    EXPECT_FALSE(pending);
    EXPECT_EQ(marker.holds + manifest.holds + archive.holds, 0);
}

TEST_F(BootstrapCapture, ReportsEveryDataCaptureFailure)
{
    data_lookup_error = EIO;
    reject("Data lookup", EIO);
    data_lookup_error = 0;
    data.root = false;
    reject("Data volume root", EPERM);
    data.root = true;
    for (unsigned flag : {MNT_RDONLY, MNT_IGNORE_OWNERSHIP}) {
        data_mount.flags = flag;
        reject("Data writable ownership", EPERM);
    }
    data_mount.flags = 0;
    data.attribute_error = EIO;
    reject("Data file identity", EIO);
    data.attribute_error = 0;
    data.supported = false;
    reject("Data file identity", ENOTSUP);
}

TEST_F(BootstrapCapture, CapturesOnlyDataAndLeavesActivationToTheRootService)
{
    for (bool recovery : {false, true}) {
        recovery_requested = recovery;
        switch_bootstrap_capture();
        EXPECT_EQ(switch_bootstrap_data, &data_mount);
        EXPECT_EQ(switch_bootstrap_data_id, data.id);
        EXPECT_EQ(switch_bootstrap_ready, 0);
        EXPECT_EQ(switch_bootstrap_recovery, recovery);
        EXPECT_EQ(data.holds, 0);
#if DEVELOPMENT || DEBUG
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0], std::string("SwitchBootstrap: captured TEST Data volume; recovery ") +
            std::to_string(recovery) + "\n");
#else
        EXPECT_TRUE(messages.empty());
#endif
        messages.clear();
    }
}
}
