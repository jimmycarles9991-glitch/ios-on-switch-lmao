// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE: the T210B01 Security Engines SE1 and SE2 as the kernel's AES and SHA engines (TegraSE.cpp).
#ifndef TEGRA_SE_H
#define TEGRA_SE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// From the board thread (SwitchRootDiag.h): starts the driver's own thread and returns.
int tegra_se_start(void);

// One owner of an engine at a time: the AppleKeyStore stand-in's boot probe (NativeKeyStoreEvents/
// TegraSecurityEngine.cpp) and this driver. engine 1 = SE1, 2 = SE2. Thread context; waits up to `ms`.
bool tegra_se_claim(unsigned engine, uint32_t owner, uint32_t ms);
void tegra_se_release(unsigned engine, uint32_t owner);

// System sleep (drivers/SwitchSuspend, one thread): suspend waits, bounded, for each engine's owner and running
// operation, then holds every engine (callers fall back to the CPU) with its interrupt off; resume re-arms the
// engines' end interrupts (SC7 clears INT_ENABLE) and lets callers in again.
void tegra_se_suspend(void);
void tegra_se_resume(void);
#define TEGRA_SE_OWNER_AKS_PROBE 1u
#define TEGRA_SE_OWNER_DRIVER    2u
// SE2-only forms of tegra_se_claim / tegra_se_release.
bool tegra_se2_claim(uint32_t owner, uint32_t ms);
void tegra_se2_release(uint32_t owner);
#define TEGRA_SE2_OWNER_AKS_PROBE TEGRA_SE_OWNER_AKS_PROBE
#define TEGRA_SE2_OWNER_DRIVER    TEGRA_SE_OWNER_DRIVER

// Kernel callers. Each returns 0 when the engine did the work (bit-identical to the CPU's), or nonzero, and then the
// caller must do it on the CPU: an algorithm a check turned off, a buffer the engine cannot reach, or a context that
// cannot sleep. Thread context with preemption enabled; buffers are kernel virtual addresses (any alignment, any
// physical layout: scatter-gather, pages above 4 GiB through a bounce buffer).
enum tegra_se_mode {
    TEGRA_SE_ECB = 0,
    TEGRA_SE_CBC = 1,
    TEGRA_SE_CTR = 2,  // 128-bit big-endian counter in iv, advanced by the blocks done; a tail is done here too
    TEGRA_SE_OFB = 3,
};

// AES-128/192/256 (keyBytes 16/24/32). ECB/CBC: len a multiple of 16. iv (16 bytes, not ECB) is updated to the next
// operation's: CBC the last ciphertext block, CTR the next counter, OFB the next keystream input.
int tegra_se_aes(enum tegra_se_mode mode, bool encrypt, const uint8_t *key, unsigned keyBytes, uint8_t *iv,
                 const void *in, void *out, size_t len);
// AES-CMAC (RFC 4493 / NIST SP 800-38B) of len bytes.
int tegra_se_cmac(const uint8_t *key, unsigned keyBytes, const void *in, size_t len, uint8_t mac[16]);
// One-shot digest: which is 1 (SHA-1), 224, 256, 384 or 512.
int tegra_se_digest(unsigned which, const void *in, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
