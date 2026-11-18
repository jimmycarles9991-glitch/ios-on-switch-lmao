#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include "../switch/include/switch/bootstrap_rules.h"

#define SWITCH_BOOTSTRAP_HELPER "/usr/libexec/switch-bootstrap"

namespace {
constexpr unsigned CS_VALID = 1, CS_SIGNED = 0x20000000;
struct vnode { const char *path = SWITCH_BOOTSTRAP_HELPER; };
struct cs_blob {
    unsigned csb_reconstituted = 0;
    const char *identity = SWITCH_BOOTSTRAP_HELPER_IDENTITY;
};
struct proc {
    vnode *p_textvp;
    cs_blob *blob;
    unsigned flags = CS_VALID | CS_SIGNED;
    bool platform = true;
};
using proc_t = proc *;
unsigned proc_getcsflags(proc_t p) { return p->flags; }
bool csproc_get_platform_binary(proc_t p) { return p->platform; }
bool switch_bootstrap_named(vnode *vp, const char *path) { return !std::strcmp(vp->path, path); }
cs_blob *csproc_get_blob(proc_t p) { return p->blob; }
const char *csblob_get_identity(cs_blob *blob) { return blob->identity; }
#include "bootstrap_service_caller.h"

struct Request { const int *newptr; int output = -1; };
int switch_bootstrap_ready, switch_bootstrap_recovery, capture_error;
unsigned current_uid;
proc_t current;
proc_t current_proc() { return current; }
unsigned kauth_cred_get() { return current_uid; }
unsigned kauth_cred_getuid(unsigned uid) { return uid; }
int switch_bootstrap_capture_root() { return capture_error; }
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, Request *req)
#define SYSCTL_OUT(req, value, size) ((req)->output = *(value), 0)
#define SYSCTL_IN(req, value, size) (*(value) = *(req)->newptr, 0)
#define os_atomic_load(value, order) (*(value))
#define os_atomic_store(value, state, order) (*(value) = (state))
#include "bootstrap_readiness.h"

TEST(BootstrapService, ReadinessRequiresRootServiceAndSuccessfulTreeCapture)
{
    vnode image;
    cs_blob blob;
    proc caller{&image, &blob};
    current = &caller;
    current_uid = 0;
    switch_bootstrap_recovery = switch_bootstrap_ready = capture_error = 0;
    int enabled = 1;
    Request request{&enabled};
    const auto write = [&] { return switch_bootstrap_ready_sysctl(nullptr, nullptr, 0, &request); };
    current_uid = 501;
    EXPECT_EQ(write(), EPERM);
    EXPECT_EQ(switch_bootstrap_ready, 0);
    current_uid = 0;
    blob.csb_reconstituted = 2;
    EXPECT_EQ(write(), EPERM);
    blob.csb_reconstituted = 0;
    capture_error = ENOENT;
    EXPECT_EQ(write(), ENOENT);
    EXPECT_EQ(switch_bootstrap_ready, 0);
    capture_error = 0;
    EXPECT_EQ(write(), 0);
    EXPECT_EQ(switch_bootstrap_ready, 1);
    switch_bootstrap_recovery = 1;
    EXPECT_EQ(write(), EPERM);
    enabled = 0;
    capture_error = ENOENT;
    EXPECT_EQ(write(), 0);
    EXPECT_EQ(switch_bootstrap_ready, 0);
    enabled = 2;
    EXPECT_EQ(write(), EINVAL);
    request.newptr = nullptr;
    current_uid = 501;
    EXPECT_EQ(write(), 0);
    EXPECT_EQ(request.output, 0);
}

TEST(BootstrapService, ActivationRequiresTheNativeStaticServiceAtItsFixedSystemPath)
{
    vnode image;
    cs_blob blob;
    proc caller{&image, &blob};
    ASSERT_TRUE(switch_bootstrap_service_caller(&caller));
    blob.csb_reconstituted = 1;
    EXPECT_TRUE(switch_bootstrap_service_caller(&caller));
    for (unsigned provenance : {2u, SWITCH_BOOTSTRAP_SIGNATURE}) {
        blob.csb_reconstituted = provenance;
        EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    }
    blob.csb_reconstituted = 0;
    for (const char *path : {"/private/var/Switch/bootstrap/TEST/switch-bootstrap",
                             "/private/var/containers/Bundle/Application/UUID/Helper.app/Helper"}) {
        image.path = path;
        EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    }
    image.path = SWITCH_BOOTSTRAP_HELPER;
    blob.identity = SWITCH_BOOTSTRAP_SILEO_IDENTITY;
    EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    blob.identity = nullptr;
    EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    blob.identity = SWITCH_BOOTSTRAP_HELPER_IDENTITY;
    for (unsigned flag : {CS_VALID, CS_SIGNED}) {
        caller.flags &= ~flag;
        EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
        caller.flags |= flag;
    }
    caller.platform = false;
    EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    caller.platform = true;
    caller.blob = nullptr;
    EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    caller.p_textvp = nullptr;
    EXPECT_FALSE(switch_bootstrap_service_caller(&caller));
    EXPECT_FALSE(switch_bootstrap_service_caller(nullptr));
}
}
