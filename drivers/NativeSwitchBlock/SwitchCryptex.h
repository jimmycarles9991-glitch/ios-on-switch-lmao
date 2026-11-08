// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_CRYPTEX_H
#define SWITCH_CRYPTEX_H
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#define SWITCH_CRYPTEX_COUNT 2
#define SWITCH_PREBOOT_UUID  "4AA4A761-5138-439D-9A30-D4138F6887FA"
#ifdef __cplusplus
extern "C" {
#endif
struct mount;
/* Boot-only serialized attach of the unchanged images. index0=System,1=App.
 * Fixed root-owned files under /private/preboot/Switch-Staging on the exact
 * native RW Preboot device. r17-launch: a read-only block device reading the
 * image's extents straight from the SD card (the direct read map), or, if that
 * map is refused (one log line), the original DiskImages hook. Returns BSD
 * errno (0 success), not switch_block_status. A failure latches for this index. */
int switch_cryptex_attach(unsigned index, dev_t preboot, dev_t *device, char *path, size_t capacity);
/* The direct-attached image's volume did not mount: detach the map and attach
 * the image through DiskImages instead (one log line). EINVAL if it already is. */
int switch_cryptex_fallback(unsigned index, dev_t *device, char *path, size_t capacity);
/* Right after each cryptex mount: registers it with the cluster layer and,
 * once both are mounted, starts the boot prefetch (replay or recording). */
void switch_cryptex_mounted(unsigned index, struct mount *mount);
/* System Boot Complete (SwitchPower's boot-done hint): the boot prefetch's window
 * ends a few seconds later, and a recording is kept only from such a boot. */
void switch_boot_io_complete(void);
/* Revalidates the retained binding and unique exact-UUID RO APFS descendant.
 * Returns BSD errno; can be called after mounting to compare actual dev_t. */
int switch_cryptex_volume_status(unsigned index, dev_t *device, char *path, size_t capacity);
/* Only retained, valid System binding. Existing fixed development identity;
 * this is neither an image hash calculation nor Apple root authentication. */
int switch_cryptex_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
#ifdef __cplusplus
}
#endif
#endif
