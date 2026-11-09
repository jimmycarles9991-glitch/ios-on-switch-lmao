// SPDX-License-Identifier: MIT
// mock_iokit.c's model of the kext's compression (TegraGM20BABI.h TegraGM20BInfoCompression, gpu.c vm_map): which binds of a
// compressible kind it takes, and the contract a client must keep for the comptags to stay right, counted for the harnesses
// (mock_iokit_comp_stats):
//  - refused: a compressible bind of memory the kext gives no comptags (an import, a BO neither Compressible nor Contiguous
//    at 2^17 or more, a size or a va, offset or range off 128 KiB), or with MOCK_COMPRESSION unset (the kext's GetInfo
//    without the bit)
//  - alias: a BO bound compressed and also bound with another kind (that binding would read compressed bytes)
//  - cpu: a BO bound compressed and mapped for the CPU, either way round (the CPU reads compressed bytes), or a Compressible
//    BO's map asked for (the kext refuses it)
//  - nolines: a compressible bind past MOCK_COMP_LINES 128 KiB lines (kIOReturnNoResources, as the kext out of comptags)
// mock_iokit_va_kind names the kind a VA is bound with, for nvdec's ZBC and compressed-target counts.
#ifndef MOCK_COMP_H
#define MOCK_COMP_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    kCompMaps = 1024
};

static struct comp_map {
    uint64_t va, range, handle;
    uint32_t kind;
} comp_maps[kCompMaps];

static pthread_mutex_t comp_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_ulong comp_binds, comp_refused, comp_alias, comp_cpu, comp_nolines, comp_lines_used;

/* NVN's and NIL's compressible kinds for one sample (gm200_mmu_kind demotes each) */
static int comp_kind(uint32_t kind)
{
    switch (kind) {
    case 0x51: /* Z24S8_2CZ */
    case 0xce: /* ZF32_X24S8_2CSZV */
    case 0xd8: /* C32_2C */
    case 0xdb: /* C32_2CRA */
    case 0xe6: /* C64_2C */
    case 0xe9: /* C64_2CRA */
    case 0xf4: /* C128_2C */
    case 0xf5: /* C128_2CR */
        return 1;
    default:
        return 0;
    }
}

static int comp_aligned(uint64_t v)
{
    return !(v & ((1ull << 17) - 1));
}

/* A Map op of BO `h` (o its record): KERN_SUCCESS, or the kext's answer refusing it */
static kern_return_t comp_bind(unsigned h, struct bo *o, const struct TegraGM20BAbiBindOp *op)
{
    const int compressed = comp_kind(op->kind);
    if (o->kind_set && o->kind != op->kind && (compressed || comp_kind(o->kind))) {
        atomic_fetch_add(&comp_alias, 1);
        fprintf(stderr, "mock: BO %u bound with kind 0x%x and 0x%x, one compressible\n", h, o->kind, op->kind);
    }
    if (!compressed) {
        o->kind = op->kind;
        o->kind_set = 1;
        return KERN_SUCCESS;
    }
    const int runs =
        (o->flags & TegraGM20BBoCompressible) || ((o->flags & TegraGM20BBoContiguous) && o->align_log2 >= 17);
    if (!getenv("MOCK_COMPRESSION") || o->import || !runs || !comp_aligned(o->bytes) || !comp_aligned(op->va) ||
        !comp_aligned(op->bo_offset) || !comp_aligned(op->range) || !op->range) {
        atomic_fetch_add(&comp_refused, 1);
        return kIOReturnBadArgument;
    }
    if (!o->lines) {
        const uint64_t lines = o->bytes >> 17,
                       cap = getenv("MOCK_COMP_LINES") ? strtoull(getenv("MOCK_COMP_LINES"), 0, 0) : ~0ull;
        if (atomic_load(&comp_lines_used) + lines > cap) {
            atomic_fetch_add(&comp_nolines, 1);
            return kIOReturnNoResources;
        }
        atomic_fetch_add(&comp_lines_used, lines);
        o->lines = lines;
    }
    if (o->cpu_maps) {
        atomic_fetch_add(&comp_cpu, 1);
        fprintf(stderr, "mock: BO %u bound compressed while the CPU maps it\n", h);
    }
    o->kind = op->kind;
    o->kind_set = 1;
    atomic_fetch_add(&comp_binds, 1);
    pthread_mutex_lock(&comp_lock);
    for (unsigned i = 0; i < kCompMaps; i++)
        if (!comp_maps[i].range) {
            comp_maps[i] = (struct comp_map){op->va, op->range, h, op->kind};
            break;
        }
    pthread_mutex_unlock(&comp_lock);
    return KERN_SUCCESS;
}

/* An Unmap op over [va, va + range), or the BO `h` freed (range 0): its compressed binds go */
static void comp_unbind(uint64_t va, uint64_t range, unsigned h)
{
    pthread_mutex_lock(&comp_lock);
    for (unsigned i = 0; i < kCompMaps; i++) {
        struct comp_map *m = &comp_maps[i];
        if (m->range && (range ? (m->va < va + range && va < m->va + m->range) : m->handle == h))
            *m = (struct comp_map){0};
    }
    pthread_mutex_unlock(&comp_lock);
}

/* A CPU mapping of BO `h`: 1 if the kext refuses it (a Compressible BO) */
static int comp_cpu_map(unsigned h, struct bo *o)
{
    if (o->flags & TegraGM20BBoCompressible) {
        atomic_fetch_add(&comp_cpu, 1);
        fprintf(stderr, "mock: BO %u is Compressible and was asked to be mapped for the CPU\n", h);
        return 1;
    }
    o->cpu_maps++;
    if (o->kind_set && comp_kind(o->kind)) {
        atomic_fetch_add(&comp_cpu, 1);
        fprintf(stderr, "mock: BO %u bound compressed and mapped for the CPU\n", h);
    }
    return 0;
}

/* BoAlloc's answer to a Compressible BO the kext would refuse (bad flags or size, or the pool short: MOCK_COMP_REFUSE=<n>
 * refuses the first n with kIOReturnNoMemory), else KERN_SUCCESS */
static kern_return_t comp_alloc(const struct TegraGM20BAbiBoAlloc *a)
{
    static atomic_uint refusals;
    if (!(a->flags & TegraGM20BBoCompressible))
        return KERN_SUCCESS;
    if (a->flags != TegraGM20BBoCompressible || !comp_aligned(a->bytes))
        return kIOReturnBadArgument;
    const char *refuse = getenv("MOCK_COMP_REFUSE");
    if (refuse && atomic_fetch_add(&refusals, 1) < strtoul(refuse, 0, 0))
        return kIOReturnNoMemory;
    return KERN_SUCCESS;
}

/* The PTE kind `va` is bound with if a compressed bind holds it, else 0 */
uint32_t mock_iokit_va_kind(uint64_t va)
{
    uint32_t kind = 0;
    pthread_mutex_lock(&comp_lock);
    for (unsigned i = 0; i < kCompMaps && !kind; i++)
        if (comp_maps[i].range && va >= comp_maps[i].va && va < comp_maps[i].va + comp_maps[i].range)
            kind = comp_maps[i].kind;
    pthread_mutex_unlock(&comp_lock);
    return kind;
}

/* binds, refused, alias, cpu, nolines */
void mock_iokit_comp_stats(unsigned long out[5])
{
    out[0] = comp_binds;
    out[1] = comp_refused;
    out[2] = comp_alias;
    out[3] = comp_cpu;
    out[4] = comp_nolines;
}

#endif
