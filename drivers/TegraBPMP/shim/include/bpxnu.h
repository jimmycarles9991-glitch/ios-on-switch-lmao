/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Boundary between the Linux-flavoured C side of TegraBPMP (vendored L4T mail.c, mail_t210.c, mailman.c plus
 * shim/linux.c and TegraBPMP.c) and XNU: kits/SwitchLinux for the kernel services, shim/xnu_glue.cpp for the BPMP
 * doorbells. Plain C, fixed-width types only, so neither side sees the other's names.
 */
#ifndef BPXNU_H
#define BPXNU_H
#include "../../../../kits/SwitchLinux/SwitchLinuxLock.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Claims the four inbound BPMP doorbells (GIC SPIs) once mail_t210.c has registered their handlers; panics
 * if one cannot be had. */
void bpxnu_doorbells_start(void);
/* A BPMP answer whose doorbell interrupt never came: one-line panic with the doorbell and GIC state. */
void bpxnu_doorbell_lost(int ch, int mrq) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
