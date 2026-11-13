// SPDX-License-Identifier: MIT
#pragma once

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#define SWITCH_RAW_DIAGNOSTICS 1
#define RAW_DIAGNOSTIC(...) \
    do {                    \
        __VA_ARGS__;        \
    } while (false)
#else
#define SWITCH_RAW_DIAGNOSTICS 0
#define RAW_DIAGNOSTIC(...) \
    do {                    \
    } while (false)
#endif
