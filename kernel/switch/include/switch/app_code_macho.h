// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_APP_CODE_MACHO_H
#define SWITCH_APP_CODE_MACHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint32_t switch_app_code_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t switch_app_code_u64(const uint8_t *p)
{
    return switch_app_code_u32(p) | (uint64_t)switch_app_code_u32(p + 4) << 32;
}

static inline uint32_t switch_app_code_be32(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

static inline uint64_t switch_app_code_be64(const uint8_t *p)
{
    return (uint64_t)switch_app_code_be32(p) << 32 | switch_app_code_be32(p + 4);
}

static inline size_t switch_app_code_fat_bytes(const uint8_t *p, size_t size)
{
    if (size < 8) return 0;
    const uint32_t magic = switch_app_code_be32(p);
    if (magic != 0xcafebabe && magic != 0xcafebabf) return 8;
    const uint32_t count = switch_app_code_be32(p + 4);
    return count && count <= 256 ? 8 + count * (magic == 0xcafebabf ? 32 : 20) : 0;
}

static inline bool switch_app_code_slice_size(const uint8_t *p, size_t size, uint64_t file_size,
                                               uint64_t slice, uint64_t *result)
{
    const size_t bytes = switch_app_code_fat_bytes(p, size);
    if (!bytes || size < bytes) return false;
    if (bytes == 8) {
        if (slice) return false;
        *result = file_size;
        return true;
    }
    const bool wide = switch_app_code_be32(p) == 0xcafebabf;
    const size_t stride = wide ? 32 : 20;
    bool found = false;
    for (size_t at = 8; at < bytes; at += stride) {
        const uint64_t offset = wide ? switch_app_code_be64(p + at + 8) : switch_app_code_be32(p + at + 8);
        const uint64_t length = wide ? switch_app_code_be64(p + at + 16) : switch_app_code_be32(p + at + 12);
        if (offset < bytes || !length || offset > file_size || length > file_size - offset) return false;
        if (offset == slice) {
            if (found) return false;
            found = true;
            *result = length;
        }
        for (size_t other = 8; other < at; other += stride) {
            const uint64_t lo = wide ? switch_app_code_be64(p + other + 8) : switch_app_code_be32(p + other + 8);
            const uint64_t len = wide ? switch_app_code_be64(p + other + 16) : switch_app_code_be32(p + other + 12);
            if (offset < lo + len && lo < offset + length) return false;
        }
    }
    return found;
}

static inline bool switch_app_code_macho_coverage(const uint8_t *p, size_t size, uint64_t slice_size,
                                                  uint64_t *coverage)
{
    if (size < 32 || switch_app_code_u32(p) != 0xfeedfacf || switch_app_code_u32(p + 4) != 0x0100000c) return false;
    const uint32_t count = switch_app_code_u32(p + 16), bytes = switch_app_code_u32(p + 20);
    if (!count || bytes < 8 || bytes > 256 * 1024 || slice_size < 32 || bytes > slice_size - 32 ||
        bytes > size - 32) return false;
    size_t at = 32;
    uint64_t end = 32 + bytes;
    bool executable = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (32 + bytes - at < 8) return false;
        const uint32_t command = switch_app_code_u32(p + at), length = switch_app_code_u32(p + at + 4);
        if (length < 8 || (length & 7) || length > 32 + bytes - at) return false;
        if (command == 0x19) {
            if (length < 72 || switch_app_code_u32(p + at + 64) > (length - 72) / 80) return false;
            const uint64_t offset = switch_app_code_u64(p + at + 40), amount = switch_app_code_u64(p + at + 48);
            if (offset > slice_size || amount > slice_size - offset) return false;
            if (end < offset + amount) end = offset + amount;
            executable |= (switch_app_code_u32(p + at + 60) & 4) && amount;
        }
        at += length;
    }
    if (at != 32 + bytes || !executable) return false;
    *coverage = end;
    return true;
}

#endif
