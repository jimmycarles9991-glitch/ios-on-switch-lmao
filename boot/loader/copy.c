#include "copy.h"
#include "layout.h"
#include "stage.h"

#define PAGE            UINT64_C(0x1000)
#define BLOCK           UINT64_C(0x200000)
#define ADAPTER         UINT64_C(0xa0000000)
#define REFERENCE       UINT64_C(0xa001f000)
#define NORMAL          (UINT64_C(1) << 2)
#define INNER_SHAREABLE (UINT64_C(3) << 8)
#define ACCESSED        (UINT64_C(1) << 10)
#define READONLY        (UINT64_C(2) << 6)
#define PXN             (UINT64_C(1) << 53)
#define UXN             (UINT64_C(1) << 54)
#define RAM             (NORMAL | INNER_SHAREABLE | ACCESSED | UXN)
#define DATA            (RAM | PXN)

/* Image-backed, aligned storage: the loader does not depend on BSS clearing.
 * 0=L1, 1=L2 RAM, 2=L2 framebuffer, 3=adapter L3, 4=source tail L3,
 * 5=destination head L3, 6=destination tail L3, 7=source head L3 (the staged
 * window in bl33.bin starts mid-block, stage.h). */
uint64_t loader_copy_tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES]
    __attribute__((section(".copy_tables"), aligned(4096))) = {{0}};

static void map_ram(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES], uint64_t table_address,
                    uint64_t start, uint64_t end, uint64_t attributes, unsigned head_table, unsigned tail_table)
{
    uint64_t first_block = start & ~(BLOCK - 1);
    for (uint64_t address = start; address < end;) {
        unsigned l2 = (unsigned)((address >> 21) & 511);
        if (!(address & (BLOCK - 1)) && end - address >= BLOCK) {
            tables[1][l2] = address | attributes | 1;
            address += BLOCK;
        } else {
            unsigned page_table = (address & ~(BLOCK - 1)) == first_block ? head_table : tail_table;
            tables[1][l2] = (table_address + page_table * PAGE) | 3;
            tables[page_table][(address >> 12) & 511] = address | attributes | 3;
            address += PAGE;
        }
    }
}

int loader_copy_build_tables(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES],
                             uint64_t table_address, const struct loader_copy_bounds *b)
{
    if (!tables || !b || ((uintptr_t)tables & (PAGE - 1)) || (table_address & (PAGE - 1)) || table_address < ADAPTER ||
        table_address >= REFERENCE || b->image_end != table_address + LOADER_COPY_TABLE_PAGES * PAGE ||
        b->image_end > REFERENCE || b->rx_end < ADAPTER + PAGE || (b->rx_end & (PAGE - 1)) || b->ro_end < b->rx_end ||
        (b->ro_end & (PAGE - 1)) || b->ro_end > table_address || b->source != SWITCH_STAGE_PA || !b->bytes ||
        (b->bytes & (PAGE - 1)) || b->bytes > LOADER_WINDOW_BYTES_MAX || !b->source_bytes ||
        (b->source_bytes & (PAGE - 1)) || b->source_bytes > SWITCH_STAGE_END - PAGE - SWITCH_STAGE_PA ||
        (b->destination & (PAGE - 1)) || b->destination < UINT64_C(0xb2000000) ||
        b->destination >= UINT64_C(0xc0000000) || b->bytes > UINT64_C(0xc0000000) - b->destination)
        return 1;

    for (unsigned page = 0; page < LOADER_COPY_TABLE_PAGES; ++page)
        for (unsigned entry = 0; entry < LOADER_COPY_TABLE_ENTRIES; ++entry)
            tables[page][entry] = 0;

    /* All addresses are below 4 GiB. TTBR1 is disabled. Nothing else has a
     * valid descriptor, including the fixed SD reference at 0xa001f000. */
    tables[0][2] = (table_address + PAGE) | 3;
    tables[0][3] = (table_address + 2 * PAGE) | 3;
    tables[1][(ADAPTER >> 21) & 511] = (table_address + 3 * PAGE) | 3;
    for (uint64_t address = ADAPTER; address < b->image_end; address += PAGE) {
        uint64_t permissions = address < b->rx_end ? RAM | READONLY : address < b->ro_end ? DATA | READONLY : DATA;
        tables[3][(address >> 12) & 511] = address | permissions | 3;
    }
    map_ram(tables, table_address, b->source, b->source + b->source_bytes, DATA | READONLY, 7, 4);
    map_ram(tables, table_address, b->destination, b->destination + b->bytes, DATA, 5, 6);

    /* Firmware reserves these complete two blocks for the framebuffer. They
     * remain Device-nGnRnE, non-executable. Display helpers access only the
     * measured 0x384000-byte pixel span; the trailing reserved bytes are idle. */
    for (uint64_t address = UINT64_C(0xf5a00000); address < UINT64_C(0xf5e00000); address += BLOCK)
        tables[2][(address >> 21) & 511] = address | ACCESSED | PXN | UXN | 1;
    return 0;
}

/* 64-byte blocks; bytes is a nonzero multiple of 64 (callers pass 4 KiB multiples). Inline loops, so the compiler
 * cannot turn them into memcpy/memset calls the freestanding image does not have. */
static void copy_blocks(uint64_t to, uint64_t from, uint64_t bytes)
{
    __asm__ volatile("1:\n"
                     "ldp x9, x10, [%1]\n"
                     "ldp x11, x12, [%1, #16]\n"
                     "ldp x13, x14, [%1, #32]\n"
                     "ldp x15, x16, [%1, #48]\n"
                     "stp x9, x10, [%0]\n"
                     "stp x11, x12, [%0, #16]\n"
                     "stp x13, x14, [%0, #32]\n"
                     "stp x15, x16, [%0, #48]\n"
                     "add %1, %1, #64\n"
                     "add %0, %0, #64\n"
                     "subs %2, %2, #64\n"
                     "b.ne 1b\n"
                     : "+r"(to), "+r"(from), "+r"(bytes)
                     :
                     : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "cc", "memory");
}

static void zero_blocks(uint64_t to, uint64_t bytes)
{
    __asm__ volatile("1:\n"
                     "stp xzr, xzr, [%0]\n"
                     "stp xzr, xzr, [%0, #16]\n"
                     "stp xzr, xzr, [%0, #32]\n"
                     "stp xzr, xzr, [%0, #48]\n"
                     "add %0, %0, #64\n"
                     "subs %1, %1, #64\n"
                     "b.ne 1b\n"
                     : "+r"(to), "+r"(bytes)
                     :
                     : "cc", "memory");
}

/* 16 bytes at any alignment. The decoder runs in loader_copy_run's regime: Normal memory with SCTLR_EL1.A clear, where
 * unaligned LDP/STP are architectural (-mstrict-align only stops the compiler from emitting them itself). */
static inline void copy16(uint8_t *to, const uint8_t *from)
{
#ifdef LOADER_COPY_HOST_TEST
    /* Host tests: plain C, so AddressSanitizer checks every access the decoder makes. */
    uint8_t step[16];
    for (unsigned i = 0; i < 16; ++i)
        step[i] = from[i];
    for (unsigned i = 0; i < 16; ++i)
        to[i] = step[i];
#else
    uint64_t low, high;
    __asm__ volatile("ldp %0, %1, [%2]" : "=&r"(low), "=&r"(high) : "r"(from) : "memory");
    __asm__ volatile("stp %0, %1, [%2]" : : "r"(low), "r"(high), "r"(to) : "memory");
#endif
}

/* Exactly length bytes, forward. from is at least 16 bytes behind to, or in another buffer. */
static void copy_exact(uint8_t *to, const uint8_t *from, uint64_t length)
{
    for (; length >= 16; length -= 16, to += 16, from += 16)
        copy16(to, from);
    for (; length; --length)
        *to++ = *from++;
}

/* length bytes rounded up to 16, forward: the caller has that much room at to (and to read at from). from is at least
 * 16 bytes behind to, or in another buffer; the bytes past length are rewritten by the output that follows. */
static void copy_wild(uint8_t *to, const uint8_t *from, uint64_t length)
{
    uint8_t *const end = to + length;
    do {
        copy16(to, from);
        to += 16;
        from += 16;
    } while (to < end);
}

/* An LZ4 length extension: 255-valued bytes continue it. 0, or 1 past the end of the input. */
static int lz4_length(const uint8_t **in, const uint8_t *end, uint64_t *length)
{
    unsigned more;
    do {
        if (*in == end)
            return 1;
        more = *(*in)++;
        *length += more;
    } while (more == 255);
    return 0;
}

/* One LZ4 block (lz4.org block format) into [out, out + out_bytes) from [in, in + in_bytes). 0 only when its sequences
 * use the input exactly and fill the output exactly. Every literal run, match length and offset is checked against
 * both ends before it is copied, so a damaged file is refused, never written or read outside its block. */
static int lz4_block(uint8_t *const out, const uint64_t out_bytes, const uint8_t *in, const uint64_t in_bytes)
{
    const uint8_t *const in_end = in + in_bytes;
    uint8_t *op = out;
    uint8_t *const out_end = out + out_bytes;
    for (;;) {
        if (in == in_end)
            return 1;
        const unsigned token = *in++;
        uint64_t length = token >> 4;
        if (length == 15 && lz4_length(&in, in_end, &length))
            return 1;
        if (length > (uint64_t)(in_end - in) || length > (uint64_t)(out_end - op))
            return 1;
        if (length) {
            if ((uint64_t)(in_end - in) >= length + 16 && (uint64_t)(out_end - op) >= length + 16)
                copy_wild(op, in, length);
            else
                copy_exact(op, in, length);
            op += length;
            in += length;
        }
        if (in == in_end)
            return op == out_end ? 0 : 1; /* the last sequence is literals only */
        if (in_end - in < 2)
            return 1;
        const uint64_t offset = (uint64_t)in[0] | (uint64_t)in[1] << 8;
        in += 2;
        if (!offset || offset > (uint64_t)(op - out))
            return 1;
        length = token & 15;
        if (length == 15 && lz4_length(&in, in_end, &length))
            return 1;
        length += 4;
        if (length > (uint64_t)(out_end - op))
            return 1;
        const uint8_t *match = op - offset;
        if (offset < 16) {
            /* A short period: bytes one at a time until the output repeats at the smallest multiple of the offset that
             * is at least 16 bytes back, then whole steps from there (the same bytes, a period further back). */
            uint64_t period = offset;
            while (period < 16)
                period += offset;
            const uint64_t head = length < period ? length : period;
            for (uint64_t i = 0; i < head; ++i)
                op[i] = match[i];
            op += head;
            length -= head;
            match = op - period;
        }
        if (length) {
            if ((uint64_t)(out_end - op) >= length + 16)
                copy_wild(op, match, length);
            else
                copy_exact(op, match, length);
            op += length;
        }
    }
}

int loader_unpack(uint64_t destination, uint64_t bytes, uint64_t source, uint64_t source_bytes,
                  const uint64_t (*blocks)[4], unsigned count, void (*progress)(uint64_t, uint64_t, uint64_t),
                  uint64_t started)
{
    uint64_t at = 0, read = 0;
    if (bytes & (PAGE - 1))
        return 1;
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t offset = blocks[i][0], length = blocks[i][1], from = blocks[i][2];
        const uint64_t packed = blocks[i][3] & ~LOADER_BLOCK_STORED;
        if (offset < at || offset > bytes || !length || length > bytes - offset || ((offset | length) & (PAGE - 1)) ||
            from != read || !packed || packed > source_bytes - read)
            return 1;
        if (offset > at)
            zero_blocks(destination + at, offset - at);
        if (blocks[i][3] & LOADER_BLOCK_STORED) {
            if (packed != length)
                return 1;
            copy_blocks(destination + offset, source + from, length);
        } else if (lz4_block((uint8_t *)(uintptr_t)(destination + offset), length,
                             (const uint8_t *)(uintptr_t)(source + from), packed)) {
            return 1;
        }
        read += packed;
        at = offset + length;
        if (progress)
            progress(at, bytes, started);
    }
    if (read != source_bytes)
        return 1;
    if (bytes > at)
        zero_blocks(destination + at, bytes - at);
    if (progress)
        progress(bytes, bytes, started);
    return 0;
}

#ifndef LOADER_COPY_HOST_TEST
#include "runs.h"

/* Under loader_copy_run's temporary cacheable regime: the staged file at LOAD_SOURCE (the window's nonzero extents,
 * LZ4-compressed in blocks, back to back) decoded into the window at LOAD_DEST, zeros everywhere else. The builder
 * checked that the table decodes to the exact layout image. */
int loader_copy_payload(void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started)
{
    return loader_unpack(LOAD_DEST, LOAD_BYTES, LOAD_SOURCE, LOAD_STREAM_BYTES, loader_blocks, LOAD_BLOCK_COUNT,
                         progress, started);
}

extern char __loader_rx_end[], __loader_ro_end[], __image_end[];

int loader_copy_prepare(void)
{
    /* Static: a local this size is built with a memcpy call, which the freestanding image does not have. */
    static const struct loader_copy_bounds bounds = {LOAD_SOURCE,
                                                     LOAD_DEST,
                                                     LOAD_BYTES,
                                                     (uint64_t)(uintptr_t)__loader_rx_end,
                                                     (uint64_t)(uintptr_t)__loader_ro_end,
                                                     (uint64_t)(uintptr_t)__image_end,
                                                     LOAD_PACKED_BYTES};
    return loader_copy_build_tables(loader_copy_tables, (uint64_t)(uintptr_t)loader_copy_tables, &bounds);
}
#endif
