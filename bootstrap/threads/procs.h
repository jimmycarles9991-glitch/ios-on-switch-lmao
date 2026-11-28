// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int *switch_all_pids(int *count);
bool switch_pid_is_named(int pid, const char *name);
size_t switch_pids_named(const char *name, int *out, size_t capacity);

#ifdef __cplusplus
}
#endif
