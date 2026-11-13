// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SWITCH_RAW_HARNESS_MARKS)
// Standalone raw command tools may omit the translator; dynamic XR draws then refuse explicitly.
void switch_nmb_blend_color_words(const float color[4], uint32_t words[8]) __attribute__((weak_import));
#else
void switch_nmb_blend_color_words(const float color[4], uint32_t words[8]);
#endif

#ifdef __cplusplus
}
#endif
