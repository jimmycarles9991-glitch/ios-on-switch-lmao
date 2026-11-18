// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <os/atomic_private.h>
#include <switch/bootstrap.h>
#include <switch/migration_home.h>

static bool switch_home_native;
static bool switch_migration_scheduled;

void switch_migration_home_requested(void)
{
    switch_home_native = true;
}

void switch_migration_home_prepare(bool has_payload, bool pending)
{
    // Apple's migrator owns the lifetime of its request and may need SpringBoard throughout it.
    os_atomic_store(&switch_migration_scheduled, has_payload && (switch_home_native || pending), relaxed);
}

static int switch_home_ready_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
    int ready = !os_atomic_load(&switch_migration_scheduled, relaxed);
    int error = SYSCTL_OUT(req, &ready, sizeof(ready));
    if (error || !req->newptr) return error;
    if (kauth_cred_getuid(kauth_cred_get()) != 0 || !switch_bootstrap_service_caller(current_proc())) return EPERM;
    if ((error = SYSCTL_IN(req, &ready, sizeof(ready)))) return error;
    if (ready != 1) return EINVAL;
    os_atomic_store(&switch_migration_scheduled, false, relaxed);
    return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, switch_migration_home_ready, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            NULL, 0, switch_home_ready_sysctl, "I", "Bootstrap migration is no longer scheduled for this boot");
