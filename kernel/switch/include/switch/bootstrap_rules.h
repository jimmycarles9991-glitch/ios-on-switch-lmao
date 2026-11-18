// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_BOOTSTRAP_RULES_H
#define SWITCH_BOOTSTRAP_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SWITCH_P_LTWEAKS 0x00000200u
#define SWITCH_BOOTSTRAP_SIGNATURE 3u
#define SWITCH_BOOTSTRAP_INJECTOR "/var/jb/usr/lib/ellekit/libinjector.dylib"
#define SWITCH_BOOTSTRAP_PERSONA 99u
#define SWITCH_BOOTSTRAP_SILEO_IDENTITY "org.coolstar.SileoStore"
#define SWITCH_BOOTSTRAP_HELPER_IDENTITY "com.brunostjohn.switch.bootstrap"
#define SWITCH_BOOTSTRAP_EXEC_CALLER 1u
#define SWITCH_BOOTSTRAP_EXEC_LAUNCHER 2u

static inline bool switch_bootstrap_owned(uint32_t uid, uint32_t gid, uint32_t mode, uint64_t links, bool directory)
{
    return uid == 0 && gid == 0 && !(mode & 0022) && (directory || links == 1);
}

static inline bool switch_bootstrap_contains_case(const char *path, const char *word)
{
    for (; *path; ++path) {
        size_t n = 0;
        while (word[n]) {
            unsigned char c = (unsigned char)path[n], w = (unsigned char)word[n];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (w >= 'A' && w <= 'Z') w += 'a' - 'A';
            if (!c || c != w) break;
            ++n;
        }
        if (!word[n]) return true;
    }
    return false;
}

static inline bool switch_bootstrap_tool_path(const char *path, const char *root)
{
    const char *const roots[] = { root, "/var/jb", "/private/var/jb" };
    if (!path) return false;
    for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); ++i) {
        const size_t root_length = strlen(roots[i]);
        if (strncmp(path, roots[i], root_length) || path[root_length] != '/') continue;
        const char *relative = path + root_length + 1;
        if (!*relative) return false;
        if (switch_bootstrap_contains_case(relative, ".app/") ||
            switch_bootstrap_contains_case(relative, ".appex/") ||
            switch_bootstrap_contains_case(relative, ".xpc/")) return false;
        for (const char *part = relative; *part;) {
            const char *end = strchr(part, '/');
            const size_t length = end ? (size_t)(end - part) : strlen(part);
            if (!length || (length == 1 && *part == '.') ||
                (length == 2 && !memcmp(part, "..", 2))) return false;
            part += length;
            if (*part) ++part;
        }
        return true;
    }
    return false;
}

static inline bool switch_bootstrap_app_path(const char *path, const char *root, bool executable)
{
    const size_t root_length = strlen(root);
    if (!path || strncmp(path, root, root_length) ||
        strncmp(path + root_length, "/Applications", sizeof("/Applications") - 1)) return false;
    const char *bundle = path + root_length + sizeof("/Applications") - 1;
    if (!*bundle) return !executable;
    if (*bundle++ != '/') return false;
    const char *end = strchr(bundle, '/');
    const size_t length = end ? (size_t)(end - bundle) : strlen(bundle);
    if (length <= 4 || memcmp(bundle + length - 4, ".app", 4)) return false;
    if (!end) return !executable;
    if (!end[1]) return !executable;
    if (executable && strchr(end + 1, '/')) {
        const char *extension;
        const char *suffix;
        if (!strncmp(end + 1, "PlugIns/", sizeof("PlugIns/") - 1)) {
            extension = end + sizeof("/PlugIns/") - 1;
            suffix = ".appex";
        } else if (!strncmp(end + 1, "XPCServices/", sizeof("XPCServices/") - 1)) {
            extension = end + sizeof("/XPCServices/") - 1;
            suffix = ".xpc";
        } else return false;
        const char *executable_name = strchr(extension, '/');
        const size_t suffix_length = strlen(suffix);
        if (!executable_name || (size_t)(executable_name - extension) <= suffix_length ||
            memcmp(executable_name - suffix_length, suffix, suffix_length) ||
            !executable_name[1] || strchr(executable_name + 1, '/')) return false;
    }
    for (const char *part = end + 1; *part;) {
        const char *next = strchr(part, '/');
        const size_t count = next ? (size_t)(next - part) : strlen(part);
        if (!count || (count == 1 && *part == '.') ||
            (count == 2 && !memcmp(part, "..", 2))) return false;
        part += count;
        if (*part) ++part;
    }
    return true;
}

static inline bool switch_bootstrap_injectable(const char *path)
{
    static const char *const excluded[] = {
        "BlastDoor", "mobile_assertion_agent", "watchdog", "webkit", "jailbreakd", "loader", "GSSCred",
        "/sbin/launchd", "/usr/libexec/xpcproxy", "/usr/libexec/logd", "/usr/sbin/notifyd",
        "/usr/libexec/usermanagerd", "/usr/libexec/switch-appregd", "switch-bootstrap",
    };
    if (!path || path[0] != '/') return false;
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); ++i)
        if (switch_bootstrap_contains_case(path, excluded[i])) return false;
    return true;
}

static inline bool switch_bootstrap_env_has(const char *value, const char *library)
{
    const size_t length = strlen(library);
    while (*value) {
        const char *end = strchr(value, ':');
        const size_t count = end ? (size_t)(end - value) : strlen(value);
        if (count == length && !memcmp(value, library, length)) return true;
        if (!end) break;
        value = end + 1;
    }
    return false;
}

#endif
