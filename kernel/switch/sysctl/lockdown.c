// SPDX-License-Identifier: GPL-2.0-only
/*
 * security.mac.lockdown_mode_state_public, xnu-13432's Lockdown Mode state for platform binaries and apps with the
 * lockdown-mode entitlement. iOS 27's os_lockdown_mode_enabled (libsystem_darwin) reads it once and aborts the caller
 * when the read fails; launchd and amfid call it.
 */
#include <IOKit/IOBSD.h>
#include <sys/codesign.h>
#include <sys/lockdown_mode.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#define SWITCH_LOCKDOWN_STATE_ENTITLEMENT "com.apple.developer.lockdown-mode.state"

static int switch_lockdown_mode_state_public SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
    if (!csproc_get_platform_binary(current_proc()) &&
        !IOCurrentTaskHasEntitlement(SWITCH_LOCKDOWN_STATE_ENTITLEMENT)) {
        return EPERM;
    }
    int state = get_lockdown_mode_state();
    return SYSCTL_OUT(req, &state, sizeof(state));
}

SYSCTL_DECL(_security_mac);
SYSCTL_PROC(_security_mac, OID_AUTO, lockdown_mode_state_public, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            switch_lockdown_mode_state_public, "I", "Lockdown Mode state (public)");
