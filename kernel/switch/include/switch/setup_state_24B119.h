// SPDX-License-Identifier: GPL-2.0-only
/*
 * 24B119: the value iOS 27 has booted with. SpringBoard reads the domain as done ("Buddy completed: YES (domain:
 * com.apple.purplebuddy, SetupDone: YES ...)") and the Setup app does not run; budd, Setup's daemon, runs its
 * finish-setup handlers every boot. Not read from this build's Setup binaries.
 */
#ifndef SWITCH_SETUP_STATE_24B119_H
#define SWITCH_SETUP_STATE_24B119_H

#define SWITCH_SETUP_VERSION "11"

#endif
