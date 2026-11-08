// SPDX-License-Identifier: BSD-2-Clause-Patent
// A sysctl a driver registers at run time (sysctl_register_oid), filled the one way arm64 XNU accepts: CTLFLAG_OID2 and
// the current SYSCTL_OID_VERSION. An oid without them panics there ("Old style sysctl without a version number isn't
// supported"), at boot, where no build gate sees it. Every hand-built oid goes through switch_sysctl_oid (test/).
#ifndef SWITCH_SYSCTL_H
#define SWITCH_SYSCTL_H

#include <string.h>
#include <sys/sysctl.h>

#ifdef __cplusplus
extern "C" {
#endif

// kind: CTLTYPE_* | CTLFLAG_* (RD, KERN, LOCKED...); OID2 is added here. The strings must outlive the oid.
static inline void switch_sysctl_oid(struct sysctl_oid *oid, struct sysctl_oid_list *parent, const char *name, int kind,
                                     int(*handler) SYSCTL_HANDLER_ARGS, const char *format, const char *description)
{
    memset(oid, 0, sizeof(*oid));
    oid->oid_parent = parent;
    oid->oid_number = OID_AUTO;
    oid->oid_kind = kind | CTLFLAG_OID2;  // NOLINT(bugprone-signed-bitwise): XNU's CTLTYPE/CTLFLAG bits are int
    oid->oid_name = name;
    oid->oid_handler = handler;
    oid->oid_fmt = format;
    oid->oid_descr = description;
    oid->oid_version = SYSCTL_OID_VERSION;
}

#ifdef __cplusplus
}
#endif

#endif
