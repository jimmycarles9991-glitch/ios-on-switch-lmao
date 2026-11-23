// SPDX-License-Identifier: MIT
#pragma once

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#define SWITCH_METAL_DIAGNOSTICS 1
#define SWITCH_METAL_DIAGNOSTIC(...) \
    do {                             \
        __VA_ARGS__;                 \
    } while (false)
#else
#define SWITCH_METAL_DIAGNOSTICS 0
#define SWITCH_METAL_DIAGNOSTIC(...) \
    do {                             \
    } while (false)
#endif
