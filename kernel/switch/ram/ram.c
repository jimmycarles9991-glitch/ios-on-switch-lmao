// SPDX-License-Identifier: GPL-2.0-only
/*
 * The console's RAM (<switch/ram.h>): the bootloader's ledger (boot/loader/ram.h) of the two DRAM banks and the ranges
 * other owners keep, handed over in the device tree's /chosen/switch-ram-ledger.
 *
 * The physical aperture stays dense: ARM64's page metadata is indexed by span, so vm_pages covers every page from
 * gPhysBase, and the holes (reservations, the carveouts between the banks, the GPU pool) are descriptors the VM never
 * frees.
 */
#include <kern/assert.h>
#include <kern/misc_protos.h>
#include <mach/vm_param.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/device_tree.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>

#include <switch/boot_layout.h>
#include <switch/ram.h>

extern vm_offset_t first_avail;
extern vm_offset_t virtual_space_start;
extern pmap_paddr_t avail_start, avail_end;
#if XNU_10063_141_1
extern uint_t vm_delayed_count;
#endif

static struct switch_ram_ledger switch_ledger;
static pmap_paddr_t switch_kernel_start, switch_bootstrap_start, switch_bootstrap_limit, switch_accounted_bootstrap_end;
/*
 * The GPU pool: outside the managed pages like the framebuffer (no pv lists, never handed to the VM), and in the
 * physical aperture write-combined only (no cacheable alias anywhere): the GPU driver reaches it with phystokv(). A
 * separate 128 MiB ml_io_map_wcomb fails, kernel VA (10-30: "kmem_alloc(.., 134217728, ..): failed with 3").
 */
static pmap_paddr_t switch_gpu_start, switch_gpu_end;
/* Set only while SWITCH_RAM_PHYSMAP_SLIDE maps the GPU pool. */
bool switch_physmap_wcomb;
bool switch_ram_committed, switch_dense_pages;
static pmap_paddr_t switch_stolen_end;
static unsigned int switch_reserved_pages;

boolean_t switch_ram_enabled(void)
{
    return switch_ledger.committed == 1;
}

boolean_t switch_ram_gpu_pool(pmap_paddr_t *start, pmap_paddr_t *end)
{
    *start = switch_gpu_start;
    *end = switch_gpu_end;
    return switch_gpu_start < switch_gpu_end;
}

bool switch_ram_pa_valid_high(pmap_paddr_t pa)
{
    return pa >= SWITCH_RAM_GPU_POOL_END && !switch_ram_in(pa, SWITCH_RAM_FB_START, SWITCH_RAM_FB_END) &&
           pa < SWITCH_RAM_BANK0_END;
}

boolean_t switch_ram_reserved(ppnum_t pnum)
{
    pmap_paddr_t pa = (pmap_paddr_t)pnum << PAGE_SHIFT;
    if (!switch_ledger.committed) {
        return FALSE;
    }
    if (!switch_ram_in(pa, SWITCH_RAM_BANK0_START, SWITCH_RAM_BANK1_END) || !switch_ram_pa_valid(pa)) {
        return TRUE;
    }
    if (pa < first_avail_phys && pa + PAGE_SIZE > switch_kernel_start) {
        return TRUE;
    }
    for (unsigned i = 0; i < switch_ledger.count; ++i) {
        if (pa < switch_ledger.reserved[i].end && pa + PAGE_SIZE > switch_ledger.reserved[i].start) {
            return TRUE;
        }
    }
    return FALSE;
}

void switch_ram_bootstrap_check(pmap_paddr_t start, pmap_paddr_t end)
{
    if (switch_ledger.committed && (start < switch_bootstrap_start || end < start || end > switch_bootstrap_limit)) {
        panic("native bootstrap allocation outside contiguous free RAM");
    }
}

/*
 * drivers/TegraGM20B's GPU pool (nvxnu_pool_init, through switch_ram_gpu_pool): 256 MiB, every process's coherent GPU
 * memory and nvkm's own (128 starved poster extensions on 11-04 with a dozen Metal processes, of 3.9 GB). It joins the
 * ledger here, so the VM never hands it out and the GPU driver maps it write-combined. A physically contiguous 128 MiB
 * below 4 GiB cannot be allocated once the VM is running (10-30: "M1 FAIL pool -12"). It lies in the low bank's free
 * RAM, above the bootstrap blocks (0xe2000000) and below the framebuffer (0xf5a00000).
 */
static void switch_ram_reserve_gpu_pool(void)
{
    const uint64_t gpu_pa = SWITCH_RAM_GPU_POOL_START;
    const uint64_t gpu_end = SWITCH_RAM_GPU_POOL_END;
    if (switch_ledger.count == SWITCH_RAM_RANGES) {
        panic("the GPU pool %#llx-%#llx does not fit: the RAM ledger is full", gpu_pa, gpu_end);
    }
    /*
     * The ledger also holds the live FDT's /reserved-memory and memreserve ranges (loader memory.c), which the build
     * does not know: RAM another owner keeps must not become the GPU's.
     */
    for (unsigned i = 0; i < switch_ledger.count; ++i) {
        if (gpu_pa < switch_ledger.reserved[i].end && switch_ledger.reserved[i].start < gpu_end) {
            panic("the GPU pool %#llx-%#llx overlaps reserved RAM %#llx-%#llx (ledger entry %u)", gpu_pa, gpu_end,
                  (uint64_t)switch_ledger.reserved[i].start, (uint64_t)switch_ledger.reserved[i].end, i);
        }
    }
    switch_ledger.reserved[switch_ledger.count++] = (struct switch_ram_range){gpu_pa, gpu_end};
    switch_gpu_start = gpu_pa;
    switch_gpu_end = gpu_end;
}

void switch_ram_init(struct boot_args *args)
{
    DTEntry entry = NULL;
    const void *wire = NULL;
    unsigned int bytes = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "switch-ram-ledger", &wire, &bytes) != kSuccess) {
        if (args->physBase == SWITCH_BOOT_PHYS_BASE && args->memSize == SWITCH_BOOT_MEM_SIZE) {
            panic("native full RAM requires live ownership ledger");
        }
        return;
    }
    if (bytes != sizeof(switch_ledger)) {
        panic("native RAM ledger size");
    }
    memcpy(&switch_ledger, wire, sizeof(switch_ledger));
    if (switch_ledger.magic != SWITCH_RAM_MAGIC || switch_ledger.version != 1 || switch_ledger.committed != 1 ||
        !switch_ledger.count || switch_ledger.count > SWITCH_RAM_RANGES ||
        switch_ledger.banks[0].start != SWITCH_RAM_BANK0_START || switch_ledger.banks[0].end != SWITCH_RAM_BANK0_END ||
        switch_ledger.banks[1].start != SWITCH_RAM_BANK1_START || switch_ledger.banks[1].end != SWITCH_RAM_BANK1_END ||
        args->physBase != SWITCH_BOOT_PHYS_BASE || args->memSize != SWITCH_BOOT_MEM_SIZE ||
        args->virtBase != SWITCH_BOOT_VIRT_BASE) {
        panic("native RAM ledger/bank geometry");
    }
    switch_kernel_start = SWITCH_KERNEL_PHYS_BASE;
    switch_bootstrap_start = args->topOfKernelData;
    /* Temporary assembly blocks end at the old envelope rounded down. */
    switch_bootstrap_limit = 0xe2000000ULL;
    if (switch_bootstrap_start <= switch_kernel_start || switch_bootstrap_start >= switch_bootstrap_limit) {
        panic("native kernel extent outside low bank");
    }
    for (unsigned i = 0; i < switch_ledger.count; ++i) {
        pmap_paddr_t start = switch_ledger.reserved[i].start;
        pmap_paddr_t end = switch_ledger.reserved[i].end;
        if (start >= end) {
            panic("native invalid reservation");
        }
        if (start < switch_bootstrap_start && end > switch_kernel_start) {
            panic("native reservation overlaps kernel");
        }
        if (start >= switch_bootstrap_start && start < switch_bootstrap_limit) {
            switch_bootstrap_limit = start & ~(pmap_paddr_t)PAGE_MASK;
        }
    }
    switch_ram_reserve_gpu_pool();
    switch_ram_committed = true;
}

void switch_ram_check_memory_size(uint64_t requested, uint64_t present)
{
    if (switch_ledger.committed && requested && requested < present) {
        panic("native full RAM profile cannot truncate dense aperture");
    }
}

static bool switch_ram_ledger_reserved(pmap_paddr_t pa)
{
    for (unsigned i = 0; i < switch_ledger.count; i++) {
        if (pa < switch_ledger.reserved[i].end && pa + PAGE_SIZE > switch_ledger.reserved[i].start) {
            return true;
        }
    }
    return false;
}

void switch_ram_account(uint64_t present)
{
    if (!switch_ledger.committed) {
        return;
    }
    uint64_t eligible = 0;
    uint64_t free_bytes = 0;
    for (pmap_paddr_t pa = gPhysBase; pa < gPhysBase + present; pa += PAGE_SIZE) {
        if (switch_ram_pa_valid(pa)) {
            const bool reserved = switch_ram_ledger_reserved(pa);
            if (!reserved) {
                eligible += PAGE_SIZE;
            }
            if (!reserved && !(pa < avail_start && pa + PAGE_SIZE > switch_kernel_start)) {
                free_bytes += PAGE_SIZE;
            }
        }
    }
    max_mem = mem_actual = max_mem_actual = eligible;
    sane_size = free_bytes;
    switch_accounted_bootstrap_end = avail_start;
}

void switch_ram_bootstrap_done(void)
{
    if (switch_ledger.committed) {
        first_avail_phys = avail_start;
        sane_size -= avail_start - switch_accounted_bootstrap_end;
    }
}

unsigned int switch_ram_physmap_pieces(pmap_paddr_t start, pmap_paddr_t end,
                                       struct switch_ram_piece pieces[SWITCH_RAM_PHYSMAP_PIECES])
{
    static const struct switch_ram_range regions[] = {
        {SWITCH_RAM_BANK0_START, SWITCH_RAM_HOLE_START},
        {SWITCH_RAM_HOLE_END, SWITCH_RAM_FB_START},
        {SWITCH_RAM_FB_END, SWITCH_RAM_BANK0_END},
        {SWITCH_RAM_BANK1_START, SWITCH_RAM_BANK1_END},
    };
    unsigned int count = 0;
    for (unsigned i = 0; i < 4; i++) {
        pmap_paddr_t lo = MAX(start, regions[i].start);
        pmap_paddr_t hi = MIN(end, regions[i].end);
        /* The GPU pool splits the region it lies in and is mapped write-combined. */
        pmap_paddr_t split[3][2] = {{lo, hi}, {0, 0}, {0, 0}};
        if (switch_gpu_start < switch_gpu_end && lo < switch_gpu_end && switch_gpu_start < hi) {
            split[0][1] = MAX(lo, switch_gpu_start);
            split[1][0] = MAX(lo, switch_gpu_start);
            split[1][1] = MIN(hi, switch_gpu_end);
            split[2][0] = MIN(hi, switch_gpu_end);
            split[2][1] = hi;
        }
        for (unsigned j = 0; j < 3; j++) {
            if (split[j][0] < split[j][1]) {
                pieces[count++] = (struct switch_ram_piece){split[j][0], split[j][1], j == 1};
            }
        }
    }
    return count;
}

boolean_t switch_ram_virtual_region(unsigned int region_select, vm_map_offset_t *startp, vm_map_size_t *size,
                                    boolean_t *ret)
{
    if (!switch_ram_enabled()) {
        return FALSE;
    }
    /* Full native RAM starts below low globals. Reserve the union of the low ranges once, then retain the separate
     * high window. */
    if (region_select == 0) {
        *startp = VM_MIN_KERNEL_AND_KEXT_ADDRESS;
        *size = ((virtual_space_start + 0x7fffffULL) & ~0x7fffffULL) - *startp;
        *ret = TRUE;
        return TRUE;
    }
    if (region_select != 1) {
        *ret = FALSE;
        return TRUE;
    }
    return FALSE;
}

unsigned int switch_ram_dense_page_count(void)
{
    return switch_ram_enabled() ? (unsigned int)(0x100000000ULL >> PAGE_SHIFT) : 0;
}

boolean_t switch_ram_page_reserved(ppnum_t pnum)
{
    pmap_paddr_t pa = (pmap_paddr_t)pnum << PAGE_SHIFT;
    return switch_ram_reserved(pnum) || pa < switch_stolen_end;
}

boolean_t switch_ram_begin_dense_pages(void)
{
    if (!switch_ram_dense_page_count()) {
        return FALSE;
    }
    (void)pmap_free_pages();
    switch_stolen_end = first_avail;
    first_avail = gPhysBase;
    switch_dense_pages = true;
    return TRUE;
}

void switch_ram_count_pages(unsigned int *avail_page_count, pmap_paddr_t end)
{
    if (!switch_ram_dense_page_count()) {
        return;
    }
    first_avail = gPhysBase;
    *avail_page_count = 0;
    for (pmap_paddr_t pa = gPhysBase; pa < end; pa += PAGE_SIZE) {
        if (!switch_ram_page_reserved((ppnum_t)atop(pa))) {
            ++*avail_page_count;
        }
    }
}

void switch_ram_skip_reserved_slow(void)
{
    while (first_avail < avail_end && switch_ram_page_reserved((ppnum_t)atop(first_avail))) {
        first_avail += PAGE_SIZE;
    }
}

void switch_ram_dense_begin(void)
{
    if (switch_ram_dense_page_count() && !switch_ram_begin_dense_pages()) {
        panic("native dense memory setup failed");
    }
}

boolean_t switch_ram_hole_init_slow(struct vm_page *page, ppnum_t phys_page)
{
    if (!switch_ram_page_reserved(phys_page)) {
        return FALSE;
    }
#if XNU_10063_141_1
    bzero(page, sizeof(*page));
    page->vmp_offset = (vm_object_offset_t)-1;
    page->vmp_busy = TRUE;
    page->vmp_error = TRUE;
    VM_PAGE_SET_PHYS_PAGE(page, phys_page);
#else
    page->vmp_error = true;
#endif
    ++switch_reserved_pages;
    return TRUE;
}

unsigned int switch_ram_dense_done(unsigned int count)
{
    if (switch_ram_dense_page_count()) {
#if XNU_10063_141_1
        assert(count == switch_ram_dense_page_count() && vm_delayed_count == 0);
#else
        assert(count == switch_ram_dense_page_count());
#endif
        assert(pmap_free_pages() == 0);
    }
    return switch_reserved_pages;
}
