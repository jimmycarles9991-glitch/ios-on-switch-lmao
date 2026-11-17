// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_APP_CODE_RULES_H
#define SWITCH_APP_CODE_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_APP_CODE_MARKER ".SwitchUnsigned"
#define SWITCH_P_LAPP_CODE 0x00000100u

static inline bool switch_app_code_amfi(const char *name)
{
    return name && name[0] == 'A' && name[1] == 'M' && name[2] == 'F' && name[3] == 'I' && name[4] == '\0';
}

static inline size_t switch_app_code_bundle_end(const char *path)
{
    static const char *const roots[] = {
        "/private/var/containers/Bundle/Application/", "/var/containers/Bundle/Application/",
    };
    if (!path) return 0;
    for (size_t root = 0; root < sizeof(roots) / sizeof(roots[0]); ++root) {
        size_t at = 0;
        while (roots[root][at] && roots[root][at] == path[at]) ++at;
        if (roots[root][at]) continue;
        for (unsigned component = 0; component < 2; ++component) {
            const size_t begin = at;
            while (path[at] && path[at] != '/') ++at;
            if (at == begin || (at == begin + 1 && path[begin] == '.') ||
                (at == begin + 2 && path[begin] == '.' && path[begin + 1] == '.')) return 0;
            if (path[at] != '/') return 0;
            if (component == 1) return path[at + 1] ? at : 0;
            ++at;
        }
    }
    return 0;
}

static inline bool switch_app_code_marker_valid(bool regular, uint32_t mode, uint64_t size, uint64_t links)
{
    /* The marker discards signing privileges; ownership cannot grant any. */
    return regular && (mode & 07777) == 0600 && size == 0 && links == 1;
}

#endif
