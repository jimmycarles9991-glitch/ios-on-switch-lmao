// SPDX-License-Identifier: GPL-2.0-only
/*
 * The selected IPSW's Setup Assistant values the boot-time setup in bsd/kern/kern_exec.c writes into
 * com.apple.purplebuddy (SWITCH_SETUP_VERSION, a decimal string), one header per build. kern_exec.c's -include names
 * the build (ipsw/ipsw.bzl ipsw_volumes_header).
 */
#ifndef SWITCH_SETUP_STATE_H
#define SWITCH_SETUP_STATE_H

#if defined(SWITCH_IPSW_KIND_21H461)
#include <switch/setup_state_21H461.h>
#elif defined(SWITCH_IPSW_KIND_24B119)
#include <switch/setup_state_24B119.h>
#else
#error "the Setup Assistant values of the selected IPSW"
#endif

#endif
