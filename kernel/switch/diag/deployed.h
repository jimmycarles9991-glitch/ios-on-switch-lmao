// SPDX-License-Identifier: GPL-2.0-only
/*
 * deployed.json's fields (tools/deploy/deployed.py writes them with json.dumps: string values without escapes, "dirty"
 * a JSON boolean), for deployed.c's one line. A field that is missing or longer than its buffer reads "?". Plain C, so
 * kernel/tests runs it on the host.
 */
#ifndef SWITCH_DIAG_DEPLOYED_H
#define SWITCH_DIAG_DEPLOYED_H

#include <stdbool.h>
#include <stddef.h>

#define SWITCH_DEPLOYED_MAX 1024u

struct switch_deployed {
    char commit[41];
    char branch[64];
    char build[16];
    char deployed_at[32];
    bool dirty;
};

/* One line naming the deployed build; false while the file cannot be opened (Data not mounted yet). */
bool switch_log_deployed(void);

static inline const char *switch_deployed_value(const char *json, const char *key)
{
    for (const char *at = json; *at != '\0'; at++) {
        size_t k = 0;

        if (*at != '"') {
            continue;
        }
        while (key[k] != '\0' && at[1 + k] == key[k]) {
            k++;
        }
        if (key[k] != '\0' || at[1 + k] != '"') {
            continue;
        }
        const char *v = at + 2 + k;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r' || *v == ':') {
            v++;
        }
        return v;
    }
    return NULL;
}

static inline void switch_deployed_string(const char *json, const char *key, char *out, size_t size)
{
    const char *v = switch_deployed_value(json, key);
    size_t n = 0;

    out[0] = '?';
    out[1] = '\0';
    if (v == NULL || *v != '"') {
        return;
    }
    for (v++; v[n] != '"'; n++) {
        if (v[n] == '\0' || v[n] == '\\' || n + 1 == size) {
            return;
        }
    }
    __builtin_memcpy(out, v, n);
    out[n] = '\0';
}

static inline void switch_deployed_parse(const char *json, struct switch_deployed *d)
{
    const char *dirty = switch_deployed_value(json, "dirty");

    switch_deployed_string(json, "commit", d->commit, sizeof(d->commit));
    switch_deployed_string(json, "branch", d->branch, sizeof(d->branch));
    switch_deployed_string(json, "build", d->build, sizeof(d->build));
    switch_deployed_string(json, "deployed_at", d->deployed_at, sizeof(d->deployed_at));
    d->dirty = dirty != NULL && dirty[0] == 't';
}

#endif /* SWITCH_DIAG_DEPLOYED_H */
