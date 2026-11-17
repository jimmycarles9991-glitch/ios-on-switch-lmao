/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <stddef.h>
#include <string.h>

#define SWITCH_CAPABILITIES_MAX_SIZE 65536
#define SWITCH_CAPABILITIES_MAX_DEPTH 8
#define SWITCH_CAPABILITIES_IDENTITY_MAX 255

static inline int switch_capability_prefix(const char *value, const char *prefix)
{
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

static inline int switch_capability_namespace(const char *value, const char *prefix)
{
    const size_t length = strlen(prefix);
    return !strncmp(value, prefix, length) && (!value[length] || value[length] == '.');
}

static inline int switch_capability_group_allowed(const char *value)
{
    if (!value || !value[0] || strchr(value, '*') || strchr(value, '?') ||
        switch_capability_namespace(value, "apple") || switch_capability_namespace(value, "group.apple") ||
        switch_capability_namespace(value, "com.apple") || switch_capability_namespace(value, "group.com.apple") ||
        switch_capability_namespace(value, "systemgroup") || switch_capability_namespace(value, "group.system"))
        return 0;
    for (const char *part = value; *part; ++part)
        if (*part == '.' && switch_capability_namespace(part + 1, "com.apple")) return 0;
    return 1;
}

static inline int switch_capability_group_key(const char *key)
{
    return !strcmp(key, "keychain-access-groups") ||
           !strcmp(key, "com.apple.security.application-groups") ||
           !strcmp(key, "com.apple.developer.icloud-container-identifiers") ||
           !strcmp(key, "com.apple.developer.ubiquity-container-identifiers") ||
           !strcmp(key, "com.apple.developer.parent-application-identifiers") ||
           !strcmp(key, "com.apple.developer.icloud-container-development-container-identifiers");
}

static inline int switch_capability_identity_key(const char *key)
{
    return !strcmp(key, "application-identifier") ||
           !strcmp(key, "com.apple.application-identifier") ||
           !strcmp(key, "com.apple.developer.team-identifier") ||
           !strcmp(key, "com.apple.developer.associated-application-identifier") ||
           !strcmp(key, "com.apple.developer.ubiquity-kvstore-identifier");
}

static inline int switch_capability_key_allowed(const char *key)
{
    static const char *const denied[] = {
        "platform-application", "get-task-allow", "task_for_pid-allow", "run-unsigned-code",
        "dynamic-codesigning", "seatbelt-profiles", "no-sandbox", "no-container", "allow-root",
        "proc_info-allow", "skip-library-validation", "allow-jit", "allow-unsigned-executable-memory",
        "com.apple.developer.system-extension", "com.apple.developer.driverkit",
        "com.apple.developer.endpoint-security", "com.apple.developer.endpointsecurity",
        "com.apple.developer.kernel", "com.apple.developer.security",
        "com.apple.developer.system-container", "com.apple.developer.debug",
        "com.apple.developer.private", "com.apple.developer.privileged", "com.apple.developer.root",
        "com.apple.developer.platform", "com.apple.developer.no-sandbox", "com.apple.developer.no-container",
    };
    if (!key || !key[0] || strchr(key, '*') || strchr(key, '?'))
        return 0;
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        size_t length = strlen(denied[i]);
        if (!strncmp(key, denied[i], length) && (!key[length] || key[length] == '.'))
            return 0;
    }
    if (switch_capability_group_key(key) || switch_capability_identity_key(key))
        return 1;
    if (switch_capability_prefix(key, "com.apple."))
        return switch_capability_prefix(key, "com.apple.developer.");
    return strchr(key, '.') || !strcmp(key, "aps-environment") || !strcmp(key, "inter-app-audio") ||
           !strcmp(key, "beta-reports-active");
}
