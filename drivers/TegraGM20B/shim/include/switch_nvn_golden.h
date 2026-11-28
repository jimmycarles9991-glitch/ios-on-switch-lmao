/* SPDX-License-Identifier: MIT */
/* NVN's three golden-context register bits, the kernel's part (TegraGM20BNvnGolden.h). switch_nvn_golden: nvkm's gm20b
 * golden context gets them. switch_nvn_golden_skipped: the writes a debug kernel's switch-no-nvn-golden boot-arg leaves
 * out (0 in a release kernel). */
#ifndef SWITCH_NVN_GOLDEN_H
#define SWITCH_NVN_GOLDEN_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvkm_device;

void switch_nvn_golden(struct nvkm_device *device);
uint32_t switch_nvn_golden_skipped(void);

#ifdef __cplusplus
}
#endif
#endif
