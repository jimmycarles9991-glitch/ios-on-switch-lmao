// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int switch_compiler_compare(const char *key, const uint8_t *data, size_t bytes, char *message, size_t room);
#ifdef __cplusplus
}
#endif
