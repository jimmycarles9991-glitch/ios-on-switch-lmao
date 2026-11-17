// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_DYLD_POLICY_24B119_H
#define SWITCH_DYLD_POLICY_24B119_H

#include "app_code_macho.h"

#define SWITCH_DYLD_POLICY_FILE_OFFSET UINT64_C(0x2e2c8)
#define SWITCH_DYLD_POLICY_BRANCH UINT32_C(0x17fffedd)

static inline bool switch_dyld_policy_24B119_site(const uint8_t *image, size_t size, uint64_t slide,
                                                uint64_t *address)
{
    static const uint8_t uuid[16] = {
        0x17, 0xf0, 0x28, 0x09, 0xe0, 0xe1, 0x32, 0xf4,
        0x86, 0x2e, 0xfa, 0xcd, 0xe8, 0x62, 0xb8, 0xde,
    };
    if (size < 32 || switch_app_code_u32(image) != 0xfeedfacf || switch_app_code_u32(image + 12) != 7)
        return false;
    const uint32_t count = switch_app_code_u32(image + 16), bytes = switch_app_code_u32(image + 20);
    if (bytes > size - 32 || count > bytes / 8) return false;
    bool matched = false, located = false;
    size_t at = 32;
    for (uint32_t i = 0; i < count; ++i) {
        if (32 + bytes - at < 8) return false;
        const uint32_t command = switch_app_code_u32(image + at), length = switch_app_code_u32(image + at + 4);
        if (length < 8 || (length & 7) || length > 32 + bytes - at) return false;
        if (command == 0x1b) {
            if (length != 24 || matched) return false;
            matched = true;
            for (size_t n = 0; n < sizeof(uuid); ++n)
                if (image[at + 8 + n] != uuid[n]) return false;
        }
        if (command == 0x19) {
            if (length < 72) return false;
            const uint64_t file = switch_app_code_u64(image + at + 40);
            const uint64_t amount = switch_app_code_u64(image + at + 48);
            if ((switch_app_code_u32(image + at + 60) & 4) && file <= SWITCH_DYLD_POLICY_FILE_OFFSET &&
                SWITCH_DYLD_POLICY_FILE_OFFSET - file <= amount &&
                amount - (SWITCH_DYLD_POLICY_FILE_OFFSET - file) >= 12) {
                const uint64_t base = switch_app_code_u64(image + at + 24);
                const uint64_t delta = SWITCH_DYLD_POLICY_FILE_OFFSET - file;
                if (located || base > UINT64_MAX - delta || base + delta > UINT64_MAX - slide) return false;
                *address = base + delta + slide;
                located = true;
            }
        }
        at += length;
    }
    return matched && located && at == 32 + bytes;
}

static inline bool switch_dyld_policy_24B119_patch(uint32_t words[3])
{
    if (words[0] != UINT32_C(0xf94006a0) || words[1] != UINT32_C(0x3945ec08) ||
        (words[2] != UINT32_C(0x3707dba8) && words[2] != SWITCH_DYLD_POLICY_BRANCH)) return false;
    words[2] = SWITCH_DYLD_POLICY_BRANCH;
    return true;
}

#endif
