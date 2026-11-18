// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
bool switch_bootstrap_relative_path(const char *path);
int switch_bootstrap_extract(const char *archive_path, const char *directory);
int switch_bootstrap_extract_core(const char *archive_path, const char *directory);
#ifdef __cplusplus
}
#endif
