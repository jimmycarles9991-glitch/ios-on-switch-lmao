/* Read one SD sector after the verified own EL0 fixture. No recovery path. */
#include <stdint.h>
#include <stddef.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#if !SWITCH_SD_MODEL
#include <IOKit/IOLib.h>
#include <kern/misc_protos.h>
#endif
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "SwitchSD.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"
#include "SwitchFiles.h"
#include "storage/sdmmc_read.h"
#include "storage/sdmmc_dma.h"
#include "storage/sd_reference.h"
#ifndef SWITCH_SD_MODEL
#error Explicit native (0) or fake model (1) build required
#endif
#if SWITCH_SD_MODEL != 0 && SWITCH_SD_MODEL != 1
#error Invalid SD model selection
#endif
extern unsigned int real_ncpus;
/* magic,version,stage,error,model,attempts,cpus,ref-error,freq,ref-crc,crc,
 * transport-stage,status,response,words,blocked,mismatches,first-mismatch,
 * present,host-control,power,clock,reset,version,block-size,initial-status,
 * reference-pa,controller-pa,bounds-error,MBR-signature,ref-MBR-signature,mapped,readiness-failures */
volatile uint64_t switch_sd_record[33];
uint8_t switch_sd_bytes[512];
uint8_t switch_sd_reference_bytes[512];
static struct switch_sd_reference reference;
static struct switch_sd_card qualified_card;
static volatile uint64_t switch_card_record[16];
static unsigned attempted;

struct board_io {
    volatile uint8_t *registers;
    uint32_t bounds_error;
#if !SWITCH_SD_MODEL
    volatile uint8_t *mc;
    struct switch_sd_dma dma;
#endif
#if SWITCH_SD_MODEL
    uint32_t mode, phase, words, commands, writes;
    uint32_t reg[128];
    uint64_t ticks;
    const uint8_t *image, *sector;
    uint32_t image_size, image_count;
#endif
};
static struct board_io board;

#if !SWITCH_SD_MODEL
static uint8_t r8(void *context, uint32_t offset)
{
    struct board_io *b = context;
    if (offset >= 0x200) {
        b->bounds_error = 1;
        return 0;
    }
    return switch_mmio_read8(b->registers, offset);
}

static uint16_t r16(void *context, uint32_t offset)
{
    struct board_io *b = context;
    if (offset > 0x1fe || (offset & 1)) {
        b->bounds_error = 1;
        return 0;
    }
    return switch_mmio_read16(b->registers, offset);
}

static uint32_t r32(void *context, uint32_t offset)
{
    struct board_io *b = context;
    /* The native boot path must never fall back to CPU FIFO transfers. */
    if (offset > 0x1fc || (offset & 3) || offset == 0x20) {
        b->bounds_error = 1;
        return 0;
    }
    return switch_mmio_read32(b->registers, offset);
}

static void w16(void *context, uint32_t offset, uint16_t value)
{
    struct board_io *b = context;
    if (offset == 0x3e) {
        if (value != (uint16_t)(r16(context, offset) | 0x3000)) {
            b->bounds_error = 1;
            return;
        }
    } else if (offset != 4 && offset != 6 && offset != 0x0c && offset != 0x0e) {
        b->bounds_error = 1;
        return;
    }
    /* Existing reads, status/SCR/cache qualification, and bounded writes only.
     * No erase, cache-control CMD49, card selection, reset or tuning command. */
    if (offset == 0x0e && value != 0x113a && value != 0x123a && value != 0x0d1a && value != 0x371a && value != 0x333a &&
        value != 0x303a && value != 0x183a && value != 0x193a) {
        b->bounds_error = 1;
        return;
    }
    switch_mmio_write16(b->registers, offset, value);
}

static void w32(void *context, uint32_t offset, uint32_t value)
{
    struct board_io *b = context;
    if (offset != 8 && offset != 0x30 && offset != 0x34 && offset != 0x38 && offset != 0x58 && offset != 0x5c) {
        b->bounds_error = 1;
        return;
    }
    switch_mmio_write32(b->registers, offset, value);
}

static uint64_t ticks(void *context)
{
    uint64_t value = 0;
    (void)context;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(value)::"memory");
    return value;
}

/* Physical-address DMA requires the readable SDMMC1A per-SWGROUP bypass.
 * Tegra210 global config/client masks are TrustZone-only and can read all ones
 * at non-secure EL1. Linux/Hekate use ASID MC+0xa94 bit31 for group translation;
 * its only other defined field is the 7-bit ASID. Verify this mapped MC window
 * against the photographed OLED's 4096 MiB EMEM_CFG, then recheck every phase.
 * Never modify the SMMU or another client's mapping. */
static volatile uint64_t switch_dma_record[13];

static int dma_bus_ready(void *context)
{
    struct board_io *b = context;
    if (b != &board || !b->mc)
        return -1;
    uint32_t config = switch_mmio_read32(b->mc, 0x10);
    uint32_t asid = switch_mmio_read32(b->mc, 0xa94);
    uint32_t clients = switch_mmio_read32(b->mc, 0x234);
    uint32_t emem = switch_mmio_read32(b->mc, 0x50);
    switch_dma_record[5] = config;
    switch_dma_record[6] = asid;
    switch_dma_record[7] = clients;
    switch_dma_record[12] = emem;
    /* Global state is diagnostic only. Disabled group translation establishes
     * identity independently of the secure global enable/client registers. */
    return emem != 4096u || (asid & ~0x7fu) ? -1 : 0;
}

static void dma_write8(void *context, uint32_t offset, uint8_t value)
{
    struct board_io *b = context;
    if (b != &board || offset != 0x28 || value != (uint8_t)(r8(context, offset) & ~0x18u)) {
        b->bounds_error = 1;
        return;
    }
    switch_mmio_write8(b->registers, offset, value);
}

static int dma_cache(void *context, void *address, uint32_t length, int after_read)
{
    struct board_io *b = context;
    if (b != &board || address != b->dma.bytes || !length || length > SWITCH_SD_DMA_BYTES ||
        ((uintptr_t)address & 63) || (after_read != 0 && after_read != 1))
        return -1;
    const size_t lines = (length + SWITCH_DCACHE_LINE - 1) & ~(size_t)(SWITCH_DCACHE_LINE - 1);
    if (after_read) {
        switch_dcache_invalidate(address, lines);
    } else {
        switch_dcache_clean_invalidate(address, lines);
    }
    return 0;
}

static int dma_install(struct switch_sd_host *host)
{
    struct switch_sd_dma *d = &board.dma;
    host->dma_required = 1; /* No native PIO even when allocation/qualification fails. */
    switch_dma_record[0] = UINT64_C(0x53574348444d4131);
    switch_dma_record[1] = 1;
    int status = SWITCH_SD_UNQUALIFIED;
    switch_dma_record[8] = r16(&board, 0xfe);
    switch_dma_record[9] = r32(&board, 0x40);
    switch_dma_record[10] = r8(&board, 0x28);
    switch_dma_record[11] = r16(&board, 0x3e);
    uint64_t midr = 0;
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0,midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0,ctr_el0" : "=r"(ctr));
    if ((midr & UINT64_C(0xff0ffff0)) != UINT64_C(0x410fd070) || ((ctr >> 16) & 15) != 4)
        goto failed;
    switch_dma_record[1] = 2;
    board.mc = (volatile uint8_t *)ml_io_map((vm_offset_t)UINT64_C(0x70019000), 0x1000);
    if (!board.mc || dma_bus_ready(&board))
        goto failed;
    switch_dma_record[1] = 3;
    /* This C entry point delegates to XNU's actual contiguous allocator.
     * Its deprecated physical-address argument is deliberately unused. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    d->bytes = IOMallocContiguous(SWITCH_SD_DMA_BYTES, SWITCH_SD_DMA_BYTES, NULL);
#pragma clang diagnostic pop
    if (!d->bytes)
        goto failed;
    switch_dma_record[1] = 4;
    d->capacity = SWITCH_SD_DMA_BYTES;
    d->physical = ml_vtophys((vm_offset_t)d->bytes);
    switch_dma_record[3] = d->physical;
    switch_dma_record[4] = d->capacity;
    /* VM supplied a wired physically contiguous allocation. Independently
     * verify every 4 KiB page and final byte; never assume CPU VA is bus PA. */
    if (!d->physical || (d->physical & (SWITCH_SD_DMA_BYTES - 1)) ||
        d->physical > UINT64_C(0x1000000000) - SWITCH_SD_DMA_BYTES)
        goto failed;
    for (uint32_t at = 0; at < SWITCH_SD_DMA_BYTES; at += 4096) {
        if (ml_vtophys((vm_offset_t)(d->bytes + at)) != d->physical + at)
            goto failed;
    }
    if (ml_vtophys((vm_offset_t)(d->bytes + SWITCH_SD_DMA_BYTES - 1)) != d->physical + SWITCH_SD_DMA_BYTES - 1)
        goto failed;
    d->write8 = dma_write8;
    d->bus_ready = dma_bus_ready;
    d->cache = dma_cache;
    switch_dma_record[1] = 5;
    status = switch_sd_dma_bind(d, &host->io, &board, host->frequency);
    if (status)
        goto failed;
    host->dma = d;
    switch_dma_record[1] = 6;
    switch_dma_record[2] = 0;
    return SWITCH_SD_OK;
failed:
    host->blocked = 1;
    d->failed = 1;
    switch_dma_record[2] = (unsigned)status;
    switch_dma_record[8] = r16(&board, 0xfe);
    switch_dma_record[9] = r32(&board, 0x40);
    switch_dma_record[10] = r8(&board, 0x28);
    switch_dma_record[11] = r16(&board, 0x3e);
    return status;
}
#else
volatile uint64_t switch_sd_model_record[8];
/* Development-only debugger mailbox. Mode 101 pauses at the checkpoint so
 * the host can fill one bounded batch from real local image files. Native
 * builds contain none of these buffers, symbols, calls or I/O substitutions. */
volatile uint64_t switch_sd_bridge_record[8];
uint8_t switch_sd_model_cache[65536] __attribute__((aligned(4096)));
static uint64_t model_cache_lba = UINT64_MAX;
static uint64_t model_cache_count;

__attribute__((noinline)) void switch_sd_model_request_checkpoint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

static const uint8_t *model_bridge_sector(uint64_t lba)
{
    if (model_cache_lba == UINT64_MAX || lba < model_cache_lba || lba - model_cache_lba >= model_cache_count) {
        uint64_t base = lba & ~UINT64_C(127), count = 128;
        if (base >= qualified_card.capacity_sectors)
            return 0;
        if (count > qualified_card.capacity_sectors - base)
            count = qualified_card.capacity_sectors - base;
        switch_sd_bridge_record[0] = UINT64_C(0x5357434842524447);
        uint64_t sequence = switch_sd_bridge_record[1] + 1;
        if (!sequence)
            return 0;
        switch_sd_bridge_record[1] = sequence;
        switch_sd_bridge_record[2] = base;
        switch_sd_bridge_record[3] = count;
        switch_sd_bridge_record[4] = 0;
        switch_sd_bridge_record[5] = 0;
        switch_sd_bridge_record[6] = 0;
        switch_sd_bridge_record[7] = 0;
        model_cache_lba = UINT64_MAX;
        model_cache_count = 0;
        switch_sd_model_request_checkpoint();
        /* No debugger or an incomplete/error reply fails immediately. */
        if (switch_sd_bridge_record[4] != sequence || switch_sd_bridge_record[5] != 1 ||
            switch_sd_bridge_record[6] != switch_sd_crc32(switch_sd_model_cache, (uint32_t)count * 512) ||
            switch_sd_bridge_record[7])
            return 0;
        model_cache_lba = base;
        model_cache_count = count;
    }
    return switch_sd_model_cache + (lba - model_cache_lba) * 512;
}

static uint32_t model_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t model_u64(const uint8_t *p)
{
    return (uint64_t)model_u32(p) | (uint64_t)model_u32(p + 4) << 32;
}

static int model_image_init(struct board_io *b)
{
    const uint8_t *image = (const uint8_t *)ml_io_map_with_prot(UINT64_C(0xe8000000), 4096, VM_PROT_READ);
    static const uint8_t magic[8] = {'S', 'W', 'C', 'H', 'B', 'L', 'K', '1'};
    if (!image)
        return 0;
    for (unsigned i = 0; i < 8; i++)
        if (image[i] != magic[i])
            return 0;
    uint32_t count = model_u32(image + 12), offset = model_u32(image + 24);
    uint64_t size = model_u64(image + 40), capacity = model_u64(image + 32);
    if (model_u32(image + 8) != 1 || !count || count > 30000 || model_u32(image + 16) != 64 ||
        model_u32(image + 20) != 16 || model_u32(image + 28) || model_u64(image + 48) || model_u64(image + 56) ||
        offset < 64 + count * 16 || (offset & 4095) || size > 16u << 20 || size != offset + (uint64_t)count * 512 ||
        capacity != qualified_card.capacity_sectors)
        return 0;
    /* Model-only media sits outside managed DRAM. Map just the checked
     * page-rounded size instead of reserving the full upper bound. */
    image = (const uint8_t *)ml_io_map_with_prot(UINT64_C(0xe8000000), (vm_size_t)((size + 4095) & ~UINT64_C(4095)),
                                                 VM_PROT_READ);
    if (!image)
        return 0;
    uint64_t previous = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = image + 64 + i * 16;
        uint64_t lba = model_u64(entry);
        if (lba >= capacity || (i && lba <= previous) || model_u32(entry + 8) != offset + i * 512)
            return 0;
        previous = lba;
    }
    b->image = image;
    b->image_size = (uint32_t)size;
    b->image_count = count;
    switch_sd_model_record[4] = capacity;
    switch_sd_model_record[5] = size;
    switch_sd_model_record[7] = count;
    return 1;
}

static const uint8_t *model_sector(struct board_io *b, uint32_t lba)
{
    if (b->mode == 101) {
        switch_sd_model_record[6]++;
        const uint8_t *sector = model_bridge_sector(lba);
        if (!sector)
            switch_sd_model_record[3] = lba;
        return sector;
    }
    uint32_t low = 0, high = b->image_count;
    switch_sd_model_record[6]++;
    while (low < high) {
        uint32_t at = low + (high - low) / 2;
        uint64_t candidate = model_u64(b->image + 64 + at * 16);
        if (candidate < lba)
            low = at + 1;
        else
            high = at;
    }
    if (low >= b->image_count || model_u64(b->image + 64 + low * 16) != lba) {
        switch_sd_model_record[3] = lba;
        return 0;
    }
    const uint8_t *entry = b->image + 64 + low * 16;
    uint32_t offset = model_u32(entry + 8);
    if (offset > b->image_size || b->image_size - offset < 512)
        return 0;
    const uint8_t *sector = b->image + offset;
    if (switch_sd_crc32(sector, 512) != model_u32(entry + 12))
        return 0;
    return sector;
}

/* Compile-time model adapter. No physical MMIO dereference exists in this
 * branch. Its sole data source is the checked reference page in model RAM. */
static uint8_t r8(void *context, uint32_t offset)
{
    struct board_io *b = context;
    if (offset == 0x28)
        return 2;
    if (offset == 0x29)
        return b->mode == 7 ? 0 : b->mode >= 9 ? 15 : 11;
    if (offset == 0x2f)
        return b->mode == 8 ? 1 : 0;
    b->bounds_error = 1;
    return 0;
}

static uint16_t r16(void *context, uint32_t offset)
{
    struct board_io *b = context;
    if (offset == 0xfe)
        return b->mode >= 100  ? 0x0303
               : b->mode == 6  ? 1
               : b->mode == 9  ? 0x0303
               : b->mode == 10 ? 0x0304
               : b->mode == 11 ? 0x0300
               : b->mode == 12 ? 0x03ff
                               : 2;
    if (offset == 0x2c)
        return 7;
    if (offset == 4 && b->mode == 2)
        return 513;
    if (offset == 4 || offset == 6 || offset == 12)
        return (uint16_t)b->reg[offset / 2];
    b->bounds_error = 1;
    return 0;
}

static uint32_t r32(void *context, uint32_t offset)
{
    struct board_io *b = context;
    if (offset == 0x24)
        return 0x1fb0000u | (b->phase == 2 ? 0x800u : 0);
    if (offset == 0x30) {
        if (b->phase == 1)
            return b->mode == 3 ? 0x20001u : b->mode == 4 ? 0 : 1;
        if (b->phase == 2)
            return 0x20;
        if (b->phase == 3)
            return 2;
        return 0;
    }
    if (offset == 0x10)
        return 0x900;
    if (offset == 0x20 && b->phase == 2 && b->words < 128) {
        uint32_t v = 0, at = b->words++ * 4;
        for (unsigned i = 0; i < 4; ++i) {
            uint8_t octet = b->mode >= 100 ? (b->sector ? b->sector[at + i] : 0) : reference.sector[at + i];
            if (b->mode == 5 && at + i == 7)
                octet ^= 1;
            v |= (uint32_t)octet << (8 * i);
        }
        switch_sd_model_record[1]++;
        return v;
    }
    if (offset == 8 || offset == 0x34 || offset == 0x38)
        return b->reg[offset / 2];
    b->bounds_error = 1;
    return 0;
}

static void w16(void *context, uint32_t offset, uint16_t value)
{
    struct board_io *b = context;
    ++b->writes;
    if (offset == 0x0e) {
        if (value != 0x113a || b->phase || b->reg[4 / 2] != 512 || b->reg[6 / 2] != 1 || b->reg[12 / 2] != 0x12 ||
            (b->mode < 100 && b->reg[8 / 2]) || b->reg[0x38 / 2]) {
            b->bounds_error = 1;
            return;
        }
        if (b->mode >= 100) {
            b->sector = model_sector(b, b->reg[8 / 2]);
            if (!b->sector) {
                b->bounds_error = 1;
                return;
            }
        }
        ++b->commands;
        b->phase = 1;
        switch_sd_model_record[0]++;
        switch_sd_model_record[2] = b->reg[8 / 2];
        return;
    }
    if (offset == 4 || offset == 6 || offset == 12) {
        b->reg[offset / 2] = value;
        return;
    }
    b->bounds_error = 1;
}

static void w32(void *context, uint32_t offset, uint32_t value)
{
    struct board_io *b = context;
    ++b->writes;
    if (offset == 0x30) {
        if (!b->phase && value == 0x23)
            return;
        if (b->phase == 3 && value == 2) {
            b->phase = 4;
            return;
        }
        if (b->phase == 1 && value == 1) {
            b->phase = 2;
            return;
        }
        if (b->phase == 2 && value == 0x20 && b->words == 128) {
            b->phase = 3;
            return;
        }
        b->bounds_error = 1;
        return;
    }
    if (offset == 8 || offset == 0x34 || offset == 0x38) {
        b->reg[offset / 2] = value;
        if (offset == 0x38 && b->phase == 4) {
            b->phase = 0;
            b->words = 0;
        }
        return;
    }
    b->bounds_error = 1;
}

static uint64_t ticks(void *context)
{
    struct board_io *b = context;
    if (b->mode >= 100) {
        uint64_t value;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(value)::"memory");
        return value;
    }
    b->ticks += 50;
    return b->ticks;
}
#endif
static void barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void refuse(uint64_t error)
{
    switch_sd_record[3] = error;
}

static int native_sd_cpu_profile_matches(int cpus, int root_boot)
{
    if (cpus == 1)
        return __atomic_load_n(&real_ncpus, __ATOMIC_ACQUIRE) == 1;
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT && !SWITCH_SD_MODEL
    if (!root_boot || !switch_native_cpu_profile_matches(cpus))
        return 0;
    /* Registration is asynchronous matching, not scheduler-online evidence.
     * Yield so the remaining CPU nubs can register; the driver owns online proof. */
    for (unsigned waited = 0; waited <= 2000; ++waited) {
        unsigned registered = __atomic_load_n(&real_ncpus, __ATOMIC_ACQUIRE);
        if (registered == 4)
            return 1;
        if (!registered || registered > 4 || waited == 2000)
            return 0;
        IOSleep(10);
    }
#else
    (void)root_boot;
#endif
    return 0;
}

void switch_sd_probe(void)
{
    if (attempted)
        return;
    attempted = 1;
    switch_sd_record[0] = SWITCH_SD_DIAG_MAGIC;
    switch_sd_record[1] = 1;
    switch_sd_record[2] = 1;
    switch_sd_record[4] = SWITCH_SD_MODEL;
    switch_sd_record[5] = 1;
    switch_sd_record[6] = real_ncpus;
    switch_sd_record[17] = UINT64_MAX;
    switch_sd_record[26] = SWITCH_SD_REFERENCE_PA;
    switch_sd_record[27] = SWITCH_SD_CONTROLLER_PA;
    const int cpus = ml_get_max_cpu_number() + 1;
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
    const int root_boot = 1;
#else
    const int root_boot = 0;
#endif
    if (!native_sd_cpu_profile_matches(cpus, root_boot) ||
        (!root_boot && (switch_user_record[2] != 3 || switch_user_record[3] || switch_user_record[6] != 1 ||
                        switch_user_record[8] != 1 || switch_user_record[9] ||
                        switch_user_record[12] != (uint64_t)PAGE_SIZE || switch_user_record[13] != 4))) {
        refuse(100);
        return;
    }
    const int files = 1;
    DTEntry chosen = NULL;
    const void *property = NULL;
    unsigned length = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "switch-sd-reference", &property, &length) != kSuccess || length != 16 ||
        !property || le64(property) != SWITCH_SD_REFERENCE_PA ||
        le64((const uint8_t *)property + 8) != SWITCH_SD_REFERENCE_BYTES) {
        refuse(101);
        return;
    }
    /* The reference lies below managed physBase. Use a read-only explicit map,
     * never phystokv. Loader has committed the checked external page. */
    vm_offset_t mapped =
        ml_io_map_with_prot((vm_offset_t)SWITCH_SD_REFERENCE_PA, SWITCH_SD_REFERENCE_BYTES, VM_PROT_READ);
    if (!mapped) {
        refuse(102);
        return;
    }
    barrier(NULL);
    int reference_error =
        files
            ? switch_sd_card_reference_validate((const volatile uint8_t *)mapped, SWITCH_SD_REFERENCE_BYTES, 1,
                                                &reference, &qualified_card)
            : switch_sd_reference_validate((const volatile uint8_t *)mapped, SWITCH_SD_REFERENCE_BYTES, 1, &reference);
    switch_sd_record[7] = (uint64_t)reference_error;
    if (reference_error) {
        refuse(110 + (unsigned)reference_error);
        return;
    }
    if (files) {
        switch_card_record[0] = UINT64_C(0x5357434843415244);
        switch_card_record[1] = 1;
        switch_card_record[2] = qualified_card.capacity_sectors;
        switch_card_record[3] = qualified_card.capacity_bytes;
        switch_card_record[4] = qualified_card.ocr;
        switch_card_record[5] = qualified_card.rca;
        switch_card_record[6] = qualified_card.version;
        for (unsigned i = 0; i < 4; i++) {
            switch_card_record[7 + i] = qualified_card.csd[i];
            switch_card_record[11 + i] = qualified_card.cid[i];
        }
    }
    for (unsigned i = 0; i < 512; ++i)
        switch_sd_reference_bytes[i] = reference.sector[i];
    switch_sd_record[9] = reference.crc32;
    switch_sd_record[30] = (uint32_t)reference.sector[510] | (uint32_t)reference.sector[511] << 8;
    switch_sd_record[2] = 2;
    uint64_t frequency = 0;
#if SWITCH_SD_MODEL
#ifndef SWITCH_SD_MODEL_MODE
#define SWITCH_SD_MODEL_MODE 1
#endif
    const int mode = SWITCH_SD_MODEL_MODE;
    if (mode < 1 || (mode > 12 && mode != 100 && mode != 101)) {
        refuse(120);
        return;
    }
    board.mode = (unsigned)mode;
    board.reg[4 / 2] = 0x7200;
    board.reg[6 / 2] = 1;
    board.reg[0x34 / 2] = 0x8003;
    board.reg[0x38 / 2] = 0x8002;
    frequency = 1000;
    if (mode >= 100) {
        if (!files || !model_image_init(&board)) {
            refuse(125);
            return;
        }
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    } else if (files) {
        refuse(126);
        return;
    }
#else
    /* ml_io_map uses VM_WIMG_IO. The pinned pmap maps this non-DRAM PA as
     * Device-nGnRnE and NX, a whole VM page of it; callbacks constrain all
     * accesses to this controller's first0x200 and never touch eMMC at+0x600. */
    mapped = ml_io_map((vm_offset_t)SWITCH_SD_CONTROLLER_PA, 0x200);
    if (!mapped) {
        refuse(121);
        return;
    }
    board.registers = (volatile uint8_t *)mapped;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
#endif
    switch_sd_record[31] = 1;
    switch_sd_record[2] = 3;
    switch_sd_record[8] = frequency;
    static struct switch_sd_host host;
    host = (struct switch_sd_host){.io = {r8, r16, r32, w16, w32, ticks, barrier},
                                   .context = &board,
                                   .frequency = frequency,
                                   .capacity_sectors = 1,
                                   .permitted_first_sector = 0,
                                   .permitted_sector_count = 1,
                                   .block_addressing_verified = 0};
#if !SWITCH_SD_MODEL
    int dma_error = dma_install(&host);
    if (dma_error) {
        printf("native SD DMA init failed: %d stage=%llu version=%llx caps=%llx hc=%llx hc2=%llx MC=%llx ASID=%llx "
               "clients=%llx EMEM=%llx\n",
               dma_error, switch_dma_record[1], switch_dma_record[8], switch_dma_record[9], switch_dma_record[10],
               switch_dma_record[11], switch_dma_record[5], switch_dma_record[6], switch_dma_record[7],
               switch_dma_record[12]);
        refuse(128);
        return;
    }
#endif
    struct switch_sd_result result;
    /* No other SD driver exists before this serialized owner is published. Keep CPU interrupt
     * dispatch out of the complete polling operation; transport masks SD IRQs. */
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    barrier(&board);
    int error = switch_sd_read_sector(&host, 0, switch_sd_bytes, &result);
    switch_sd_record[18] = result.present;
    switch_sd_record[19] = result.host_control;
    switch_sd_record[20] = result.power;
    switch_sd_record[21] = result.clock;
    switch_sd_record[22] = result.reset;
    switch_sd_record[23] = result.version;
    switch_sd_record[24] = result.block_size;
    switch_sd_record[25] = result.initial_status;
    switch_sd_record[32] = result.readiness_failures;
    barrier(&board);
    switch_sd_record[11] = result.stage;
    switch_sd_record[12] = result.status;
    switch_sd_record[13] = result.response;
    switch_sd_record[14] = result.words_read;
    switch_sd_record[15] = host.blocked;
    switch_sd_record[28] = board.bounds_error;
    switch_sd_record[2] = 4;
    switch_sd_record[3] = (unsigned)error;
    if (board.bounds_error)
        switch_sd_record[3] = 122;
    (void)ml_set_interrupts_enabled(previous);
    if (switch_sd_record[3])
        return;
    switch_sd_record[10] = switch_sd_crc32(switch_sd_bytes, 512);
    switch_sd_record[29] = (uint32_t)switch_sd_bytes[510] | (uint32_t)switch_sd_bytes[511] << 8;
    for (unsigned i = 0; i < 512; ++i) {
        if (switch_sd_bytes[i] != reference.sector[i]) {
            if (!switch_sd_record[16])
                switch_sd_record[17] = i;
            ++switch_sd_record[16];
        }
    }
    if (switch_sd_record[16] || switch_sd_record[10] != reference.crc32) {
        refuse(123);
        return;
    }
    switch_sd_record[2] = 5;
    if (files) {
        host.capacity_sectors = qualified_card.capacity_sectors;
        host.permitted_first_sector = 0;
        host.permitted_sector_count = qualified_card.capacity_sectors;
        host.block_addressing_verified = 1;
        host.rca = qualified_card.rca;
        host.card_write_protected = (qualified_card.csd[3] & 0x3000u) != 0;
        switch_files_probe(&host);
    }
}
