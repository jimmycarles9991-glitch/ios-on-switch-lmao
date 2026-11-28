// SPDX-License-Identifier: GPL-2.0-only
/*
 * The build this boot runs, for the kernel log (debug kernels): tools/deploy writes the commit it deployed to the Data
 * volume's own folder (<Data>/Switch/deployed.json, tools/deploy/deployed.py). The kernel itself carries no commit, so
 * a commit never invalidates a cached kernel. The scheduling report tries from its first window until the file reads,
 * so a boot that never reaches System Boot Complete still names its build once Data is mounted.
 */
#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <os/log.h>
#include <string.h>

#include "deployed.h"

#if DEVELOPMENT || DEBUG

#define DEPLOYED_PATH "/private/var/Switch/deployed.json"

bool switch_log_deployed(void)
{
    static bool missing_logged;
    char json[SWITCH_DEPLOYED_MAX];
    struct switch_deployed d;
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int resid = 0;
    int error = vnode_open(DEPLOYED_PATH, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);

    if (error != 0) {
        if (!missing_logged) {
            missing_logged = true;
            os_log(OS_LOG_DEFAULT, "SwitchBuild: no %s yet (%d): this boot's commit is not known", DEPLOYED_PATH,
                   error);
        }
        return false;
    }
    error = vn_rdwr(UIO_READ, vp, json, sizeof(json) - 1, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                    vfs_context_proc(ctx));
    (void)vnode_close(vp, FREAD, ctx);
    if (error != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchBuild: %s unreadable (%d)", DEPLOYED_PATH, error);
        return true;
    }
    json[sizeof(json) - 1 - (size_t)resid] = '\0';
    switch_deployed_parse(json, &d);
    os_log(OS_LOG_DEFAULT, "SwitchBuild: deployed %s%s from %s at %s, build %s", d.commit, d.dirty ? " (dirty)" : "",
           d.branch, d.deployed_at, d.build);
    return true;
}

#endif /* DEVELOPMENT || DEBUG */
