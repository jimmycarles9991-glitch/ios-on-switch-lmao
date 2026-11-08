// SPDX-License-Identifier: GPL-2.0-only
/*
 * What prelink26 binds an iOS 27 kext import to when our kernel has no function of the name Apple's kernel calls it
 * by. translate/arm64e/xnu/prelink26/shims.py lists each import with the one it gets and why.
 */
#include <sys/errno.h>
#include <kern/debug.h>

int switch_prelink_enosys(void);
__abortlike void switch_prelink_unreachable(void);

/* A code-signing monitor call this kernel, which runs without a monitor, does not take (txm.c's answer). */
int switch_prelink_enosys(void)
{
    return ENOSYS;
}

/* Code only Apple SoC hardware the console's ADT does not describe can reach; the caller's site names the import. */
__abortlike void switch_prelink_unreachable(void)
{
    panic("SwitchPrelink: kext code for hardware this console lacks called an import shims.py answers as unreachable "
          "(from %p)",
          __builtin_return_address(0));
}
