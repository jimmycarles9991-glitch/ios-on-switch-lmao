// SPDX-License-Identifier: BSD-2-Clause-Patent
// The MSB-first bit reader and start-code scan the MPEG-1/2 and MPEG-4 Part 2 parsers share. Reads past the end
// return 0 and set `bad`, so a parser checks once at the end of a header.
#ifndef AVD_BITS_H
#define AVD_BITS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *p;
    size_t bits, pos;
    bool bad;
} AVDBits;

static inline AVDBits avd_bits(const uint8_t *data, size_t size)
{
    return (AVDBits){.p = data, .bits = size * 8};
}

static inline uint32_t avd_u(AVDBits *b, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (b->pos >= b->bits) {
            b->bad = true;
            return 0;
        }
        v = v << 1 | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static inline void avd_skip(AVDBits *b, size_t n)
{
    if (b->pos + n > b->bits)
        b->bad = true;
    b->pos = b->pos + n > b->bits ? b->bits : b->pos + n;
}

// A marker bit must be 1; a 0 marks the header bad.
static inline void avd_marker(AVDBits *b)
{
    if (!avd_u(b, 1))
        b->bad = true;
}

// The offset of the next 00 00 01 prefix at or after `from`, or `size` when there is none.
static inline size_t avd_next_start_code(const uint8_t *data, size_t size, size_t from)
{
    for (size_t i = from; i + 3 <= size; i++) {
        if (data[i + 2] > 1)
            i += 2;
        else if (!data[i] && !data[i + 1] && data[i + 2] == 1)
            return i;
    }
    return size;
}

#endif
