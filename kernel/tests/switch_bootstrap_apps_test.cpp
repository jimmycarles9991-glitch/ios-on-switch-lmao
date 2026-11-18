#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include "../../bazel/testing/source_text.h"
#include "../switch/include/switch/bootstrap_rules.h"

namespace {
#define SWITCH_BOOTSTRAP_ROOT "/private/var/Switch/bootstrap/TEST/root"
#define SWITCH_BOOTSTRAP_APPLICATIONS SWITCH_BOOTSTRAP_ROOT "/Applications"
#define SWITCH_BOOTSTRAP_HELPER "/usr/libexec/switch-bootstrap"
constexpr unsigned MAXPATHLEN = 1024;
constexpr int ZV_NAMEI = 0, UIO_SYSSPACE = 0, UIO_READ = 0;
constexpr unsigned CS_VALID = 1, CS_SIGNED = 0x20000000, CS_ENTITLEMENTS_VALIDATED = 0x4000;
constexpr unsigned VSUID = 04000, VSGID = 02000, IMGPF_INTERPRET = 1;
#ifndef FREAD
constexpr int FREAD = 1, FWRITE = 2;
#endif
#ifndef FEXEC
constexpr int FEXEC = 0x40000000;
#endif
using ino64_t = uint64_t;
struct cs_blob {
    unsigned csb_reconstituted = 0;
    unsigned flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    bool platform = true;
    int64_t csb_base_offset = 0;
    const char *identity = "native.code";
    const void *csb_cd = this;
};
struct vnode;
struct Mount { std::vector<vnode *> nodes; };
struct vnode {
    Mount *mount;
    uint64_t id, parent;
    const char *path;
    bool directory = true, symlink = false, acl = false, supported = true;
    uint32_t uid = 0, gid = 0, mode = 0755;
    uint64_t links = 1;
    int references = 0;
    cs_blob *blob = nullptr;
    const char *target = "";
};
using vnode_t = vnode *;
constexpr vnode_t NULLVP = nullptr;
struct vnode_attr {
    uint32_t va_uid, va_gid, va_mode;
    uint64_t va_nlink, va_fileid, va_parentid;
    void *va_acl;
    bool supported;
};
#define VATTR_INIT(va) (*(va) = {})
#define VATTR_WANTED(va, field) ((void)0)
#define VATTR_IS_SUPPORTED(va, field) ((va)->supported)
int vfs_context_kernel() { return 0; }
int vnode_getattr(vnode_t vp, vnode_attr *va, int)
{
    *va = {vp->uid, vp->gid, vp->mode, vp->links, vp->id, vp->parent,
           vp->acl ? vp : nullptr, vp->supported};
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
    for (vnode_t node : mount->nodes) {
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

char *zalloc(int) { return static_cast<char *>(std::malloc(MAXPATHLEN)); }
void zfree(int, void *memory) { std::free(memory); }
int vn_getpath(vnode_t vp, char *path, int *length)
{
    const size_t size = std::strlen(vp->path) + 1;
    if (size > static_cast<size_t>(*length)) return ENAMETOOLONG;
    std::memcpy(path, vp->path, size);
    *length = static_cast<int>(size);
    return 0;
}
bool switch_bootstrap_named(vnode_t vp, const char *path) { return vp && !std::strcmp(vp->path, path); }
struct uio { char *buffer = nullptr; size_t remaining = 0; };
using uio_t = uio *;
#define UIO_STACKBUF(name, count) uio name[1]
#ifndef CAST_USER_ADDR_T
#define CAST_USER_ADDR_T(value) reinterpret_cast<uintptr_t>(value)
#endif
uio_t uio_createwithbuffer(int, int, int, int, void *buffer, size_t) { return static_cast<uio_t>(buffer); }
int uio_addiov(uio_t io, uintptr_t target, size_t size)
{
    io->buffer = reinterpret_cast<char *>(target);
    io->remaining = size;
    return 0;
}
size_t uio_resid(uio_t io) { return io->remaining; }
int VNOP_READLINK(vnode_t vp, uio_t io, int)
{
    const size_t count = std::min(io->remaining, std::strlen(vp->target));
    std::memcpy(io->buffer, vp->target, count);
    io->remaining -= count;
    return 0;
}
#include "bootstrap_app_alias.h"

struct proc {
    vnode_t p_textvp;
    unsigned flags = 0x22002301;
    bool platform = true;
    int64_t p_textoff = 0;
    bool persona = true;
};
using proc_t = proc *;
proc current{};
proc_t current_proc() { return &current; }
unsigned proc_getcsflags(proc_t p) { return p->flags; }
bool csproc_get_platform_binary(proc_t p) { return p->platform; }
cs_blob *csproc_get_blob(proc_t p) { return p->p_textvp->blob; }
cs_blob *csvnode_get_blob(vnode_t vp, int64_t offset)
{
    return vp->blob && offset >= vp->blob->csb_base_offset &&
        offset < vp->blob->csb_base_offset + 8192 ? vp->blob : nullptr;
}
unsigned csblob_get_flags(cs_blob *blob) { return blob->flags; }
bool csblob_get_platform_binary(cs_blob *blob) { return blob->platform; }
const char *csblob_get_identity(cs_blob *blob) { return blob->identity; }
proc_t proc_task(proc_t p) { return p; }
bool IOTaskHasEntitlement(proc_t p, const char *name)
{
    return p->persona && !std::strcmp(name, "com.apple.private.persona-mgmt");
}
#include "bootstrap_app_signature.h"
#include "bootstrap_tool_image.h"
#include "bootstrap_persona_caller.h"
#include "bootstrap_service_caller.h"
bool switch_bootstrap_access(vnode_t, int) { return false; }
struct image_params {
    bool ip_switch_package_exec = false;
    vnode_t ip_vp, ip_scriptvp = nullptr;
    unsigned ip_flags = 0;
    vnode_attr *ip_vattr, *ip_origvattr;
    uint64_t ip_arch_offset = 0;
};
#include "bootstrap_apps.h"

unsigned int preflight(proc_t p, vnode_t vp)
{
    image_params image{};
    image.ip_vp = vp;
    return switch_bootstrap_exec_preflight(p, &image);
}

class BootstrapApps : public testing::Test {
protected:
    Mount mount, system;
    vnode data{&mount, 1, 1, "/private/var"}, parent{&mount, 2, 1, "/private/var/Switch/bootstrap/TEST"};
    vnode root{&mount, 3, 2, SWITCH_BOOTSTRAP_ROOT};
    vnode applications{&mount, 4, 3, SWITCH_BOOTSTRAP_APPLICATIONS};
    vnode app{&mount, 5, 4, SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app"};
    vnode executable{&mount, 6, 5, SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/Example"};
    vnode asset{&mount, 7, 5, SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/Info.plist"};
    vnode launcher{&system, 1, 1, "/usr/libexec/xpcproxy"};
    vnode alias{&mount, 8, 1, "/private/var/jb"};
    cs_blob native, signature{SWITCH_BOOTSTRAP_SIGNATURE};
    vnode_attr attributes{}, original{};
    image_params image{};
    void SetUp() override
    {
        mount.nodes = {&data, &parent, &root, &applications, &app, &executable, &asset, &alias};
        executable.directory = asset.directory = alias.directory = launcher.directory = false;
        alias.symlink = true;
        alias.mode = 0777;
        alias.target = SWITCH_BOOTSTRAP_ROOT;
        launcher.blob = &native;
        current = {&launcher};
        switch_bootstrap_root = &root;
        switch_bootstrap_data = &mount;
        switch_bootstrap_root_id = root.id;
        switch_bootstrap_data_id = data.id;
        image = {true, &executable, nullptr, 0, &attributes, &original, 0};
        executable.blob = &signature;
    }
    void TearDown() override
    {
        for (vnode_t node : mount.nodes) EXPECT_EQ(node->references, 0);
    }
};

TEST_F(BootstrapApps, PreflightAcceptsUncachedAppButFinalAdmissionRequiresSelectedSignedSlice)
{
    executable.blob = nullptr;
    ASSERT_TRUE(preflight(&current, &executable));
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    executable.blob = &signature;
    ASSERT_EQ(switch_bootstrap_exec_check(&image), 0);
    image.ip_arch_offset = 4096;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    signature.csb_base_offset = 4096;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
    EXPECT_FALSE(current.flags & CS_ENTITLEMENTS_VALIDATED);
}

TEST_F(BootstrapApps, ExecAndResourceAdmissionRequireNativeCallerAuthority)
{
    ASSERT_TRUE(switch_bootstrap_app_access(&asset, FREAD));
    ASSERT_TRUE(switch_bootstrap_read_access(&asset, FREAD));
    for (unsigned provenance : {2u, 3u, 4u}) {
        native.csb_reconstituted = provenance;
        EXPECT_FALSE(preflight(&current, &executable));
        EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
    }
    native.csb_reconstituted = 0;
    for (unsigned flag : {CS_VALID, CS_SIGNED}) {
        current.flags &= ~flag;
        EXPECT_FALSE(preflight(&current, &executable));
        EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
        current.flags |= flag;
    }
    current.platform = false;
    EXPECT_FALSE(preflight(&current, &executable));
    EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
    current.platform = true;
    launcher.blob = nullptr;
    EXPECT_FALSE(preflight(&current, &executable));
    EXPECT_FALSE(preflight(nullptr, &executable));
}

TEST_F(BootstrapApps, NativeLifecycleReadersNeedNoServiceNameAllowlistOrTweakReadiness)
{
    for (const char *path : {"/System/Library/CoreServices/iconservicesagent", "/usr/libexec/MTLAssetUpgraderD",
                             "/usr/libexec/lsd", "/usr/libexec/switch-appregd"}) {
        launcher.path = path;
        EXPECT_TRUE(switch_bootstrap_app_access(&asset, FREAD));
        EXPECT_TRUE(switch_bootstrap_app_access(&app, FREAD));
        EXPECT_FALSE(preflight(&current, &executable));
    }
    for (int mode : {FWRITE, FREAD | FWRITE, O_TRUNC, O_APPEND, FEXEC})
        EXPECT_FALSE(switch_bootstrap_app_access(&asset, mode));
    EXPECT_FALSE(switch_bootstrap_app_access(&root, FREAD));
    asset.path = SWITCH_BOOTSTRAP_ROOT "/Library/dpkg/status";
    EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
    executable.path = "/private/var/containers/Bundle/Application/UUID/Example.app/Example";
    EXPECT_FALSE(preflight(&current, &executable));
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
}

TEST_F(BootstrapApps, MembershipAndMetadataChangesRevokeBothPhasesAndResourceReads)
{
    for (vnode_t node : {&parent, &root, &applications, &app, &executable}) {
        node->uid = 501;
        EXPECT_FALSE(preflight(&current, &executable));
        EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
        node->uid = 0;
        node->mode |= 0020;
        EXPECT_FALSE(preflight(&current, &executable));
        node->mode &= ~0020u;
        node->acl = true;
        EXPECT_FALSE(preflight(&current, &executable));
        node->acl = false;
    }
    executable.links = 2;
    EXPECT_FALSE(preflight(&current, &executable));
    executable.links = 1;
    executable.parent = parent.id;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    asset.links = 2;
    EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
    asset.links = 1;
    app.uid = 501;
    EXPECT_FALSE(switch_bootstrap_app_access(&asset, FREAD));
}

TEST_F(BootstrapApps, FinalGateRejectsScriptsSetIdsAndChangedSignatureAuthority)
{
    image.ip_scriptvp = &asset;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    image.ip_scriptvp = nullptr;
    image.ip_flags = IMGPF_INTERPRET;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    image.ip_flags = 0;
    for (unsigned mode : {VSUID, VSGID}) {
        executable.mode |= mode;
        EXPECT_FALSE(preflight(&current, &executable));
        executable.mode &= ~mode;
        original.va_mode = mode;
        EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    }
    original.va_mode = 0;
    for (unsigned flag : {CS_VALID, CS_SIGNED, CS_ENTITLEMENTS_VALIDATED}) {
        signature.flags &= ~flag;
        EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
        signature.flags |= flag;
    }
    for (unsigned provenance : {0u, 1u, 2u}) {
        signature.csb_reconstituted = provenance;
        EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    }
    signature.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    signature.platform = false;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    image.ip_switch_package_exec = false;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
}

TEST_F(BootstrapApps, BundleRoutesPermitAliasesAndNativeExtensionsWhileRejectingEscapes)
{
    for (const char *path : {SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/PlugIns/Widget.appex/Widget",
                             SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/XPCServices/Service.xpc/Service"}) {
        executable.path = path;
        EXPECT_TRUE(preflight(&current, &executable));
        EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
    }
    for (const char *path : {SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/Frameworks/Kit.framework/Kit",
                             SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/PlugIns/Bad.app/Bad",
                             SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/PlugIns/Good.appex/../Bad",
                             SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/../Outside",
                             SWITCH_BOOTSTRAP_APPLICATIONS "/Example.app/./Example"}) {
        executable.path = path;
        EXPECT_FALSE(preflight(&current, &executable));
    }
    for (const char *path : {"/var/Switch/bootstrap/TEST/root/Applications/Example.app/Info.plist",
                             "/var/jb/Applications/Example.app/Info.plist",
                             "/private/var/jb/Applications/Example.app/Info.plist"})
        EXPECT_TRUE(switch_bootstrap_app_preflight(&data, path, std::strlen(path)));
    EXPECT_TRUE(switch_bootstrap_app_lookup(&root, "Applications", 12));
    EXPECT_TRUE(switch_bootstrap_app_lookup(&data, "jb", 2));
    EXPECT_FALSE(switch_bootstrap_app_lookup(&root, "Library", 7));
    EXPECT_FALSE(switch_bootstrap_app_lookup(&app, "..", 2));
    EXPECT_FALSE(switch_bootstrap_app_preflight(&app, "../../Library/dpkg/status", 25));
    EXPECT_FALSE(switch_bootstrap_app_preflight(&data, "/var/jb/Library/dpkg/status", 25));
    ASSERT_TRUE(switch_bootstrap_app_access(&alias, FREAD));
    alias.target = SWITCH_BOOTSTRAP_ROOT "/Library";
    EXPECT_FALSE(switch_bootstrap_app_access(&alias, FREAD));
    alias.target = SWITCH_BOOTSTRAP_ROOT;
    alias.uid = 501;
    EXPECT_FALSE(switch_bootstrap_app_access(&alias, FREAD));
}

TEST_F(BootstrapApps, LaunchProxyAndAdmittedCommandCanRunUncachedToolsAndCompiledMaintainers)
{
    executable.path = SWITCH_BOOTSTRAP_ROOT "/usr/bin/dash";
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_LAUNCHER);
    executable.path = SWITCH_BOOTSTRAP_ROOT "/Library/dpkg/info/package.postinst";
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_LAUNCHER);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
    vnode caller{&mount, 9, root.id, SWITCH_BOOTSTRAP_ROOT "/usr/bin/sbreload"};
    caller.directory = false;
    caller.blob = &signature;
    current.p_textvp = &caller;
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_CALLER);
    executable.blob = nullptr;
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_CALLER);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    executable.blob = &signature;
    caller.uid = 501;
    EXPECT_EQ(preflight(&current, &executable), 0u);
    caller.uid = 0;
    signature.csb_reconstituted = 2;
    EXPECT_EQ(preflight(&current, &executable), 0u);
    signature.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    caller.path = SWITCH_BOOTSTRAP_APPLICATIONS "/Other.app/Other";
    EXPECT_EQ(preflight(&current, &executable), 0u);
}

TEST_F(BootstrapApps, ExactSileoAndNativeHelperReuseTheirExistingAuthority)
{
    executable.path = SWITCH_BOOTSTRAP_ROOT "/usr/bin/dpkg";
    vnode caller{&mount, 9, root.id, SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo"};
    caller.directory = false;
    caller.blob = &signature;
    current.p_textvp = &caller;
    current.flags |= CS_ENTITLEMENTS_VALIDATED;
    signature.identity = SWITCH_BOOTSTRAP_SILEO_IDENTITY;
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_CALLER);
    current.persona = false;
    EXPECT_EQ(preflight(&current, &executable), 0u);
    current.persona = true;
    signature.identity = "other.app";
    EXPECT_EQ(preflight(&current, &executable), 0u);
    current.p_textvp = &launcher;
    launcher.path = SWITCH_BOOTSTRAP_HELPER;
    native.identity = SWITCH_BOOTSTRAP_HELPER_IDENTITY;
    EXPECT_EQ(preflight(&current, &executable), SWITCH_BOOTSTRAP_EXEC_CALLER);
    launcher.path = "/usr/libexec/other-native-service";
    EXPECT_EQ(preflight(&current, &executable), 0u);
}

TEST_F(BootstrapApps, MaintainerScriptsRequireRetainedOwnedOriginalAndSignedInterpreter)
{
    asset.path = SWITCH_BOOTSTRAP_ROOT "/Library/dpkg/info/package.postinst";
    executable.path = SWITCH_BOOTSTRAP_ROOT "/usr/bin/dash";
    image.ip_flags = IMGPF_INTERPRET;
    image.ip_scriptvp = &asset;
    EXPECT_EQ(switch_bootstrap_exec_preflight(&current, &image), SWITCH_BOOTSTRAP_EXEC_LAUNCHER);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
    asset.uid = 501;
    EXPECT_EQ(switch_bootstrap_exec_preflight(&current, &image), 0u);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    asset.uid = 0;
    asset.mode |= VSUID;
    EXPECT_EQ(switch_bootstrap_exec_preflight(&current, &image), 0u);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    asset.mode &= ~VSUID;
    asset.links = 2;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    asset.links = 1;
    executable.mount = &system;
    executable.path = "/usr/bin/env";
    executable.blob = nullptr;
    EXPECT_EQ(switch_bootstrap_exec_preflight(&current, &image), SWITCH_BOOTSTRAP_EXEC_LAUNCHER);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    executable.blob = &native;
    native.flags = CS_VALID;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), 0);
    for (unsigned provenance : {2u, 3u}) {
        native.csb_reconstituted = provenance;
        EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    }
    native.csb_reconstituted = 0;
    native.flags = 0;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    native.flags = CS_VALID;
    native.platform = false;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    native.platform = true;
    image.ip_arch_offset = 4096;
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
    image.ip_arch_offset = 0;
    image.ip_scriptvp = nullptr;
    EXPECT_EQ(switch_bootstrap_exec_preflight(&current, &image), 0u);
    EXPECT_EQ(switch_bootstrap_exec_check(&image), EPERM);
}
}

TEST(BootstrapAppsSource, BothKernelsCheckFinalSelectedImageBeforeCommittingItsMap)
{
    for (const char *version : {"10063.141.1", "12377.121.6"}) {
        const std::string base = std::string("../xnu-") + version + "/bsd/";
        const auto exec = switch_ios::test::CodeText(switch_ios::test::SourceText(base + "kern/kern_exec.c"));
        EXPECT_TRUE(exec.before("lret = load_machfile(imgp", "switch_bootstrap_exec_check(imgp)"));
        EXPECT_TRUE(exec.before("switch_bootstrap_exec_check(imgp)", "swap_task_map("));
        EXPECT_TRUE(exec.has("if ((error = switch_bootstrap_exec_check(imgp))) {"
                             "imgp->ip_free_map = map; map = VM_MAP_NULL; goto badtoolate; }"));
        const auto fields = switch_ios::test::CodeText(switch_ios::test::SourceText(base + "sys/imgact.h"));
        EXPECT_TRUE(fields.has("bool ip_switch_package_exec;"));
    }
}
