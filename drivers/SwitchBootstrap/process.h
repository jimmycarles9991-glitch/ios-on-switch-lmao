// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void switch_bootstrap_set_budget(unsigned milliseconds);
unsigned switch_bootstrap_remaining_ms(void);
int switch_bootstrap_run(const char *path, char *const arguments[]);
int switch_bootstrap_run_prep(const char *path, char *const arguments[]);
int switch_bootstrap_run_update(const char *path, char *const arguments[]);
int switch_bootstrap_run_daemon(const char *action, const char *plist, int *result);
#ifdef __cplusplus
}
#endif
