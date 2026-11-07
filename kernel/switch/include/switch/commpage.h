// SPDX-License-Identifier: GPL-2.0-only
/*
 * The CPU features the commpage and hw.optional.arm.* publish (osfmk/arm/commpage/commpage.c).
 *
 * Included by osfmk/arm/commpage/commpage.c, after its own includes: the ID_AA64ISAR0_EL1 fields (arm64/proc_reg.h)
 * and kHasARMv8Crypto (arm/cpu_capabilities.h) come from there.
 */
#ifndef SWITCH_COMMPAGE_H
#define SWITCH_COMMPAGE_H

#include <stdint.h>

extern int gARM_FEAT_AES;
extern int gARM_FEAT_PMULL;
extern int gARM_FEAT_SHA1;
extern int gARM_FEAT_SHA256;

/*
 * The ARMv8 crypto extensions, from ID_AA64ISAR0_EL1. Apple's arm64 board configs set them from
 * __ARM_V8_CRYPTO_EXTENSIONS__ (T7000's ISAR0 under-reports PMULL); the BCM2837 config this kernel builds with does not
 * define it, and the Cortex-A57's ISAR0 reports all four. kHasARMv8Crypto stands for the four together.
 */
static inline uint64_t switch_commpage_crypto_bits(uint64_t isar0)
{
#if __ARM_V8_CRYPTO_EXTENSIONS__
    (void)isar0;
    return 0;
#else
    const uint64_t aes = isar0 & ID_AA64ISAR0_EL1_AES_MASK;

    gARM_FEAT_AES = aes >= ID_AA64ISAR0_EL1_AES_EN;
    gARM_FEAT_PMULL = aes >= ID_AA64ISAR0_EL1_AES_PMULL_EN;
    gARM_FEAT_SHA1 = (isar0 & ID_AA64ISAR0_EL1_SHA1_MASK) >= ID_AA64ISAR0_EL1_SHA1_EN;
    gARM_FEAT_SHA256 = (isar0 & ID_AA64ISAR0_EL1_SHA2_MASK) >= ID_AA64ISAR0_EL1_SHA2_EN;
    return gARM_FEAT_AES && gARM_FEAT_PMULL && gARM_FEAT_SHA1 && gARM_FEAT_SHA256 ? kHasARMv8Crypto : 0;
#endif
}

#endif
