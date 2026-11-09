#include <stdint.h>
#include "record.h"
#include "display.h"
#include "layout.h"
#include "memory.h"
#include "sd_reference.h"
#include "copy.h"
#include "stamps.h"
#include "stage.h"
#include "emc.h"
#include "seed.h"
#include "boot_args.h"

#ifndef LOAD_FAST_COPY
#define LOAD_FAST_COPY 0
#endif
#ifndef LOAD_STAMPS_ADT_OFFSET
#define LOAD_STAMPS_ADT_OFFSET 0
#endif
#ifndef LOAD_EMC_KHZ
#define LOAD_EMC_KHZ 0
#endif
#ifndef LOAD_SEED_ADT_OFFSET
#define LOAD_SEED_ADT_OFFSET 0
#endif
#ifndef SWITCH_VERBOSE_CONSOLE
#error "SWITCH_VERBOSE_CONSOLE: 1 when -v in bootargs.txt selects the text console (debug), 0 when it never does"
#endif

extern volatile uint64_t probe_record[R_RECORD_WORDS];
extern void probe_halt(void) __attribute__((noreturn));
extern void loader_enter_kernel(uint64_t args, uint64_t entry) __attribute__((noreturn));
#if LOAD_VERIFY_KERNEL
extern uint32_t loader_crc32_update(uint32_t crc, const volatile void *address, uint64_t bytes);
#endif
volatile uint64_t loader_progress[8] __attribute__((section(".data"))) = {0};
#if LOAD_FAST_COPY
volatile uint64_t loader_copy_prepare_ticks __attribute__((section(".data"))) = 0;
#endif

// Additional words in the existing 512-byte diagnostic record.
enum {
    L_SOURCE_CRC = 48,
    L_DEST_CRC,
    L_SOURCE,
    L_DEST,
    L_BYTES,
    L_ARGS,
    L_ENTRY,
    L_MEMORY_CHECK,
    L_RESERVATIONS,
    L_RESERVED_NODES,
    L_GICD_CTLR,
    L_GICD_TYPER,
    L_GICD_ENABLE,
    L_GICC_CTLR,
    L_GICC_PMR,
    L_GICC_IIDR
};

_Static_assert((LOAD_BYTES & 7) == 0, "copy requires complete aligned words");
_Static_assert((LOAD_SOURCE & 7) == 0 && (LOAD_DEST & 7) == 0, "unaligned copy");
_Static_assert(LOAD_BYTES > 0 && LOAD_BYTES <= LOADER_WINDOW_BYTES_MAX, "bounded payload");
_Static_assert(LOAD_SOURCE + LOAD_BYTES <= LOAD_DEST || LOAD_DEST + LOAD_BYTES <= LOAD_SOURCE,
               "copy source and destination overlap");
_Static_assert(LOAD_ARGS >= LOAD_DEST && LOAD_ARGS + 0x480 <= LOAD_DEST + LOAD_BYTES, "arguments outside payload");
_Static_assert(LOAD_ENTRY >= LOAD_DEST && LOAD_ENTRY < LOAD_DEST + LOAD_BYTES, "entry outside payload");

static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value)::"memory");
    return value;
}

#if LOAD_STAMPS_ADT_OFFSET
/* Pre-kernel timing (stamps.h), written into the ADT just before XNU starts. Diagnostic only: nothing here refuses. */
static volatile uint64_t boot_stamps[SWITCH_STAMP_COUNT] __attribute__((section(".data"))) = {0};

/* The microsecond timer U-Boot sampled, against the counter, once at entry. */
static void stamps_entry(void)
{
    boot_stamps[SWITCH_STAMP_MAGIC] = SWITCH_STAMP_RECORD_MAGIC;
    boot_stamps[SWITCH_STAMP_WORDS] = SWITCH_STAMP_COUNT;
    boot_stamps[SWITCH_STAMP_FREQUENCY] = probe_record[R_EL1_CNTFRQ];
    boot_stamps[SWITCH_STAMP_LOADER_ENTRY] = probe_record[R_COUNTER_PRE];
    const uint64_t before = ticks();
    const uint32_t us = *(const volatile uint32_t *)(uintptr_t)SWITCH_TIMERUS_PA;
    const uint64_t after = ticks();
    boot_stamps[SWITCH_STAMP_TIMERUS] = us;
    boot_stamps[SWITCH_STAMP_TIMERUS_AT] = before + (after - before) / 2;
}

/* U-Boot's samples (stamps.h slot) as counter values: only those taken after the counter started and before this
 * loader did. The slot is then cleared, so a later boot never reads this one's. The caller checked the slot against
 * the live FDT's reservations. */
static void stamps_uboot(void)
{
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)SWITCH_BOOT_STAMPS_PA;
    if (slot[0] != SWITCH_BOOT_STAMPS_UBOOT_MAGIC)
        return;
    const uint64_t frequency = boot_stamps[SWITCH_STAMP_FREQUENCY], at = boot_stamps[SWITCH_STAMP_TIMERUS_AT];
    const uint32_t now = (uint32_t)boot_stamps[SWITCH_STAMP_TIMERUS];
    static const uint8_t record[SWITCH_BOOT_STAMPS_UBOOT_COUNT] = {
        SWITCH_STAMP_UBOOT_DISTRO, SWITCH_STAMP_UBOOT_SCRIPT,   SWITCH_STAMP_UBOOT_ARGS, SWITCH_STAMP_UBOOT_DTB,
        SWITCH_STAMP_UBOOT_WINDOW, SWITCH_STAMP_UBOOT_EXPORTER, SWITCH_STAMP_UBOOT_PREP, SWITCH_STAMP_UBOOT_GO,
        SWITCH_STAMP_UBOOT_MAIN,   SWITCH_STAMP_UBOOT_SD,
    };
    for (unsigned i = 0; i < SWITCH_BOOT_STAMPS_UBOOT_COUNT; ++i) {
        const uint32_t sample = slot[1 + i];
        const uint64_t back = (uint64_t)(uint32_t)(now - sample) * frequency / 1000000u;
        if (sample && back < at && at - back <= boot_stamps[SWITCH_STAMP_LOADER_ENTRY])
            boot_stamps[record[i]] = at - back;
    }
    slot[0] = 0;
}

/* The record into the ADT placeholder in the copied window (caches are off again: plain stores reach RAM). ADT values
 * are only 4-byte aligned, so 32-bit stores. A placeholder that is not zero is left alone. */
static void stamps_commit(void)
{
    volatile uint32_t *record = (volatile uint32_t *)(uintptr_t)(LOAD_ADT_PA + LOAD_STAMPS_ADT_OFFSET);
    for (unsigned i = 0; i < 2 * SWITCH_STAMP_COUNT; ++i)
        if (record[i])
            return;
    boot_stamps[SWITCH_STAMP_XNU_ENTRY] = ticks();
    for (unsigned i = 0; i < SWITCH_STAMP_COUNT; ++i) {
        record[2 * i] = (uint32_t)boot_stamps[i];
        record[2 * i + 1] = (uint32_t)(boot_stamps[i] >> 32);
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

#if !LOAD_FAST_COPY || LOAD_VERIFY_KERNEL
static void progress(unsigned row, uint64_t done, uint64_t total, uint64_t started)
{
    unsigned percent = (unsigned)(done * 100 / total);
    uint64_t elapsed = (ticks() - started) / 19200;
    char line[] = "000 PERCENT    000000 MS";
    for (unsigned i = 0; i < 3; ++i) {
        line[2 - i] = (char)('0' + percent % 10);
        percent /= 10;
    }
    if (elapsed > 999999)
        elapsed = 999999;
    for (unsigned i = 0; i < 6; ++i) {
        line[20 - i] = (char)('0' + elapsed % 10);
        elapsed /= 10;
    }
    display_text(500, row, line, DISPLAY_WHITE);
    loader_progress[2] = done;
}
#endif

#if LOAD_FAST_COPY
// Timing only (the record words, for a debugger): nothing is drawn on the way to the kernel.
static void copy_progress(uint64_t done, uint64_t total, uint64_t started)
{
    const uint64_t now = ticks(), elapsed = now - started;
    // The zero callback occurs after the temporary cache regime is enabled,
    // before the first payload load/store.
    if (!done)
        loader_copy_prepare_ticks = elapsed;
#if LOAD_STAMPS_ADT_OFFSET
    if (!done)
        boot_stamps[SWITCH_STAMP_UNPACK_START] = now;
    if (done == total)
        boot_stamps[SWITCH_STAMP_UNPACK_END] = now;
#endif
    (void)total;
    loader_progress[7] = elapsed;
    loader_progress[2] = done;
}
#endif

#if LOAD_VERIFY_KERNEL
static uint32_t checksum(uint64_t base, unsigned row)
{
    uint64_t started = ticks();
    uint32_t crc = UINT32_MAX;
    progress(row, 0, LOAD_BYTES, started);
    for (uint64_t offset = 0; offset < LOAD_BYTES;) {
        uint64_t bytes = LOAD_BYTES - offset;
        if (bytes > 0x100000)
            bytes = 0x100000;
        crc = loader_crc32_update(crc, (const volatile void *)(uintptr_t)(base + offset), bytes);
        offset += bytes;
        progress(row, offset, LOAD_BYTES, started);
    }
    loader_progress[row == 64 ? 3 : 5] = ticks() - started;
    return ~crc;
}
#endif

static void refuse(uint64_t reason) __attribute__((noreturn));

static void refuse(uint64_t reason)
{
    probe_record[R_FAILURE] = reason;
    display_text(16, 490, "LOADER REFUSED", DISPLAY_RED);
    char number[17];
    const char digits[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 16; ++i)
        number[i] = digits[(reason >> ((15 - i) * 4)) & 15];
    number[16] = 0;
    display_text(16, 520, "REFUSAL CODE", DISPLAY_WHITE);
    display_text(300, 520, number, DISPLAY_WHITE);
    probe_halt();
}

#if LOAD_FAST_COPY
void loader_copy_refuse(uint64_t reason) __attribute__((noreturn));

void loader_copy_refuse(uint64_t reason)
{
    refuse(reason);
}
#endif

#if LOAD_FAST_COPY
/* This build's window, whole, where hekate put it (stage.h): the header and trailer pages around the staged bytes
 * carry their sizes and the stream id this loader was built with. Read with the MMU off, after the FDT check. */
static uint64_t stage_check(void)
{
    const volatile uint32_t *head = (const volatile uint32_t *)(uintptr_t)SWITCH_STAGE_HEADER_PA;
    const volatile uint32_t *tail = (const volatile uint32_t *)(uintptr_t)(LOAD_SOURCE + LOAD_PACKED_BYTES);
    const uint32_t id_low = (uint32_t)LOAD_STAGE_ID, id_high = (uint32_t)(LOAD_STAGE_ID >> 32);
    if (head[0] != SWITCH_STAGE_MAGIC || head[1] != SWITCH_STAGE_HEAD || head[2] != SWITCH_STAGE_VERSION ||
        head[3] != LOAD_SOURCE - SWITCH_STAGE_HEADER_PA)
        return 0x6a0;
    if (head[4] != LOAD_STREAM_BYTES || head[5] != LOAD_PACKED_BYTES)
        return 0x6a1;
    if (head[6] != id_low || head[7] != id_high)
        return 0x6a2;
    if (tail[0] != SWITCH_STAGE_MAGIC || tail[1] != SWITCH_STAGE_TAIL || tail[2] != LOAD_STREAM_BYTES ||
        tail[3] != LOAD_PACKED_BYTES || tail[4] != id_low || tail[5] != id_high)
        return 0x6a3;
    return 0;
}
#endif

static uint64_t native_gate(void)
{
#if LOAD_NATIVE_GATES
    // U-Boot's inherited stack must be where hardware showed it before we switch to ours.
    // The full RAM profile later retires completed U-Boot allocations; live
    // FDT reservations, secure carveouts and the display remain protected.
    if (probe_record[R_ENTRY_SP] < UINT64_C(0xf0001000) || probe_record[R_ENTRY_SP] >= UINT64_C(0xffd00000))
        return 0x401;
    // Memory-controller carveout registers as read on hardware. Refuse a
    // changed carveout layout instead of inferring new free memory.
    static const uint32_t mc[][2] = {
        {0x70019050, 0x1000},     {0x70019670, 0},          {0x70019674, 0},          {0x700199d4, 0},
        {0x700199a0, 0xfff00000}, {0x700199a4, 0},          {0x700199a8, 0},          {0x70019648, 0},
        {0x7001964c, 0},          {0x70019978, 0},          {0x70019c0c, 0xffe00000}, {0x70019c10, 0},
        {0x70019c14, 8},          {0x70019c5c, 0xffd00000}, {0x70019c60, 0},          {0x70019c64, 2},
        {0x70019cac, 0xffd40000}, {0x70019cb0, 0},          {0x70019cb4, 6},          {0x70019cfc, 0},
        {0x70019d00, 0},          {0x70019d04, 0},          {0x70019d4c, 0},          {0x70019d50, 0},
        {0x70019d54, 0},
    };
    for (unsigned i = 0; i < sizeof(mc) / sizeof(mc[0]); ++i)
        if (*(const volatile uint32_t *)(uintptr_t)mc[i][0] != mc[i][1])
            return 0x410 + i;
    // Non-destructive register observation only: never read IAR or change
    // security groups. The kernel will check its accessible NS interface.
    const uint32_t addresses[] = {0x50041000, 0x50041004, 0x50041100, 0x50042000, 0x50042004, 0x500420fc};
    for (unsigned i = 0; i < 6; ++i)
        probe_record[L_GICD_CTLR + i] = *(const volatile uint32_t *)(uintptr_t)addresses[i];
#endif
    return 0;
}

void loader_after_transition(void)
{
    volatile uint64_t *r = probe_record;
#if LOAD_STAMPS_ADT_OFFSET
    stamps_entry();
#endif
    if (r[R_EL1_EL] != 4 || (r[R_EL1_SCTLR] & 0x1005) || (r[R_EL1_DAIF] & 0x3c0) != 0x3c0 ||
        r[R_EL1_CNTFRQ] != 19200000 || r[R_HCR_AFTER] != 0x80000000 || r[R_CNTHCTL_AFTER] != 3 || r[R_CNTVOFF_AFTER])
        refuse(0x301);
#if LOAD_EMC_KHZ
    {
        /* DRAM from hekate's 204 MHz to the kernel's EMC rate (emc.h) first: every check below reads RAM
         * uncached, one access at a time, and runs that much faster. It needs none of them (it touches only BPMP
         * registers and the BPMP's own channel area) and never refuses. */
        struct loader_emc_report emc;
        const uint64_t emc_start = ticks();
        loader_emc_raise((uint32_t)LOAD_EMC_KHZ, (uint32_t)r[R_EL1_CNTFRQ], &emc);
#if LOAD_STAMPS_ADT_OFFSET
        boot_stamps[SWITCH_STAMP_EMC_START] = emc_start;
        boot_stamps[SWITCH_STAMP_EMC_END] = ticks();
        boot_stamps[SWITCH_STAMP_EMC_RESULT] = emc.result | (uint64_t)(uint32_t)emc.bpmp_code << 32;
        boot_stamps[SWITCH_STAMP_EMC_RATES] = emc.before_khz | (uint64_t)emc.after_khz << 32;
        boot_stamps[SWITCH_STAMP_EMC_CORE] = emc.core_mv | (uint64_t)emc.area << 32;
#else
        (void)emc_start;
#endif
    }
#endif
    /* Every range the loader will touch, against the live FDT's reservations in one walk (memory.c): the walk reads
     * the FDT uncached, so one walk instead of one per range. The first failing range in this order sets the
     * refusal; a refusal of 0 only skips that range (the timing slot). */
    struct loader_memory_range ranges[LOADER_MEMORY_RANGES_MAX];
    uint64_t refusals[LOADER_MEMORY_RANGES_MAX];
    unsigned count = 0, index = 0;
    ranges[count] = (struct loader_memory_range){LOAD_DEST & ~UINT64_C(0x1fffff),
                                                 (LOAD_DEST + LOAD_WINDOW + 0x1fffff) & ~UINT64_C(0x1fffff)};
    refusals[count++] = 0x500;
#if LOAD_SD_REFERENCE
    // The reference uses the last page of our existing 128KiB adapter region.
    // The builder checks that every load segment and stack ends before it.
    // Do not access that page until the live FDT rejects any retained overlap.
    ranges[count] =
        (struct loader_memory_range){SWITCH_SD_REFERENCE_PA, SWITCH_SD_REFERENCE_PA + SWITCH_SD_REFERENCE_BYTES};
    refusals[count++] = 0x600;
#endif
#if LOAD_FAST_COPY
    /* Cacheable mappings cover only the exact staged bytes and loader image,
     * in addition to the destination envelope already checked above. Tables
     * are image-backed and included in __image_end, below the SD reference.
     * The staged window (in bl33.bin, stage.h) is checked with its header and
     * trailer pages, which are read before the copy. */
    extern char __image_end[];
    _Static_assert(LOAD_SOURCE == SWITCH_STAGE_PA && LOAD_SOURCE + LOAD_PACKED_BYTES + 0x1000 <= SWITCH_STAGE_END,
                   "staged window outside bl33.bin's reserved range");
    ranges[count] = (struct loader_memory_range){SWITCH_STAGE_HEADER_PA, LOAD_SOURCE + LOAD_PACKED_BYTES + 0x1000};
    refusals[count++] = 0x680;
    ranges[count] = (struct loader_memory_range){UINT64_C(0xa0000000), (uint64_t)(uintptr_t)__image_end};
    refusals[count++] = 0x6c0;
#endif
    ranges[count] = (struct loader_memory_range){SWITCH_BOOT_ARGS_PA, SWITCH_BOOT_ARGS_PA + SWITCH_BOOT_ARGS_BYTES};
    refusals[count++] = 0x740;
#if LOAD_STAMPS_ADT_OFFSET
    /* U-Boot's timing slot sits below the SD reference page, outside the loader image; read only if the live FDT
     * keeps nothing there. */
    ranges[count] =
        (struct loader_memory_range){SWITCH_BOOT_STAMPS_PA, SWITCH_BOOT_STAMPS_PA + SWITCH_BOOT_STAMPS_BYTES};
    refusals[count++] = 0;
#endif
    struct loader_memory_report memory;
    uint64_t failure = (uint64_t)loader_check_fdt_memory_ranges((const uint8_t *)(uintptr_t)r[R_FDT], 0x40000, ranges,
                                                                count, &memory, &index);
    r[L_MEMORY_CHECK] = index ? 0 : failure;
    r[L_RESERVATIONS] = memory.reservations;
    r[L_RESERVED_NODES] = memory.reserved_nodes;
    if (failure && refusals[index])
        refuse(refusals[index] + failure);
    const int slot_usable = !failure; /* a failure that did not refuse is the timing slot's own */
    (void)slot_usable;
#if LOAD_FAST_COPY
    failure = stage_check();
    if (failure)
        refuse(failure);
#endif
    failure = native_gate();
    if (failure)
        refuse(failure);
#if LOAD_STAMPS_ADT_OFFSET
    if (slot_usable)
        stamps_uboot();
    boot_stamps[SWITCH_STAMP_GATES] = ticks();
#endif
#if LOAD_VERIFY_KERNEL
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    // CRC32 is optional in Armv8.0; the physical OLED probe reports field=1.
    // Refuse unexpected CPUs before executing the optional instruction.
    if (((isar0 >> 16) & 15) != 1)
        refuse(0x305);
    loader_progress[0] = UINT64_C(0x5357434843524332);
    loader_progress[6] = isar0;
#else
    loader_progress[0] = UINT64_C(0x53574348434f5059);
#endif
    r[L_SOURCE] = LOAD_SOURCE;
    r[L_DEST] = LOAD_DEST;
    r[L_BYTES] = LOAD_BYTES;
    r[L_ARGS] = LOAD_ARGS;
    r[L_ENTRY] = LOAD_ENTRY;
    r[R_STAGE] = 0x50;
#if !LOAD_FAST_COPY
    display_clear();
    display_text(16, 24, "LOADER", DISPLAY_WHITE);
#endif
#if LOAD_VERIFY_KERNEL
    display_text(16, 64, "CHECK SOURCE", DISPLAY_WHITE);
    display_text(16, 144, "CHECK DESTINATION", DISPLAY_WHITE);
    loader_progress[1] = 1;
    r[L_SOURCE_CRC] = checksum(LOAD_SOURCE, 64);
    if (r[L_SOURCE_CRC] != LOAD_CRC)
        refuse(0x302);
#endif
#if !LOAD_FAST_COPY
    display_text(16, 104, "COPY KERNEL", DISPLAY_WHITE);
#endif
#if LOAD_SD_REFERENCE
    struct switch_sd_reference reference;
#if LOAD_SD_CARD
    struct switch_sd_card card;
    failure = (uint64_t)switch_sd_card_reference_validate((const volatile uint8_t *)(uintptr_t)SWITCH_SD_REFERENCE_PA,
                                                          SWITCH_SD_REFERENCE_BYTES, 0, &reference, &card);
#else
    failure = (uint64_t)switch_sd_reference_validate((const volatile uint8_t *)(uintptr_t)SWITCH_SD_REFERENCE_PA,
                                                     SWITCH_SD_REFERENCE_BYTES, 0, &reference);
#endif
    if (failure)
        refuse(0x640 + failure);
#endif
    r[R_STAGE] = 0x51;
    uint64_t started = ticks();
    loader_progress[1] = 2;
#if LOAD_FAST_COPY
    failure = loader_copy_run(copy_progress, started);
    if (failure)
        refuse(failure);
#else
    progress(104, 0, LOAD_BYTES, started);
    const volatile uint64_t *source = (const volatile uint64_t *)(uintptr_t)LOAD_SOURCE;
    volatile uint64_t *dest = (volatile uint64_t *)(uintptr_t)LOAD_DEST;
    for (uint64_t offset = 0; offset < LOAD_BYTES;) {
        uint64_t end = offset + 0x100000;
        if (end > LOAD_BYTES)
            end = LOAD_BYTES;
        for (uint64_t i = offset / 8; i < end / 8; ++i)
            dest[i] = source[i];
        offset = end;
        progress(104, offset, LOAD_BYTES, started);
    }
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    loader_progress[4] = ticks() - started;
#if LOAD_STAMPS_ADT_OFFSET
    boot_stamps[SWITCH_STAMP_COPY_DONE] = started + loader_progress[4];
#endif
    loader_progress[1] = 3;
#if LOAD_VERIFY_KERNEL
    r[L_DEST_CRC] = checksum(LOAD_DEST, 144);
    if (r[L_DEST_CRC] != LOAD_CRC)
        refuse(0x303);
#endif
    const volatile uint64_t *a = (const volatile uint64_t *)(uintptr_t)LOAD_ARGS;
    if (a[0] != 0x00020002 || a[1] != LOAD_BOOT_VIRT || a[2] != LOAD_BOOT_PHYS || a[3] != LOAD_BOOT_SIZE ||
        a[4] != LOAD_TOP || a[12] != LOAD_ADT_VA || a[0x478 / 8] != LOAD_BOOT_ACTUAL ||
        *(const volatile uint32_t *)(uintptr_t)(LOAD_ARGS + 0x68) != LOAD_ADT_BYTES)
        refuse(0x304);
    {
        int verbose = 0;
        const int result = switch_boot_args_append(
            (volatile char *)(uintptr_t)(LOAD_ARGS + SWITCH_BOOT_ARGS_COMMAND_LINE),
            (const volatile char *)(uintptr_t)SWITCH_BOOT_ARGS_PA, SWITCH_BOOT_ARGS_BYTES, &verbose);
        if (result)
            refuse(0x760 + (uint64_t)result);
#if SWITCH_VERBOSE_CONSOLE
        if (verbose)
            *(volatile uint64_t *)(uintptr_t)(LOAD_ARGS + SWITCH_BOOT_ARGS_VIDEO_DISPLAY) = 0;
#else
        (void)verbose;
#endif
    }
#if LOAD_FULL_RAM
    /* bootm has quiesced devices before cleanup. Refuse an active SD data
     * engine before retiring the old U-Boot heap/noncached pool. */
    if ((*(volatile uint32_t *)(uintptr_t)0x700b0024 & 0x307) || *(volatile uint8_t *)(uintptr_t)0x700b002f ||
        memory.bank_count != 2)
        refuse(0x710);
    if (memory.ledger.count + 1 > SWITCH_RAM_RANGES)
        refuse(0x711);
    memory.ledger.reserved[memory.ledger.count++] = (struct switch_ram_range){0xa0000000, 0xa0020000};
    memory.ledger.committed = 1;
    volatile uint8_t *ledger = (volatile uint8_t *)(uintptr_t)(LOAD_ADT_PA + LOAD_RAM_ADT_OFFSET);
    const uint8_t *wire = (const uint8_t *)&memory.ledger;
    for (unsigned i = 0; i < sizeof(memory.ledger); ++i) {
        if (ledger[i])
            refuse(0x712);
    }
    for (unsigned i = 0; i < sizeof(memory.ledger); ++i)
        ledger[i] = wire[i];
    __asm__ volatile("dsb sy" ::: "memory");
#endif
#if LOAD_SD_REFERENCE
    // Verify the immutable ADT pointer in the actual destination, then commit
    // only the external reference page. The staged/kernel image stays intact.
    const volatile uint8_t *sd_bytes =
        (const volatile uint8_t *)(uintptr_t)(LOAD_ADT_PA + LOAD_SD_REFERENCE_ADT_OFFSET);
    // AppleDT properties are only four-byte aligned; load individual bytes.
    uint64_t sd_address = 0, sd_size = 0;
    for (unsigned i = 0; i < 8; ++i) {
        sd_address |= (uint64_t)sd_bytes[i] << (8 * i);
        sd_size |= (uint64_t)sd_bytes[8 + i] << (8 * i);
    }
    if (sd_address != SWITCH_SD_REFERENCE_PA || sd_size != SWITCH_SD_REFERENCE_BYTES)
        refuse(0x650);
#if LOAD_SD_CARD
    failure = (uint64_t)switch_sd_card_reference_validate((const volatile uint8_t *)(uintptr_t)SWITCH_SD_REFERENCE_PA,
                                                          SWITCH_SD_REFERENCE_BYTES, 0, &reference, &card);
#else
    failure = (uint64_t)switch_sd_reference_validate((const volatile uint8_t *)(uintptr_t)SWITCH_SD_REFERENCE_PA,
                                                     SWITCH_SD_REFERENCE_BYTES, 0, &reference);
#endif
    if (failure)
        refuse(0x660 + failure);
    *(volatile uint32_t *)(uintptr_t)(SWITCH_SD_REFERENCE_PA + SWITCH_SD_REFERENCE_COMMIT_OFFSET) = 1;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    r[R_STAGE] = 0x52;
    loader_progress[1] = 4;
#if !LOAD_FAST_COPY
    display_text(16, 200, "COPY COMPLETE", DISPLAY_GREEN);
#endif
#if LOAD_SEED_ADT_OFFSET
    /* /chosen/random-seed from SE2's DRBG (seed.h), into the copied ADT; never refuses. 2 ms per wait. */
    (void)switch_seed_mix(LOAD_ADT_PA + LOAD_SEED_ADT_OFFSET, LOAD_ADT_PA + LOAD_SEED_SOURCE_ADT_OFFSET, ticks,
                          r[R_EL1_CNTFRQ] / 500u);
#endif
#if LOAD_STAMPS_ADT_OFFSET
    stamps_commit();
#endif
    loader_enter_kernel(LOAD_ARGS, LOAD_ENTRY);
}
