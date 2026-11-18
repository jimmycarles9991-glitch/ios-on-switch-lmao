#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include "../switch/include/switch/bootstrap_rules.h"

namespace {
struct vnode;
struct Mount { std::vector<vnode *> nodes; };
struct vnode {
    Mount *mount;
    uint64_t id, parent;
    uint32_t uid = 0, gid = 0, mode = 0755;
    uint64_t links = 1;
    bool directory = true, acl = false, supported = true;
    int references = 0;
    const char *unsupported = "";
    const char *path = "";
    bool symlink = false;
    const char *target = "";
    int read_error = 0;
};
using vnode_t = vnode *;
using ino64_t = uint64_t;
using user_addr_t = uintptr_t;
constexpr vnode_t NULLVP = nullptr;
constexpr unsigned MAXPATHLEN = 1024;
struct vnode_attr {
    uint32_t va_uid, va_gid, va_mode;
    uint64_t va_nlink, va_fileid, va_parentid;
    void *va_acl;
    bool supported;
    const char *unsupported;
};
#define VATTR_INIT(value) (*(value) = {})
#define VATTR_WANTED(...) ((void)0)
#define VATTR_IS_SUPPORTED(value, field) ((value)->supported && std::strcmp((value)->unsupported, #field))
int vfs_context_kernel() { return 0; }
int vnode_getattr(vnode_t vp, vnode_attr *va, int)
{
    *va = {vp->uid, vp->gid, vp->mode, vp->links, vp->id, vp->parent,
           vp->acl ? vp : nullptr, vp->supported, vp->unsupported};
    return 0;
}
bool vnode_isdir(vnode_t vp) { return vp->directory; }
bool vnode_isreg(vnode_t vp) { return !vp->directory && !vp->symlink; }
bool vnode_islnk(vnode_t vp) { return vp->symlink; }
bool vnode_isnamedstream(vnode_t) { return false; }
Mount *vnode_mount(vnode_t vp) { return vp->mount; }
int vnode_get(vnode_t vp) { ++vp->references; return 0; }
void vnode_put(vnode_t vp) { --vp->references; }
void kauth_acl_free(void *) {}
int VFS_VGET(Mount *mount, ino64_t id, vnode_t *out, int)
{
    for (auto *node : mount->nodes) {
        if (node->id == id) { *out = node; return vnode_get(node); }
    }
    return ENOENT;
}
int switch_bootstrap_lock;
void lck_mtx_lock(int *) {}
void lck_mtx_unlock(int *) {}
vnode_t switch_bootstrap_root;
Mount *switch_bootstrap_data;
uint64_t switch_bootstrap_root_id, switch_bootstrap_data_id;
#include "bootstrap_membership.h"

constexpr unsigned CS_VALID = 1, CS_SIGNED = 0x20000000, CS_ENTITLEMENTS_VALIDATED = 0x4000;
constexpr unsigned CS_PLATFORM_BINARY = 0x4000000;
struct cs_blob { unsigned csb_reconstituted; const char *identity; };
struct proc {
    unsigned int p_lflag;
    int pid;
    vnode_t p_textvp;
    bool platform = true;
    bool persona = true;
    unsigned flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    cs_blob *blob = nullptr;
};
using proc_t = proc *;
void proc_lock(proc_t) {}
void proc_unlock(proc_t) {}
int proc_getpid(proc_t p) { return p->pid; }
int switch_bootstrap_ready;
#define os_atomic_store(value, state, order) (*(value) = (state))
bool csproc_get_platform_binary(proc_t p) { return p && p->platform; }
bool switch_bootstrap_named(vnode_t vp, const char *path) { return vp && !std::strcmp(vp->path, path); }
#include "bootstrap_process.h"

#define SWITCH_BOOTSTRAP_ROOT "/private/var/Switch/bootstrap/TEST/root"
#define SWITCH_BOOTSTRAP_HELPER "/usr/libexec/switch-bootstrap"
#ifndef FREAD
constexpr int FREAD = 1, FWRITE = 2;
#endif
constexpr int ZV_NAMEI = 0, UIO_SYSSPACE = 0, UIO_READ = 0;
char *zalloc(int) { return static_cast<char *>(std::malloc(MAXPATHLEN)); }
void zfree(int, void *value) { std::free(value); }
int vn_getpath(vnode_t vp, char *path, int *length)
{
    const size_t size = std::strlen(vp->path) + 1;
    if (size > static_cast<size_t>(*length)) return ENAMETOOLONG;
    std::memcpy(path, vp->path, size);
    *length = static_cast<int>(size);
    return 0;
}
struct uio { char *buffer = nullptr; size_t remaining = 0; };
using uio_t = uio *;
#define UIO_STACKBUF(name, count) uio name[1]
uio_t uio_createwithbuffer(int, int, int, int, void *buffer, size_t) { return static_cast<uio_t>(buffer); }
int uio_addiov(uio_t io, uint64_t target, size_t size)
{
    io->buffer = reinterpret_cast<char *>(target);
    io->remaining = size;
    return 0;
}
size_t uio_resid(uio_t io) { return io->remaining; }
int VNOP_READLINK(vnode_t vp, uio_t io, int)
{
    if (!vp->symlink) return EINVAL;
    if (vp->read_error) return vp->read_error;
    const size_t count = std::min(io->remaining, std::strlen(vp->target));
    std::memcpy(io->buffer, vp->target, count);
    io->remaining -= count;
    return 0;
}
proc current{SWITCH_P_LTWEAKS, 42, nullptr};
proc_t current_proc() { return &current; }
#include "bootstrap_access.h"
unsigned proc_getcsflags(proc_t p) { return p->flags; }
proc_t proc_task(proc_t p) { return p; }
bool IOTaskHasEntitlement(proc_t p, const char *name)
{
    return p->persona && !std::strcmp(name, "com.apple.private.persona-mgmt");
}
cs_blob *csproc_get_blob(proc_t p) { return p->blob; }
const char *csblob_get_identity(cs_blob *blob) { return blob->identity; }
#include "bootstrap_persona_caller.h"

class BootstrapTree : public testing::Test {
protected:
    Mount mount, other;
    vnode data{&mount, 1, 1}, ancestor{&mount, 2, 1}, root{&mount, 3, 2};
    vnode directory{&mount, 4, 3}, image{&mount, 5, 4};
    vnode alias{&mount, 6, 1};
    void SetUp() override
    {
        image.directory = false;
        mount.nodes = {&data, &ancestor, &root, &directory, &image};
        switch_bootstrap_root = &root;
        switch_bootstrap_data = &mount;
        switch_bootstrap_root_id = root.id;
        switch_bootstrap_data_id = data.id;
        current.p_lflag = SWITCH_P_LTWEAKS;
        current.p_textvp = nullptr;
        current.platform = true;
        current.persona = true;
        current.flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
        current.blob = nullptr;
        data.path = "/private/var";
        root.path = SWITCH_BOOTSTRAP_ROOT;
    }
    void TearDown() override
    {
        for (auto *node : mount.nodes) EXPECT_EQ(node->references, 0);
    }
};

TEST_F(BootstrapTree, CapturedRootAndAllAncestorsMustBeOwnedAndNonWritable)
{
    ASSERT_TRUE(switch_bootstrap_member(&image, false));
    for (auto *node : mount.nodes) {
        node->uid = 501;
        EXPECT_FALSE(switch_bootstrap_member(&image, false));
        node->uid = 0;
        node->mode |= 0020;
        EXPECT_FALSE(switch_bootstrap_member(&image, false));
        node->mode &= ~0020u;
        node->acl = true;
        EXPECT_FALSE(switch_bootstrap_member(&image, false));
        node->acl = false;
    }
}

TEST_F(BootstrapTree, RejectsHardlinksMovedFilesForeignMountsAndStaleRoot)
{
    image.links = 2;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
    image.links = 1;
    image.parent = ancestor.id;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
    image.parent = directory.id;
    image.mount = &other;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
    image.mount = &mount;
    ++switch_bootstrap_root_id;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
}

TEST_F(BootstrapTree, MissingParentAndAttributeFailuresDenyAdmission)
{
    directory.parent = 99;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
    directory.parent = root.id;
    image.supported = false;
    EXPECT_FALSE(switch_bootstrap_member(&image, false));
    image.supported = true;
    EXPECT_FALSE(switch_bootstrap_member(&image, true));
    EXPECT_TRUE(switch_bootstrap_member(&root, true));
}

TEST_F(BootstrapTree, MissingTrustMetadataCannotDefaultToRootOwnership)
{
    for (const char *field : {"va_uid", "va_gid", "va_mode", "va_nlink", "va_fileid", "va_parentid"}) {
        image.unsupported = field;
        EXPECT_FALSE(switch_bootstrap_member(&image, false)) << field;
        image.unsupported = "";
        ancestor.unsupported = field;
        EXPECT_FALSE(switch_bootstrap_member(&image, false)) << field;
        ancestor.unsupported = "";
    }
}


TEST_F(BootstrapTree, OwnedSymlinksAreReadableWithoutAcquiringCodeAdmission)
{
    image.symlink = true;
    image.mode = 0777;
    EXPECT_TRUE(switch_bootstrap_member(&image, false));
    EXPECT_TRUE(switch_bootstrap_access(&image, FREAD));
    EXPECT_FALSE(switch_bootstrap_image(&image));
    EXPECT_FALSE(switch_bootstrap_access(&image, FWRITE));
    EXPECT_FALSE(switch_bootstrap_access(&image, FREAD | O_TRUNC));
    image.uid = 501;
    EXPECT_FALSE(switch_bootstrap_access(&image, FREAD));
    image.uid = 0;
    image.links = 2;
    EXPECT_FALSE(switch_bootstrap_access(&image, FREAD));
    image.links = 1;
    ancestor.mode |= 0020;
    EXPECT_FALSE(switch_bootstrap_access(&image, FREAD));
}

TEST_F(BootstrapTree, OutsideAliasRequiresExactTargetRootOwnershipAndSingleLink)
{
    mount.nodes.push_back(&alias);
    alias.directory = false;
    alias.symlink = true;
    alias.mode = 0777;
    alias.path = "/private/var/jb";
    alias.target = SWITCH_BOOTSTRAP_ROOT;
    ASSERT_TRUE(switch_bootstrap_alias(&alias));
    EXPECT_TRUE(switch_bootstrap_access(&alias, FREAD));
    EXPECT_FALSE(switch_bootstrap_image(&alias));
    EXPECT_FALSE(switch_bootstrap_access(&alias, FWRITE));
    alias.target = SWITCH_BOOTSTRAP_ROOT "/outside";
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.target = "/private/var/Switch/bootstrap/OTHER/root";
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.target = SWITCH_BOOTSTRAP_ROOT;
    alias.uid = 501;
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.uid = 0;
    alias.links = 2;
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.links = 1;
    alias.read_error = EIO;
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.read_error = 0;
    alias.path = "/private/var/other";
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
    alias.path = "/private/var/jb";
    alias.parent = root.id;
    EXPECT_FALSE(switch_bootstrap_alias(&alias));
}

TEST_F(BootstrapTree, LookupGrantsOnlyManagedReadTraversalAndRejectsLexicalEscapes)
{
    EXPECT_TRUE(switch_bootstrap_lookup(&data, "jb", 2));
    EXPECT_FALSE(switch_bootstrap_lookup(&data, "mobile", 6));
    EXPECT_FALSE(switch_bootstrap_lookup(&root, "..", 2));
    EXPECT_TRUE(switch_bootstrap_preflight(&data, "/var/jb/usr/lib/library.dylib", 0));
    EXPECT_TRUE(switch_bootstrap_preflight(&data, "jb/usr/lib/library.dylib", 0));
    EXPECT_TRUE(switch_bootstrap_preflight(&root, "usr/lib/library.dylib", 0));
    EXPECT_FALSE(switch_bootstrap_preflight(&data, SWITCH_BOOTSTRAP_HELPER, 0));
    EXPECT_FALSE(switch_bootstrap_preflight(&data, "/var/jb-other/secret", 0));
    EXPECT_FALSE(switch_bootstrap_preflight(&data, "/var/jb/../mobile/secret", 0));
    EXPECT_FALSE(switch_bootstrap_preflight(&data, "/etc/passwd", 0));
    EXPECT_FALSE(switch_bootstrap_preflight(&root, "../outside", 0));
    ancestor.path = "/private/var/Switch/bootstrap/TEST";
    EXPECT_TRUE(switch_bootstrap_lookup(&ancestor, "root", 4));
    EXPECT_FALSE(switch_bootstrap_lookup(&ancestor, "switch-bootstrap", 16));
    current.p_lflag = 0;
    EXPECT_FALSE(switch_bootstrap_lookup(&data, "jb", 2));
    EXPECT_FALSE(switch_bootstrap_preflight(&data, "/var/jb/usr/lib/library.dylib", 0));
}


TEST_F(BootstrapTree, RootPersonaRequiresAdmittedSileoIdentityAndCurrentTrustMetadata)
{
    cs_blob blob{SWITCH_BOOTSTRAP_SIGNATURE, SWITCH_BOOTSTRAP_SILEO_IDENTITY};
    current.blob = &blob;
    current.p_textvp = &image;
    image.path = SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo";
    ASSERT_TRUE(switch_bootstrap_persona_caller(&current));
    for (unsigned flag : {CS_VALID, CS_SIGNED, CS_ENTITLEMENTS_VALIDATED}) {
        current.flags &= ~flag;
        EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
        current.flags |= flag;
    }
    current.persona = false;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    current.persona = true;
    current.platform = false;
    current.flags |= CS_PLATFORM_BINARY;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    current.flags &= ~CS_PLATFORM_BINARY;
    current.platform = true;
    for (unsigned provenance : {0u, 1u, 2u}) {
        blob.csb_reconstituted = provenance;
        EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    }
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    blob.identity = "other.identity";
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    blob.identity = SWITCH_BOOTSTRAP_SILEO_IDENTITY;
    image.path = "/Applications/Sileo.app/Sileo";
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    image.path = SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo";
    image.uid = 501;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    image.uid = 0;
    image.links = 2;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    image.links = 1;
    ancestor.mode |= 0020;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    EXPECT_FALSE(switch_bootstrap_persona_caller(nullptr));
}


TEST_F(BootstrapTree, OrdinaryImportedAppCannotRequestBootstrapRootPersona)
{
    cs_blob blob{2, SWITCH_BOOTSTRAP_SILEO_IDENTITY};
    current.blob = &blob;
    current.p_textvp = &image;
    image.path = "/private/var/containers/Bundle/Application/UUID/Sileo.app/Sileo";
    current.platform = false;
    current.flags |= CS_PLATFORM_BINARY;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    current.platform = true;
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
    image.path = SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo";
    EXPECT_FALSE(switch_bootstrap_persona_caller(&current));
}

TEST(BootstrapPolicy, PackageToolsAreSeparateFromAppBundles)
{
    for (const auto *path : {SWITCH_BOOTSTRAP_ROOT "/usr/bin/dpkg", "/private/var/jb/usr/libexec/apt/methods/https",
                             "/var/jb/bin/sh", "/var/jb/usr/lib/ex/exrecover", "/var/jb/usr/libext/helper",
                             "/var/jb/Library/dpkg/info/package.postinst"})
        EXPECT_TRUE(switch_bootstrap_tool_path(path, SWITCH_BOOTSTRAP_ROOT)) << path;
    for (const auto *path : {SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo",
                             "/var/jb/usr/lib/App.app/App", "/var/jb/Library/../Applications/App.app/App",
                             "/var/jb-other/usr/bin/dpkg", "/usr/bin/dpkg"})
        EXPECT_FALSE(switch_bootstrap_tool_path(path, SWITCH_BOOTSTRAP_ROOT)) << path;
}

TEST(BootstrapPolicy, FollowsInjectorExclusionsAndPreservesExactLibraryEntries)
{
    for (const auto *path : {"/sbin/launchd", "/usr/libexec/xpcproxy", "/usr/libexec/logd",
                             "/usr/sbin/notifyd", "/usr/libexec/switch-appregd",
                             "/System/X/WebKit/run", "/private/X/GSSCRED"})
        EXPECT_FALSE(switch_bootstrap_injectable(path)) << path;
    EXPECT_TRUE(switch_bootstrap_injectable("/System/Library/CoreServices/SpringBoard.app/SpringBoard"));
    EXPECT_TRUE(switch_bootstrap_injectable("/Applications/App.app/App"));
    EXPECT_FALSE(switch_bootstrap_injectable(nullptr));
    EXPECT_TRUE(switch_bootstrap_env_has("/a:/b:/c", "/b"));
    EXPECT_FALSE(switch_bootstrap_env_has("/ab:/bc:/c", "/b"));
}

TEST(BootstrapPolicy, InitExecResetsReadinessForUserspaceReboot)
{
    proc process{0x80000000u, 42, nullptr};
    switch_bootstrap_ready = 1;
    switch_bootstrap_exec(&process, true);
    EXPECT_EQ(process.p_lflag, 0x80000000u | SWITCH_P_LTWEAKS);
    EXPECT_EQ(switch_bootstrap_ready, 1);
    process.pid = 1;
    switch_bootstrap_exec(&process, false);
    EXPECT_EQ(process.p_lflag, 0x80000000u);
    EXPECT_EQ(switch_bootstrap_ready, 0);
}
}
