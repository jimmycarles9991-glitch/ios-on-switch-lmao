// SPDX-License-Identifier: BSD-2-Clause-Patent
// The file SwitchNVRAM keeps the NVRAM bank in (/private/var/Switch/nvram.bin on the Data volume): its format, in
// plain C with no kernel dependency, so the host tests (test/switch_nvram_store_test.cpp) check this same header.
//
// Two slots, written alternately. A slot is a 32-byte header, then the bank image IODTNVRAM serialized (a CHRP bank:
// the "nvram" header with its adler32, then the common partition), zero-padded to a 4 KiB multiple (the stride):
//    0  magic     "SWCHNVRM"
//    8  version   1
//   12  bytes     image length (/chosen nvram-bank-size when it was written)
//   16  sequence  +1 per write (64-bit); of the slots that check out, the one with the higher sequence is current
//   24  reserved  0
//   28  crc32     CRC-32 (zlib's) of bytes 0-27 and the image
// Little-endian. Slot n is at n * stride. Each write goes to the slot that does not hold the current bank, so a write
// a power cut tears leaves the current one intact. A slot counts only if its header and CRC match and its image is a
// bank IODTNVRAM accepts (CHRP header checksum, adler32), since IONVRAMCHRPHandler panics on a bad adler; with no
// slot left the NVRAM starts empty.
#ifndef SWITCH_NVRAM_STORE_H
#define SWITCH_NVRAM_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../kits/SwitchCRC/SwitchCRC.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_NVRAM_STORE_VERSION   1u
#define SWITCH_NVRAM_STORE_HEADER    32u
#define SWITCH_NVRAM_STORE_MAX_IMAGE 0x10000u

static const char switch_nvram_store_magic[8] = {'S', 'W', 'C', 'H', 'N', 'V', 'R', 'M'};

static inline uint32_t switch_nvram_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void switch_nvram_put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t switch_nvram_store_stride(uint32_t bytes)
{
    return (SWITCH_NVRAM_STORE_HEADER + bytes + 4095u) & ~4095u;
}

// IONVRAMCHRPHandler's checks, in its order: "nvram" or "2nvram" header with a valid CHRP checksum
// (nvram_validate_header_v1v2), then the adler32 of everything from the generation on (adler32_with_version).
static inline int switch_nvram_chrp_valid(const uint8_t *image, uint32_t bytes)
{
    if (bytes < 32 || bytes > SWITCH_NVRAM_STORE_MAX_IMAGE)
        return 0;
    if (memcmp(image + 4, "nvram", 5) && memcmp(image + 4, "2nvram", 6))
        return 0;
    uint32_t sum = image[0];
    for (int i = 2; i < 16; ++i)
        sum += image[i];
    while (sum > 0xff)
        sum = (sum & 0xff) + (sum >> 8);
    if (sum != image[1])
        return 0;
    uint32_t low = 1;
    uint32_t high = 0;
    for (uint32_t i = 20; i < bytes; ++i) {
        low = (low + image[i]) % 65521u;
        high = (high + low) % 65521u;
    }
    return switch_nvram_get32(image + 16) == (high << 16 | low) ? 1 : 0;
}

// Fills one slot (switch_nvram_store_stride(bytes) bytes at slot) for image.
static inline void switch_nvram_store_fill(uint8_t *slot, const uint8_t *image, uint32_t bytes, uint64_t sequence)
{
    memset(slot, 0, switch_nvram_store_stride(bytes));
    memcpy(slot, switch_nvram_store_magic, sizeof(switch_nvram_store_magic));
    switch_nvram_put32(slot + 8, SWITCH_NVRAM_STORE_VERSION);
    switch_nvram_put32(slot + 12, bytes);
    switch_nvram_put32(slot + 16, (uint32_t)sequence);
    switch_nvram_put32(slot + 20, (uint32_t)(sequence >> 32));
    memcpy(slot + SWITCH_NVRAM_STORE_HEADER, image, bytes);
    const uint32_t crc = switch_crc32(0, slot, 28);
    switch_nvram_put32(slot + 28, switch_crc32(crc, image, bytes));
}

// The image in slot n of the file's first length bytes, if that slot checks out for a bank of bytes; else NULL.
static inline const uint8_t *switch_nvram_store_slot(const uint8_t *file, size_t length, uint32_t bytes, unsigned n,
                                                     uint64_t *sequence)
{
    const size_t stride = switch_nvram_store_stride(bytes);
    if (bytes > SWITCH_NVRAM_STORE_MAX_IMAGE || n > 1 || length < n * stride + SWITCH_NVRAM_STORE_HEADER + bytes)
        return NULL;
    const uint8_t *slot = file + n * stride;
    const uint8_t *image = slot + SWITCH_NVRAM_STORE_HEADER;
    if (memcmp(slot, switch_nvram_store_magic, sizeof(switch_nvram_store_magic)) ||
        switch_nvram_get32(slot + 8) != SWITCH_NVRAM_STORE_VERSION || switch_nvram_get32(slot + 12) != bytes ||
        switch_nvram_get32(slot + 24) != 0)
        return NULL;
    const uint32_t crc = switch_crc32(switch_crc32(0, slot, 28), image, bytes);
    if (crc != switch_nvram_get32(slot + 28) || !switch_nvram_chrp_valid(image, bytes))
        return NULL;
    *sequence = (uint64_t)switch_nvram_get32(slot + 16) | (uint64_t)switch_nvram_get32(slot + 20) << 32;
    return image;
}

// The current slot of the file (0 or 1, its sequence in *sequence), or -1 if neither checks out.
static inline int switch_nvram_store_pick(const uint8_t *file, size_t length, uint32_t bytes, uint64_t *sequence)
{
    uint64_t found[2] = {0, 0};
    const int ok0 = switch_nvram_store_slot(file, length, bytes, 0, &found[0]) != NULL ? 1 : 0;
    const int ok1 = switch_nvram_store_slot(file, length, bytes, 1, &found[1]) != NULL ? 1 : 0;
    if (!ok0 && !ok1)
        return -1;
    const int pick = (ok1 && (!ok0 || found[1] > found[0])) ? 1 : 0;
    *sequence = found[pick];
    return pick;
}

#ifdef __cplusplus
}
#endif

#endif
