// SPDX-License-Identifier: BSD-2-Clause-Patent
// Whether a task's signature grants a boolean entitlement. Only kOSBooleanTrue grants it: a string or number under the
// same key is a different entitlement's shape and grants nothing.
#ifndef SWITCH_ENTITLEMENT_H
#define SWITCH_ENTITLEMENT_H

#include <IOKit/IOUserClient.h>

static inline bool switch_task_entitled(task_t task, const char *entitlement)
{
    OSObject *value = IOUserClient::copyClientEntitlement(task, entitlement);
    const bool entitled = value == kOSBooleanTrue;
    OSSafeReleaseNULL(value);
    return entitled;
}

#endif
