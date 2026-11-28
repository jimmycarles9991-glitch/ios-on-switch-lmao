// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/procs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

int *switch_all_pids(int *count)
{
    const int estimate = proc_listallpids(NULL, 0);
    if (estimate <= 0)
        return NULL;
    const size_t capacity = (size_t)estimate + 64;
    int *pids = calloc(capacity, sizeof(*pids));
    if (!pids)
        return NULL;
    *count = proc_listallpids(pids, (int)(capacity * sizeof(*pids)));
    if (*count < 0) {
        free(pids);
        return NULL;
    }
    return pids;
}

// Matches the executable's file name: pbsi_comm holds 15 characters, and daemon names are often longer.
bool switch_pid_is_named(int pid, const char *name)
{
    char path[1024];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0)
        return false;
    const char *base = strrchr(path, '/');
    return strcmp(base ? base + 1 : path, name) == 0;
}

size_t switch_pids_named(const char *name, int *out, size_t capacity)
{
    int count = 0;
    int *pids = switch_all_pids(&count);
    if (!pids)
        return 0;
    size_t found = 0;
    for (int i = 0; i < count && found < capacity; ++i) {
        if (pids[i] > 0 && switch_pid_is_named(pids[i], name))
            out[found++] = pids[i];
    }
    free(pids);
    return found;
}
