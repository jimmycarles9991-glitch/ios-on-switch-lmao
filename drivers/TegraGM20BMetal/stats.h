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

#if SWITCH_METAL_DIAGNOSTICS && defined(SWITCH_STATS_GPU) && SWITCH_STATS_GPU
#define SWITCH_METAL_STATS 1
#define SWITCH_METAL_STAT(...) \
    do {                       \
        __VA_ARGS__;           \
    } while (false)
#else
#define SWITCH_METAL_STATS 0
#define SWITCH_METAL_STAT(...) \
    do {                       \
    } while (false)
#endif

#if SWITCH_METAL_DIAGNOSTICS && defined(SWITCH_PROF_GPU) && SWITCH_PROF_GPU
#define SWITCH_METAL_PROF 1
#define SWITCH_METAL_PROFD(...) \
    do {                        \
        __VA_ARGS__;            \
    } while (false)
#else
#define SWITCH_METAL_PROF 0
#define SWITCH_METAL_PROFD(...) \
    do {                        \
    } while (false)
#endif
