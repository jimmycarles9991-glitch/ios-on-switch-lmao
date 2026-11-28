// SPDX-License-Identifier: GPL-2.0-only
/*
 * a64x_xnu.c: load-time translation of arm64e user code for an ARMv8.0 core.
 *
 * Images mapped by the kernel (main executable, dyld) and by dyld through mmap(): the executable segment is re-mapped
 * through an apple_protect pager whose transform is a64x_text_page(); a second mapping right after the image holds
 * the stubs (a64x_stub_page()). Both pagers read pages of the image file: apple_protect_pager_data_request()
 * validates every source page against the image's code signature before the transform runs, and marks the result
 * validated only if the source was. The stubs are generated at load time from the same file; at page-in each text
 * page regenerates its own stubs from the validated words and compares: if they differ, the page keeps its
 * original LSE words (they then reach a64x_emulate_user()).
 *
 * The dyld shared cache: every executable cache mapping goes through a64x_cache_page(), which applies the one-word
 * rewrites, turns dead PAC checks into `b ok`, and overlays the words of the translation plan (stubs placed in those
 * dead checks, see translate/arm64e/tools/a64plan2.c). The plan's SHA-256 comes from /chosen/a64x-plan-sha256 in the device tree.
 *
 * Pointer authentication is the identity everywhere: nothing is signed, every authentication succeeds.
 *
 * Plain arm64 images get the same treatment minus the PAC rewrites, and only if their scan finds something the
 * A57 lacks; images marked as built for this core (__TEXT,__a64x_target) are never touched (see <switch/a64x.h>).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>
#include <sys/ubc_internal.h>
#include <sys/sysctl.h>
#include <sys/fcntl.h>
#include <switch/a64x.h>
#include <switch/pretranslated.h>
#include <mach-o/loader.h>
#include <mach/mach_vm.h>
#include <mach/exception_types.h>
#include <mach/message.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/page_decrypt.h>
#include <vm/vm_map_xnu.h>
#include <vm/vm_kern_xnu.h>
#include <vm/vm_protos.h>
#include <vm/vm_protos_internal.h>
#include <vm/vm_sanitize_internal.h>
#include <vm/memory_object_xnu.h>
#include <sys/mman_private.h>
#include <libkern/OSAtomic.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/device_tree.h>
#include <arm64/proc_reg.h>
#include <mach/arm/thread_status.h>

#include "../../../translate/arm64e/a64x/a64x.h"

#if CONFIG_A64X

#define A64X_PAGE          4096u
#define A64X_WORDS         (A64X_PAGE / 4)
#define A64X_READ_CLUSTER  (128u * 1024u)
#define A64X_STUB_INIT_CAP 1024u
#define A64X_BRK_STUB      0xD4200020u /* brk #1: never executed (no branch reaches unused stub words) */

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
/* statistics (sysctl kern.a64x.*) */
static uint64_t a64x_images, a64x_pages, a64x_stub_words, a64x_mismatch, a64x_traps, a64x_traps_unhandled;
static uint64_t a64x_cache_pages, a64x_images_native, a64x_images_marked, a64x_island_words;
static uint64_t a64x_images_installed;
static uint64_t a64x_jit_patches;
#define A64X_INC(counter)        OSIncrementAtomic64((SInt64 *)&(counter))
#define A64X_ADD(counter, delta) OSAddAtomic64((SInt64)(delta), (SInt64 *)&(counter))
#else
#define A64X_INC(counter)        ((void)0)
#define A64X_ADD(counter, delta) ((void)0)
#endif

LCK_GRP_DECLARE(a64x_lck_grp, "a64x");
static LCK_MTX_DECLARE(a64x_plan_lock, &a64x_lck_grp);

struct a64x_range {
    uint64_t lo, hi;
};

struct a64x_image {
    volatile SInt32 refs;
    vnode_t vp;
    uint64_t text_off, text_size; /* executable segment, absolute file offsets */
    uint64_t stub_off;            /* file pages behind the stub mapping (contents replaced) */
    int64_t stub_delta;           /* stub mapping VA - executable segment VA */
    uint32_t npages;
    uint32_t *cursor; /* [npages + 1]: stub index at the start of each page */
    uint32_t *stubs;
    uint32_t nstub, stubcap;
    uint32_t nranges;
    uint32_t ranges_cap;
    struct a64x_range *ranges; /* instruction sections, absolute file offsets, sorted and merged */
    uint32_t map_flags;        /* A64X_MAP_NO_PAC for plain arm64 */
    uint64_t changed;          /* words the scan rewrote */
    uint8_t *data;             /* [npages * 128]: 1 bit per word of the segment, set = data in __text */
    bool has_vtemps;
    bool stub_ended;
    bool text_ended;
};

static bool img_is_code(void *ctx, uint64_t off)
{
    const struct a64x_image *im = ctx;
    if (im->data && off >= im->text_off) {
        uint64_t k = (off - im->text_off) / 4;
        if (k < (uint64_t)im->npages * A64X_WORDS && (im->data[k >> 3] & (1u << (k & 7)))) {
            return false; /* data island (a64x_islands_*) */
        }
    }
    uint32_t lo = 0, hi = im->nranges;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (im->ranges[mid].hi <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < im->nranges && off >= im->ranges[lo].lo;
}

static void a64x_image_free(struct a64x_image *im)
{
    if (im->vp) {
        vnode_rele(im->vp);
    }
    if (im->cursor) {
        kfree_data(im->cursor, (size_t)(im->npages + 1) * sizeof(uint32_t));
    }
    if (im->stubs) {
        kfree_data(im->stubs, (size_t)im->stubcap * sizeof(uint32_t));
    }
    if (im->ranges) {
        kfree_data(im->ranges, im->ranges_cap * sizeof(struct a64x_range));
    }
    if (im->data) {
        kfree_data(im->data, (size_t)im->npages * (A64X_WORDS / 8));
    }
    kfree_type(struct a64x_image, im);
}

static void a64x_image_release(void *ops)
{
    struct a64x_image *im = ops;
    if (OSDecrementAtomic(&im->refs) == 1) {
        a64x_image_free(im);
    }
}

static void a64x_stub_end(void *ops)
{
    struct a64x_image *im = ops;
    os_atomic_store(&im->stub_ended, true, release);
    a64x_image_release(im);
}

static void a64x_text_end(void *ops)
{
    struct a64x_image *im = ops;
    os_atomic_store(&im->text_ended, true, release);
    a64x_image_release(im);
}

/* apple_protect transform for the executable segment */
static int a64x_text_page(const void *src, void *dst, unsigned long long off, void *ops)
{
    struct a64x_image *im = ops;
    if (off < im->text_off || off >= im->text_off + (uint64_t)im->npages * A64X_PAGE) {
        bcopy(src, dst, A64X_PAGE);
        return 0;
    }
    uint32_t pg = (uint32_t)((off - im->text_off) / A64X_PAGE);
    uint32_t c0 = im->cursor[pg], c1 = im->cursor[pg + 1];
    struct a64x_code_map cm = {im, img_is_code, im->map_flags, NULL};
    bool ok = true;
    if (c1 > c0) {
        void *scratch = NULL;
        size_t scratch_bytes = 0;
        uint32_t *chk = NULL;
        struct a64x_walk walk = {NULL, NULL, 0};
        struct a64x_walk *walkp = NULL;
#if DEVELOPMENT || DEBUG
        size_t chk_bytes = (size_t)(c1 - c0) * sizeof(uint32_t);
        scratch_bytes = chk_bytes + (im->has_vtemps ? A64X_WALK_BYTES(A64X_WORDS) : 0);
        scratch = kalloc_data(scratch_bytes, Z_WAITOK);
        if (scratch) {
            chk = scratch;
            if (im->has_vtemps) {
                walk = a64x_walk_in(chk + (c1 - c0), A64X_WORDS);
                walkp = &walk;
            }
        } else {
            ok = false;
        }
#else
        if (im->has_vtemps) {
            scratch_bytes = A64X_WALK_BYTES(A64X_WORDS);
            scratch = kalloc_data(scratch_bytes, Z_WAITOK);
            if (scratch) {
                walk = a64x_walk_in(scratch, A64X_WORDS);
                walkp = &walk;
            } else {
                ok = false;
            }
        }
#endif
        if (ok) {
            struct a64x_stub_region sr = {im->text_off + (uint64_t)im->stub_delta, chk, c1, c0, walkp};
            uint32_t end = a64x_translate_page(src, dst, A64X_WORDS, off, &cm, &sr, c0, NULL);
#if DEVELOPMENT || DEBUG
            if (end != c1 || bcmp(chk, im->stubs + c0, chk_bytes)) {
#else
            if (end != c1) {
#endif
                ok = false;
                A64X_INC(a64x_mismatch);
                printf("a64x: text page 0x%llx: stubs differ from the validated words; leaving LSE to the trap path\n",
                       off);
            }
        }
        if (scratch) {
            kfree_data(scratch, scratch_bytes);
        }
    } else {
        (void)a64x_translate_page(src, dst, A64X_WORDS, off, &cm, NULL, 0, NULL);
    }
    if (!ok) {
        /* one-word rewrites only: what needs a stub keeps its original word */
        (void)a64x_translate_page(src, dst, A64X_WORDS, off, &cm, NULL, 0, NULL);
    }
    A64X_INC(a64x_pages);
    return 0;
}

/* apple_protect transform for the stub mapping: the source page only provides validation */
static int a64x_stub_page(const void *src, void *dst, unsigned long long off, void *ops)
{
    struct a64x_image *im = ops;
    uint32_t *d = dst;
    (void)src;
    uint64_t w0 = (off - im->stub_off) / 4;
    for (uint32_t i = 0; i < A64X_WORDS; i++) {
        d[i] = w0 + i < im->nstub ? im->stubs[w0 + i] : A64X_BRK_STUB;
    }
    return 0;
}

#define A64X_MAX_SECTIONS 64

/* load commands -> executable segment and instruction sections (over 1 KB: allocated, never on the stack) */
struct a64x_cand {
    uint64_t va, size, off;
};

struct a64x_layout {
    uint64_t exec_fileoff, exec_filesize, exec_vmaddr;
    uint64_t vm_end;
    uint32_t nsect, ncand;
    bool marked;                               /* has the __TEXT,__a64x_target section: built for this core */
    uint64_t text_vmaddr;                      /* __TEXT (function starts count from it) */
    uint32_t fs_off, fs_size;                  /* LC_FUNCTION_STARTS, slice-relative */
    uint32_t dic_off, dic_size;                /* LC_DATA_IN_CODE, slice-relative */
    uint32_t cs_off, cs_size;                  /* LC_CODE_SIGNATURE, slice-relative */
    struct a64x_range sect[A64X_MAX_SECTIONS]; /* instruction sections, absolute file offsets, file order */
    struct a64x_cand cand[A64X_MAX_SECTIONS];  /* flagless S_REGULAR sections, promoted via LC_FUNCTION_STARTS */
};

static int a64x_parse(const void *lcmds, size_t size, off_t slice_off, struct a64x_layout *lay)
{
    const struct mach_header_64 *mh = lcmds;
    if (size < sizeof(*mh) || mh->magic != MH_MAGIC_64 || mh->sizeofcmds > size - sizeof(*mh)) {
        return EBADMACHO;
    }
    bzero(lay, sizeof(*lay));
    const uint8_t *p = (const uint8_t *)(mh + 1), *end = p + mh->sizeofcmds;
    int nexec = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const void *)p;
        if (p + sizeof(*lc) > end || lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) {
            return EBADMACHO;
        }
        if (lc->cmd == LC_FUNCTION_STARTS || lc->cmd == LC_DATA_IN_CODE || lc->cmd == LC_CODE_SIGNATURE) {
            const struct linkedit_data_command *ld = (const void *)p;
            if (lc->cmdsize >= sizeof(*ld)) {
                if (lc->cmd == LC_FUNCTION_STARTS) {
                    lay->fs_off = ld->dataoff;
                    lay->fs_size = ld->datasize;
                } else if (lc->cmd == LC_DATA_IN_CODE) {
                    lay->dic_off = ld->dataoff;
                    lay->dic_size = ld->datasize;
                } else {
                    lay->cs_off = ld->dataoff;
                    lay->cs_size = ld->datasize;
                }
            }
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)p;
            if (!strncmp(sg->segname, SEG_TEXT, sizeof(sg->segname))) {
                lay->text_vmaddr = sg->vmaddr;
            }
            if (lc->cmdsize < sizeof(*sg) + (uint64_t)sg->nsects * sizeof(struct section_64)) {
                return EBADMACHO;
            }
            if (strncmp(sg->segname, SEG_PAGEZERO, sizeof(sg->segname)) && sg->vmaddr + sg->vmsize > lay->vm_end) {
                lay->vm_end = sg->vmaddr + sg->vmsize;
            }
            const struct section_64 *ms = (const void *)(sg + 1);
            for (uint32_t s = 0; s < sg->nsects; s++) {
                if (!strncmp(ms[s].segname, A64X_TARGET_SEGNAME, sizeof(ms[s].segname)) &&
                    !strncmp(ms[s].sectname, A64X_TARGET_SECTNAME, sizeof(ms[s].sectname))) {
                    lay->marked = true;
                }
            }
            if (sg->initprot & VM_PROT_EXECUTE) {
                if (nexec++) {
                    return ENOTSUP; /* one executable segment (__TEXT) is all arm64e images have */
                }
                uint64_t seg_end = 0, abs_seg_end = 0;
                if (slice_off < 0 || os_add_overflow((uint64_t)slice_off, sg->fileoff, &lay->exec_fileoff) ||
                    os_add_overflow(sg->fileoff, sg->filesize, &seg_end) ||
                    os_add_overflow((uint64_t)slice_off, seg_end, &abs_seg_end)) {
                    return EBADMACHO;
                }
                lay->exec_filesize = sg->filesize;
                lay->exec_vmaddr = sg->vmaddr;
                const struct section_64 *sc = (const void *)(sg + 1);
                for (uint32_t s = 0; s < sg->nsects; s++) {
                    enum a64x_sect_kind sk =
                        a64x_classify_exec_section(sc[s].sectname, sc[s].addr, sc[s].size, sc[s].flags);
                    if (sk == A64X_SECT_DATA) {
                        continue;
                    }
                    uint64_t lo = 0, hi = 0;
                    if (sc[s].offset < sg->fileoff || sc[s].offset > seg_end || sc[s].size > seg_end - sc[s].offset ||
                        os_add_overflow((uint64_t)slice_off, sc[s].offset, &lo) ||
                        os_add_overflow(lo, sc[s].size, &hi)) {
                        return EBADMACHO;
                    }
                    if (sk == A64X_SECT_CODE) {
                        if (!(sc[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) &&
                            (sc[s].offset & 3u) != 0u) {
                            continue;
                        }
                        if (lay->nsect >= A64X_MAX_SECTIONS) {
                            return ENOTSUP;
                        }
                        lay->sect[lay->nsect].lo = lo;
                        lay->sect[lay->nsect].hi = hi;
                        lay->nsect++;
                    } else if ((sc[s].offset & 3u) == 0u && lay->ncand < A64X_MAX_SECTIONS) {
                        uint64_t va_end = 0;
                        if (os_add_overflow(sc[s].addr, sc[s].size, &va_end)) {
                            return EBADMACHO;
                        }
                        lay->cand[lay->ncand].va = sc[s].addr;
                        lay->cand[lay->ncand].size = sc[s].size;
                        lay->cand[lay->ncand].off = lo;
                        lay->ncand++;
                    }
                }
            }
        }
        p += lc->cmdsize;
    }
    return nexec ? 0 : ENOEXEC;
}

/*
 * Data islands (a64x_islands_*): function starts and data-in-code ranges from the image's LINKEDIT, then one pass
 * over the segment. Words found to be data leave the code map (img_is_code), so neither the scan nor the pager
 * touches them. Without LC_FUNCTION_STARTS the runs are cut at padding and terminators only.
 */
static int a64x_find_islands(struct a64x_image *im, const struct a64x_layout *lay, off_t slice_off)
{
    _Static_assert(A64X_MAX_SECTIONS <= 64, "cand_hit bitmask");
    size_t nbits = (size_t)im->npages * A64X_WORDS, nbytes = nbits / 8;
    uint8_t *fs = NULL;
    uint64_t cand_hit = 0;
    int error = 0;
    size_t cluster_pages = MIN((size_t)im->npages, (size_t)(A64X_READ_CLUSTER / A64X_PAGE));
    size_t cluster_bytes = cluster_pages ? cluster_pages * A64X_PAGE : A64X_PAGE;
    uint32_t *cluster = kalloc_data(cluster_bytes, Z_WAITOK);
    im->data = kalloc_data(nbytes, Z_WAITOK | Z_ZERO);
    int resid = 0;
    if (lay->fs_size && lay->fs_size <= (1u << 24)) {
        uint8_t *buf = kalloc_data(lay->fs_size, Z_WAITOK);
        if (nbytes) {
            fs = kalloc_data(nbytes, Z_WAITOK | Z_ZERO);
        }
        if (buf &&
            !vn_rdwr(UIO_READ, im->vp, (caddr_t)buf, (int)lay->fs_size, slice_off + lay->fs_off, UIO_SYSSPACE,
                     IO_NODELOCKED, vfs_context_ucred(vfs_context_kernel()), &resid, current_proc()) &&
            !resid) {
            uint64_t addr = lay->text_vmaddr;
            for (uint32_t q = 0; q < lay->fs_size;) {
                uint64_t v = 0;
                int sh = 0;
                bool ok = false;
                while (q < lay->fs_size) {
                    uint8_t b = buf[q++];
                    if (sh == 63 && b > 1u) {
                        break;
                    }
                    v |= (uint64_t)(b & 0x7Fu) << sh;
                    if (!(b & 0x80u)) {
                        ok = true;
                        break;
                    }
                    if (sh >= 63) {
                        break;
                    }
                    sh += 7;
                }
                if (!ok || !v || v > UINT64_MAX - addr) {
                    break;
                }
                addr += v;
                if (fs && addr >= lay->exec_vmaddr) {
                    uint64_t k = (addr - lay->exec_vmaddr) / 4;
                    if (k < nbits) {
                        fs[k >> 3] |= (uint8_t)(1u << (k & 7));
                    }
                }
                if ((addr & 3u) == 0u) {
                    for (uint32_t c = 0; c < lay->ncand; c++) {
                        if (!(cand_hit & (1ull << c)) && addr >= lay->cand[c].va &&
                            addr - lay->cand[c].va < lay->cand[c].size) {
                            cand_hit |= 1ull << c;
                        }
                    }
                }
            }
        }
        if (buf) {
            kfree_data(buf, lay->fs_size);
        }
    }
    uint32_t nrange = lay->nsect;
    for (uint32_t c = 0; c < lay->ncand; c++) {
        if (cand_hit & (1ull << c)) {
            nrange++;
        }
    }
    if (nrange > A64X_MAX_SECTIONS) {
        error = ENOTSUP;
        goto out;
    }
    if (nrange) {
        im->ranges = kalloc_data(nrange * sizeof(struct a64x_range), Z_WAITOK);
        if (!im->ranges) {
            error = ENOMEM;
            goto out;
        }
        im->ranges_cap = nrange;
        if (lay->nsect) {
            bcopy(lay->sect, im->ranges, lay->nsect * sizeof(struct a64x_range));
        }
        uint32_t nr = lay->nsect;
        for (uint32_t c = 0; c < lay->ncand; c++) {
            if (cand_hit & (1ull << c)) {
                uint64_t hi = 0;
                if (os_add_overflow(lay->cand[c].off, lay->cand[c].size, &hi)) {
                    error = EBADMACHO;
                    goto out;
                }
                im->ranges[nr].lo = lay->cand[c].off;
                im->ranges[nr].hi = hi;
                nr++;
            }
        }
        for (uint32_t i = 1; i < nr; i++) {
            for (uint32_t j = i; j > 0 && im->ranges[j - 1].lo > im->ranges[j].lo; j--) {
                struct a64x_range t = im->ranges[j];
                im->ranges[j] = im->ranges[j - 1];
                im->ranges[j - 1] = t;
            }
        }
        uint32_t merged = 0;
        for (uint32_t i = 0; i < nr; i++) {
            if (merged > 0 && im->ranges[i].lo <= im->ranges[merged - 1].hi) {
                if (im->ranges[i].hi > im->ranges[merged - 1].hi) {
                    im->ranges[merged - 1].hi = im->ranges[i].hi;
                }
            } else {
                im->ranges[merged++] = im->ranges[i];
            }
        }
        im->nranges = merged;
    }
    if (!cluster || !im->data) {
        goto out;
    }
    if (lay->dic_size && lay->dic_size <= (1u << 20)) {
        uint8_t *buf = kalloc_data(lay->dic_size, Z_WAITOK);
        if (buf &&
            !vn_rdwr(UIO_READ, im->vp, (caddr_t)buf, (int)lay->dic_size, slice_off + lay->dic_off, UIO_SYSSPACE,
                     IO_NODELOCKED, vfs_context_ucred(vfs_context_kernel()), &resid, current_proc()) &&
            !resid) {
            for (uint32_t q = 0; q + sizeof(struct data_in_code_entry) <= lay->dic_size;
                 q += sizeof(struct data_in_code_entry)) {
                const struct data_in_code_entry *d = (const void *)(buf + q);
                uint64_t lo = (uint64_t)slice_off + d->offset;
                for (uint64_t o = lo; o < lo + d->length; o += 4) {
                    if (o >= im->text_off && (o - im->text_off) / 4 < nbits) {
                        uint64_t k = (o - im->text_off) / 4;
                        im->data[k >> 3] |= (uint8_t)(1u << (k & 7));
                    }
                }
            }
        }
        if (buf) {
            kfree_data(buf, lay->dic_size);
        }
    }
    struct a64x_islands is = {im->data, fs, 0, 0, false, false, 0, 0, 0, 0};
    uint8_t *dic_only = im->data;
    im->data = NULL; /* the pass sees instruction sections only, not the islands it is finding */
    struct a64x_code_map cm = {im, img_is_code, im->map_flags, NULL};
    for (uint32_t pg = 0; pg < im->npages;) {
        uint32_t np = (uint32_t)MIN((size_t)(im->npages - pg), cluster_pages);
        uint32_t bytes = np * A64X_PAGE;
        uint64_t off = im->text_off + (uint64_t)pg * A64X_PAGE;
        bzero(cluster, bytes);
        if (vn_rdwr(UIO_READ, im->vp, (caddr_t)cluster, (int)bytes, (off_t)off, UIO_SYSSPACE, IO_NODELOCKED,
                    vfs_context_ucred(vfs_context_kernel()), &resid, current_proc())) {
            break;
        }
        for (uint32_t j = 0; j < np; j++) {
            uint32_t p = pg + j;
            a64x_islands_page(&is, cluster + (size_t)j * A64X_WORDS, A64X_WORDS, (uint64_t)p * A64X_WORDS,
                              im->text_off + (uint64_t)p * A64X_PAGE, &cm);
        }
        pg += np;
    }
    a64x_islands_end(&is, nbits);
    im->data = dic_only;
    if (is.runs) {
        A64X_ADD(a64x_island_words, is.words);
    }
out:
    if (fs) {
        kfree_data(fs, nbytes);
    }
    if (cluster) {
        kfree_data(cluster, cluster_bytes);
    }
    return error;
}

static bool a64x_stubs_grow(struct a64x_image *im, uint32_t keep, uint32_t min_cap)
{
    uint32_t ncap = im->stubcap ? im->stubcap : A64X_STUB_INIT_CAP;
    while (ncap < min_cap) {
        if (ncap > UINT32_MAX / 2) {
            return false;
        }
        ncap *= 2;
    }
    uint32_t *n = kalloc_data((size_t)ncap * sizeof(uint32_t), Z_WAITOK);
    if (!n) {
        return false;
    }
    if (im->stubs) {
        if (keep) {
            bcopy(im->stubs, n, (size_t)keep * sizeof(uint32_t));
        }
        kfree_data(im->stubs, (size_t)im->stubcap * sizeof(uint32_t));
    }
    im->stubs = n;
    im->stubcap = ncap;
    return true;
}

/* read the executable segment once and lay out its stubs; stub_delta must be fixed before */
static int a64x_scan(struct a64x_image *im)
{
    size_t cluster_pages = MIN((size_t)im->npages, (size_t)(A64X_READ_CLUSTER / A64X_PAGE));
    size_t cluster_bytes = cluster_pages ? cluster_pages * A64X_PAGE : A64X_PAGE;
    uint32_t *cluster = kalloc_data(cluster_bytes, Z_WAITOK);
    uint32_t *out = kalloc_data(A64X_PAGE, Z_WAITOK);
    void *walk_buf = kalloc_data(A64X_WALK_BYTES(A64X_WORDS), Z_WAITOK);
    struct a64x_stats *st = kalloc_type(struct a64x_stats, Z_WAITOK);
    if (!cluster || !out || !walk_buf || !st) {
        if (st) {
            kfree_type(struct a64x_stats, st);
        }
        if (cluster) {
            kfree_data(cluster, cluster_bytes);
        }
        if (out) {
            kfree_data(out, A64X_PAGE);
        }
        if (walk_buf) {
            kfree_data(walk_buf, A64X_WALK_BYTES(A64X_WORDS));
        }
        return ENOMEM;
    }
    struct a64x_walk walk = a64x_walk_in(walk_buf, A64X_WORDS);
    struct a64x_code_map cm = {im, img_is_code, im->map_flags, NULL};
    uint32_t cursor = 0;
    int error = 0;
    if (im->npages && !a64x_stubs_grow(im, 0, A64X_STUB_INIT_CAP)) {
        error = ENOMEM;
    }
    for (uint32_t pg = 0; pg < im->npages && !error;) {
        uint32_t np = (uint32_t)MIN((size_t)(im->npages - pg), cluster_pages);
        uint32_t bytes = np * A64X_PAGE;
        int resid = 0;
        uint64_t off = im->text_off + (uint64_t)pg * A64X_PAGE;
        bzero(cluster, bytes);
        error = vn_rdwr(UIO_READ, im->vp, (caddr_t)cluster, (int)bytes, (off_t)off, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
        if (error) {
            break;
        }
        for (uint32_t j = 0; j < np && !error; j++) {
            uint32_t p = pg + j;
            const uint32_t *page = cluster + (size_t)j * A64X_WORDS;
            uint64_t p_off = im->text_off + (uint64_t)p * A64X_PAGE;
            uint32_t next;
            for (;;) {
                if (im->stubcap - cursor < A64X_STUB_MAX &&
                    !a64x_stubs_grow(im, cursor, cursor + A64X_STUB_MAX)) {
                    error = ENOMEM;
                    break;
                }
                bzero(st, sizeof(*st));
                struct a64x_stub_region sr = {im->text_off + (uint64_t)im->stub_delta, im->stubs, im->stubcap, 0,
                                              &walk};
                next = a64x_translate_page(page, out, A64X_WORDS, p_off, &cm, &sr, cursor, st);
                if (im->stubcap - next >= A64X_STUB_MAX) {
                    break;
                }
                if (!a64x_stubs_grow(im, cursor, next + A64X_STUB_MAX)) {
                    error = ENOMEM;
                    break;
                }
            }
            if (error) {
                break;
            }
            im->cursor[p] = cursor;
            cursor = next;
            if (st->stubs[A64X_FP16] || st->stubs[A64X_FCMA] || st->stubs[A64X_RDM]) {
                im->has_vtemps = true;
            }
            for (uint32_t i = 0; i < A64X_WORDS; i++) {
                im->changed += out[i] != page[i];
            }
        }
        pg += np;
    }
    im->cursor[im->npages] = cursor;
    im->nstub = cursor;
    if (!error) {
        if (cursor == 0 && im->stubs) {
            kfree_data(im->stubs, (size_t)im->stubcap * sizeof(uint32_t));
            im->stubs = NULL;
            im->stubcap = 0;
        } else if (cursor < im->stubcap) {
            uint32_t *fit = kalloc_data((size_t)cursor * sizeof(uint32_t), Z_WAITOK);
            if (fit) {
                bcopy(im->stubs, fit, (size_t)cursor * sizeof(uint32_t));
                kfree_data(im->stubs, (size_t)im->stubcap * sizeof(uint32_t));
                im->stubs = fit;
                im->stubcap = cursor;
            }
        }
    }
    kfree_type(struct a64x_stats, st);
    kfree_data(cluster, cluster_bytes);
    kfree_data(out, A64X_PAGE);
    kfree_data(walk_buf, A64X_WALK_BYTES(A64X_WORDS));
    return error;
}

/* An installed copy of a staged system app (switch_pretranslated_translation): its executable segment is the tree's
 * translation's, laid out as the image is and validated against that file's own signature. ENOENT when there is none;
 * a mapping that fails after the image's own was replaced fails the load. */
static int a64x_map_translation(vm_map_t map, struct vnode *vp, off_t slice_off, const struct a64x_layout *lay,
                                vm_map_offset_t exec_va)
{
    if (!lay->cs_size) {
        return ENOENT;
    }
    struct vnode *copy = switch_pretranslated_translation(vp, slice_off, slice_off + lay->cs_off, lay->cs_size);
    if (copy == NULLVP) {
        return ENOENT;
    }
    vm_map_kernel_flags_t vmk_flags = VM_MAP_KERNEL_FLAGS_FIXED(.vmf_overwrite = true);
    vmk_flags.vmkf_no_copy_on_read = 1;
    vm_map_offset_t va = exec_va;
    kern_return_t kr = vm_map_enter_mem_object_control(
        map, &va, vm_map_round_page(lay->exec_filesize, vm_map_page_mask(map)), 0, vmk_flags,
        ubc_getobject(copy, UBC_FLAGS_NONE), lay->exec_fileoff, TRUE, VM_PROT_READ | VM_PROT_EXECUTE,
        VM_PROT_READ | VM_PROT_EXECUTE, VM_INHERIT_DEFAULT);
    vnode_put(copy);
    if (kr != KERN_SUCCESS) {
        printf("a64x: %s: the translation's text not mapped (kr %d)\n", vp->v_name ? vp->v_name : "?", kr);
        return EIO;
    }
    A64X_INC(a64x_images_installed);
    return 0;
}

static int a64x_map_image(vm_map_t map, struct vnode *vp, off_t slice_off, const struct mach_header_64 *mh,
                          const struct a64x_layout *lay, vm_map_offset_t exec_va)
{
    int kind = a64x_image_kind(mh->cputype, mh->cpusubtype);
    if (kind == A64X_IMAGE_NATIVE) {
        return 0;
    }
    if (mh->reserved == A64X_PRETRANSLATED || lay->marked) {
        /* built for this core: mapped as is, like any binary (signature and trust cache checks unchanged) */
        A64X_INC(a64x_images_marked);
        return 0;
    }
    {
        int error = a64x_map_translation(map, vp, slice_off, lay, exec_va);
        if (error != ENOENT) {
            return error;
        }
    }
    struct a64x_image *im = kalloc_type(struct a64x_image, Z_WAITOK | Z_ZERO);
    if (!im) {
        return ENOMEM;
    }
    im->map_flags = kind == A64X_IMAGE_ARM64 ? A64X_MAP_NO_PAC : 0;
    im->refs = 1; /* local owner reference; each pager takes its own ref before vm_map_apple_protected */
    im->text_off = lay->exec_fileoff;
    im->text_size = lay->exec_filesize;
    im->npages = (uint32_t)((lay->exec_filesize + A64X_PAGE - 1) / A64X_PAGE);
    im->cursor = kalloc_data((size_t)(im->npages + 1) * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
    if (!im->cursor) {
        a64x_image_release(im);
        return ENOMEM;
    }
    if (vnode_ref(vp)) {
        a64x_image_release(im);
        return EIO;
    }
    im->vp = vp;

    /* stubs go right after the image (same distance in every process) */
    vm_map_offset_t image_end = exec_va + (lay->vm_end - lay->exec_vmaddr);
    vm_map_offset_t stub_va = vm_map_round_page(image_end, vm_map_page_mask(map));
    im->stub_delta = (int64_t)(stub_va - exec_va);
    int error = a64x_find_islands(im, lay, slice_off);
    if (!error) {
        error = a64x_scan(im);
    }
    if (error) {
        a64x_image_release(im);
        printf("a64x: %s: translation failed (%d)\n", vp->v_name ? vp->v_name : "?", error);
        return error;
    }
    if (kind == A64X_IMAGE_ARM64 && im->changed == 0) {
        /* plain arm64 with nothing the A57 lacks: keep the file's own mapping (byte-identical, no pager) */
        a64x_image_release(im);
        A64X_INC(a64x_images_native);
        return 0;
    }
    vm_map_size_t stub_size = vm_map_round_page((vm_map_size_t)im->nstub * 4, vm_map_page_mask(map));
    bool stub_mapped = false;
    if (im->nstub) {
        /* back the stub mapping with validated pages of the same file: the executable segment itself */
        im->stub_off = im->text_off & ~(uint64_t)PAGE_MASK;
        if (stub_size > vm_map_round_page(im->text_size, vm_map_page_mask(map))) {
            error = EFBIG;
        }
    }
    if (!error && im->nstub) {
        memory_object_control_t control = ubc_getobject(vp, UBC_FLAGS_NONE);
        vm_map_kernel_flags_t vmk_flags = VM_MAP_KERNEL_FLAGS_FIXED();
        kern_return_t kr = vm_map_enter_mem_object_control(map, &stub_va, stub_size, 0, vmk_flags, control,
                                                           im->stub_off, TRUE, VM_PROT_READ | VM_PROT_EXECUTE,
                                                           VM_PROT_READ | VM_PROT_EXECUTE, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS) {
            /* the address after the image is taken: no stubs, LSE goes to the trap path */
            printf("a64x: no room for %u stub words after the image (kr %d)\n", im->nstub, kr);
            im->nstub = 0;
            bzero(im->cursor, (size_t)(im->npages + 1) * sizeof(uint32_t));
        } else {
            stub_mapped = true;
            OSIncrementAtomic(&im->refs);
            struct pager_crypt_info ci = {a64x_stub_page, a64x_stub_end, im, 0};
            kr = vm_map_apple_protected(map, stub_va, stub_va + stub_size, (vm_object_offset_t)-1, &ci,
                                        CRYPTID_APP_ENCRYPTION);
            if (kr != KERN_SUCCESS) {
                printf("a64x: stub mapping not translated (kr %d)\n", kr);
                if (!os_atomic_load(&im->stub_ended, acquire)) {
                    a64x_image_release(im);
                }
                error = EIO;
            }
        }
    }
    if (!error) {
        OSIncrementAtomic(&im->refs);
        struct pager_crypt_info ci = {a64x_text_page, a64x_text_end, im, 0};
        vm_map_offset_t end = exec_va + vm_map_round_page(im->text_size, vm_map_page_mask(map));
        kern_return_t kr =
            vm_map_apple_protected(map, exec_va, end, (vm_object_offset_t)-1, &ci, CRYPTID_APP_ENCRYPTION);
        if (kr != KERN_SUCCESS) {
            printf("a64x: text not translated (kr %d)\n", kr);
            if (!os_atomic_load(&im->text_ended, acquire)) {
                a64x_image_release(im);
            }
            error = EIO;
        }
    }
    if (error) {
        if (stub_mapped) {
            (void)mach_vm_deallocate(map, stub_va, stub_size);
        }
        printf("a64x: %s: translation failed (%d)\n", vp->v_name ? vp->v_name : "?", error);
        a64x_image_release(im);
        return error;
    }
    A64X_INC(a64x_images);
    A64X_ADD(a64x_stub_words, im->nstub);
    a64x_image_release(im);
    return 0;
}

int a64x_load_image(vm_map_t map, struct vnode *vp, off_t slice_offset, const void *lcmds, size_t lcmds_size,
                    int64_t slide)
{
    const struct mach_header_64 *mh = lcmds;
    struct a64x_layout *lay = kalloc_type(struct a64x_layout, Z_WAITOK);
    if (!lay) {
        return a64x_image_kind(mh->cputype, mh->cpusubtype) == A64X_IMAGE_ARM64E ? ENOMEM : 0;
    }
    int error = a64x_parse(lcmds, lcmds_size, slice_offset, lay);
    if (!error) {
        error = a64x_map_image(map, vp, slice_offset, mh, lay, (vm_map_offset_t)(lay->exec_vmaddr + slide));
    } else if (a64x_image_kind(mh->cputype, mh->cpusubtype) != A64X_IMAGE_ARM64E) {
        /* only arm64e must be translated to run; an arm64 image we cannot lay out runs as is */
        error = 0;
    }
    kfree_type(struct a64x_layout, lay);
    return error;
}

int a64x_mmap_exec(vm_map_t map, struct vnode *vp, off_t file_pos, vm_map_offset_t addr, vm_map_size_t size)
{
    /* dyld maps a dylib's __TEXT (file offset 0 of its slice, the Mach-O header) executable */
    struct mach_header_64 mh;
    int resid = 0;
    if (size < PAGE_SIZE ||
        vn_rdwr(UIO_READ, vp, (caddr_t)&mh, sizeof(mh), file_pos, UIO_SYSSPACE, IO_NODELOCKED,
                vfs_context_ucred(vfs_context_kernel()), &resid, current_proc()) ||
        resid) {
        return 0;
    }
    if (mh.magic != MH_MAGIC_64 || a64x_image_kind(mh.cputype, mh.cpusubtype) == A64X_IMAGE_NATIVE) {
        return 0;
    }
    if (mh.reserved == A64X_PRETRANSLATED) {
        A64X_INC(a64x_images_marked);
        return 0;
    }
    size_t lsize = sizeof(mh) + mh.sizeofcmds;
    if (lsize > 256 * 1024) {
        return EBADMACHO;
    }
    void *buf = kalloc_data(lsize, Z_WAITOK);
    struct a64x_layout *lay = kalloc_type(struct a64x_layout, Z_WAITOK);
    int error = (!buf || !lay) ? ENOMEM : 0;
    if (!error) {
        error = vn_rdwr(UIO_READ, vp, buf, (int)lsize, file_pos, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
    }
    if (!error && resid) {
        error = EBADMACHO;
    }
    if (!error) {
        error = a64x_parse(buf, lsize, file_pos, lay);
    }
    if (!error && (lay->exec_fileoff != (uint64_t)file_pos || lay->exec_filesize > size)) {
        error = ENOTSUP; /* only a mapping of the whole __TEXT from its start is translated */
    }
    if (!error) {
        error = a64x_map_image(map, vp, file_pos, buf, lay, addr);
    }
    if (lay) {
        kfree_type(struct a64x_layout, lay);
    }
    if (buf) {
        kfree_data(buf, lsize);
    }
    if (error && a64x_image_kind(mh.cputype, mh.cpusubtype) != A64X_IMAGE_ARM64E) {
        return 0; /* plain arm64 runs untranslated rather than not at all */
    }
    return error;
}

/*
 * Plan file (built on the host by tools/a64plan, installed in Preboot, hash in /chosen/a64x-plan-sha256):
 *   header, then code ranges (unslid VM addresses, sorted), then a page index (4 KB pages with planned words, sorted),
 *   then the planned words of each page.
 */
struct a64x_plan_hdr {
    char magic[8]; /* "A64XPLN1" */
    uint8_t cache_uuid[16];
    uint32_t nranges, npages;
    uint64_t ranges_off, pages_off, words_off, size;
};

struct a64x_plan_page {
    uint64_t va;           /* unslid, 4 KB aligned */
    uint32_t first, count; /* into the word array */
};

struct a64x_plan_word {
    uint16_t index; /* word in the page */
    uint16_t pad;
    uint32_t word;
};

static const struct a64x_plan_hdr *a64x_plan;
static vm_size_t a64x_plan_size;
static bool a64x_plan_tried;

#define A64X_PLAN_PATH "/private/preboot/a64x/dyld_shared_cache_arm64e.a64xplan"

static void a64x_plan_load(void)
{
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_kernel();
    vm_offset_t buf = 0;
    struct vnode_attr va;
    int error = vnode_open(A64X_PLAN_PATH, FREAD, 0, 0, &vp, ctx);
    if (error) {
        printf("a64x: no cache plan at %s (%d): cache LSE sites will trap\n", A64X_PLAN_PATH, error);
        return;
    }
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vp, &va, ctx);
    vm_size_t size = (vm_size_t)va.va_data_size;
    if (!error && (size < sizeof(struct a64x_plan_hdr) || size > (512u << 20))) {
        error = EINVAL;
    }
    if (!error && kmem_alloc(kernel_map, &buf, size, KMA_DATA | KMA_PAGEABLE, VM_KERN_MEMORY_FILE) != KERN_SUCCESS) {
        error = ENOMEM;
    }
    for (vm_size_t done = 0; !error && done < size;) {
        int resid = 0, chunk = (int)MIN(size - done, 8u << 20);
        error = vn_rdwr(UIO_READ, vp, (caddr_t)(buf + done), chunk, (off_t)done, UIO_SYSSPACE, 0,
                        vfs_context_ucred(ctx), &resid, current_proc());
        done += (vm_size_t)(chunk - resid);
        if (!error && resid == chunk) {
            error = EIO;
        }
    }
    vnode_close(vp, FREAD, ctx);
    /* the plan decides code: it must be the one the boot payload names */
    if (!error) {
        DTEntry chosen;
        void const *want = NULL;
        unsigned int wsize = 0;
        uint8_t got[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha;
        SHA256_Init(&sha);
        SHA256_Update(&sha, (const void *)buf, size);
        SHA256_Final(got, &sha);
        if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
            SecureDTGetProperty(chosen, "a64x-plan-sha256", &want, &wsize) != kSuccess ||
            wsize != SHA256_DIGEST_LENGTH || bcmp(want, got, SHA256_DIGEST_LENGTH)) {
            printf("a64x: cache plan hash does not match /chosen/a64x-plan-sha256: not used\n");
            error = EAUTH;
        }
    }
    const struct a64x_plan_hdr *h = (const void *)buf;
    if (!error && (bcmp(h->magic, "A64XPLN1", 8) || h->size != size ||
                   h->ranges_off + (uint64_t)h->nranges * sizeof(struct a64x_range) > size ||
                   h->pages_off + (uint64_t)h->npages * sizeof(struct a64x_plan_page) > size || h->words_off > size)) {
        error = EINVAL;
    }
    if (error) {
        if (buf) {
            kmem_free(kernel_map, buf, size);
        }
        return;
    }
    a64x_plan = h;
    a64x_plan_size = size;
    printf("a64x: cache plan %u ranges, %u pages\n", h->nranges, h->npages);
}

struct a64x_cache_map {
    uint64_t fileoff, vmaddr;
};

static const struct a64x_range *plan_ranges(void)
{
    return (const void *)((uintptr_t)a64x_plan + a64x_plan->ranges_off);
}

static bool plan_is_code(void *ctx, uint64_t va)
{
    (void)ctx;
    const struct a64x_range *r = plan_ranges();
    uint32_t lo = 0, hi = a64x_plan->nranges;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (r[mid].hi <= va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < a64x_plan->nranges && va >= r[lo].lo;
}

static int a64x_cache_page(const void *src, void *dst, unsigned long long off, void *ops)
{
    const struct a64x_cache_map *cm = ops;
    uint64_t va = cm->vmaddr + (off - cm->fileoff);
    const uint32_t *in = src;
    uint32_t *out = dst;
    bcopy(src, dst, A64X_PAGE);
    if (!a64x_plan) {
        return 0;
    }
    struct a64x_code_map code = {NULL, plan_is_code, 0, NULL};
    /* one-word rewrites; stub-needing words stay unless the plan places them */
    (void)a64x_translate_page(in, out, A64X_WORDS, va, &code, NULL, 0, NULL);
    /* PAC failure checks that pass by construction -> b ok (fully inside this page, as the planner assumed) */
    a64x_dead_checks(in, out, A64X_WORDS, va, &code, NULL, NULL);
    /* planned words: stub pieces in those dead checks, and the site branches */
    const struct a64x_plan_page *pages = (const void *)((uintptr_t)a64x_plan + a64x_plan->pages_off);
    const struct a64x_plan_word *words = (const void *)((uintptr_t)a64x_plan + a64x_plan->words_off);
    uint32_t lo = 0, hi = a64x_plan->npages;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (pages[mid].va < va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < a64x_plan->npages && pages[lo].va == va) {
        const struct a64x_plan_word *w = words + pages[lo].first;
        if ((uintptr_t)(w + pages[lo].count) <= (uintptr_t)a64x_plan + a64x_plan_size) {
            for (uint32_t i = 0; i < pages[lo].count; i++) {
                if (w[i].index < A64X_WORDS) {
                    out[w[i].index] = w[i].word;
                }
            }
        }
    }
    A64X_INC(a64x_cache_pages);
    return 0;
}

static void a64x_cache_map_end(void *ops)
{
    kfree_type(struct a64x_cache_map, ops);
}

/*
 * A cache file translated offline: its cdhash is one of /chosen/a64x-cache-pretranslated's (20-byte CodeDirectory
 * hashes, as trust caches hold them). Every page is validated against that cdhash as any file's, so the file maps as
 * stock XNU maps a cache: no pager, no plan.
 */
static uint32_t a64x_cache_pretranslated_logged;

bool a64x_cache_pretranslated(struct vnode *vp)
{
    DTEntry chosen;
    const void *list;
    unsigned int size;
    if (vp == NULLVP || SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "a64x-cache-pretranslated", &list, &size) != kSuccess || size % CS_CDHASH_LEN) {
        return false;
    }
    struct cs_blob *blob = ubc_cs_blob_get(vp, CPU_TYPE_ANY, CPU_SUBTYPE_ANY, 0);
    if (blob == NULL) {
        return false;
    }
    for (unsigned int at = 0; at < size; at += CS_CDHASH_LEN) {
        if (memcmp((const uint8_t *)list + at, blob->csb_cdhash, CS_CDHASH_LEN) == 0) {
            if (!os_atomic_xchg(&a64x_cache_pretranslated_logged, 1, relaxed)) {
                printf("a64x: shared cache translated offline (%u cdhashes listed): mapped stock\n",
                       size / CS_CDHASH_LEN);
            }
            return true;
        }
    }
    return false;
}

void *a64x_shared_cache_pager(void *file_object, uint64_t file_offset, uint64_t size, uint64_t vmaddr, struct vnode *vp)
{
    (void)vp;
    lck_mtx_lock(&a64x_plan_lock);
    if (!a64x_plan_tried) {
        a64x_plan_tried = true;
        a64x_plan_load();
    }
    lck_mtx_unlock(&a64x_plan_lock);
    struct a64x_cache_map *cm = kalloc_type(struct a64x_cache_map, Z_WAITOK);
    if (!cm) {
        return MEMORY_OBJECT_NULL;
    }
    cm->fileoff = file_offset;
    cm->vmaddr = vmaddr;
    struct pager_crypt_info ci = {a64x_cache_page, a64x_cache_map_end, cm, 0};
    memory_object_t mo = apple_protect_pager_setup((vm_object_t)file_object, file_offset, file_offset, &ci, 0,
                                                   vm_map_round_page(size, PAGE_MASK), TRUE);
    if (mo == MEMORY_OBJECT_NULL) {
        kfree_type(struct a64x_cache_map, cm);
    }
    return mo;
}

extern int a64x_copyio_rmw(user_addr_t uaddr, uint32_t size, uint32_t op, uint64_t operand, uint64_t *old);
extern int a64x_copyio_cas(user_addr_t uaddr, uint32_t size, uint64_t expected, uint64_t desired, uint64_t *old);
extern int a64x_copyio_casp(user_addr_t uaddr, uint64_t e0, uint64_t e1, uint64_t d0, uint64_t d1, uint64_t *o0,
                            uint64_t *o1);
extern bool a64x_pc_is_jit(user_addr_t pc);
extern bool a64x_user_neon_d(uint32_t regno, uint64_t *out);
extern int switch_user_cache_maintenance(user_addr_t va, vm_size_t len, uint32_t op);
extern kern_return_t exception_triage(exception_type_t exception, mach_exception_data_t code,
                                      mach_msg_type_number_t codeCnt);

static uint64_t reg(arm_saved_state_t *s, uint32_t r)
{
    return r == 31 ? 0 : get_saved_state_reg(s, r);
}

static void set_reg(arm_saved_state_t *s, uint32_t r, uint64_t v)
{
    if (r != 31) {
#if XNU_12377_121_6
        set_user_saved_state_reg(s, r, v);
#else
        set_saved_state_reg(s, r, v);
#endif
    }
}

static void set_pc(arm_saved_state_t *s, uint64_t pc)
{
#if XNU_12377_121_6
    set_user_saved_state_pc(s, (register_t)pc);
#else
    set_saved_state_pc(s, (register_t)pc);
#endif
}

static void set_lr(arm_saved_state_t *s, uint64_t lr)
{
#if XNU_12377_121_6
    set_user_saved_state_lr(s, (register_t)lr);
#else
    set_saved_state_lr(s, (register_t)lr);
#endif
}

static void set_nzcv_z(arm_saved_state_t *s, bool z)
{
#if XNU_12377_121_6
    mask_user_saved_state_cpsr(s, z ? PSR64_Z : 0, PSR64_NZCV_MASK);
#else
    mask_saved_state_cpsr(s, z ? PSR64_Z : 0, PSR64_NZCV_MASK);
#endif
}

int a64x_emulate_user(arm_saved_state_t *state, uint32_t w)
{
    enum a64x_class c = a64x_classify(w);
    user_addr_t pc = get_saved_state_pc(state);
    uint32_t out = 0;
    bool is_jit = false;

    if (!(pc & 3) && a64x_rewrite1(w, &out)) {
        is_jit = a64x_pc_is_jit(pc);
        if (is_jit) {
            uint64_t patched_old = 0;
            if (a64x_copyio_cas(pc, 2, w, out, &patched_old) == 0 &&
                switch_user_cache_maintenance(pc, sizeof(uint32_t), SWITCH_UCM_SYNC_I) == 0) {
                if ((uint32_t)patched_old == w) {
                    A64X_INC(a64x_jit_patches);
                }
                switch_a64x_trap_noted(c, true);
                return 1;
            }
        }
    }

    uint32_t rs = (w >> 16) & 31, rn = (w >> 5) & 31, rt = w & 31, size = w >> 30;
    user_addr_t addr = rn == 31 ? get_saved_state_sp(state) : get_saved_state_reg(state, rn);
    user_addr_t fault_addr = addr;
    uint64_t old = 0, old2 = 0;
    int error;

    switch (c) {
    case A64X_LSE_LDOP:
    case A64X_LSE_SWP: {
        uint32_t op = (w & (1u << 15)) ? 8 : (w >> 12) & 7;
        error = a64x_copyio_rmw(addr, size, op, reg(state, rs), &old);
        if (!error) {
            set_reg(state, rt, old);
        }
        break;
    }
    case A64X_LSE_CAS:
        error = a64x_copyio_cas(addr, size, reg(state, rs), reg(state, rt), &old);
        if (!error) {
            set_reg(state, rs, old);
        }
        break;
    case A64X_LSE_CASP:
        if ((rs & 1) || (rt & 1)) {
            return 0;
        }
        if (w & (1u << 30)) {
            error = a64x_copyio_casp(addr, reg(state, rs), reg(state, rs + 1), reg(state, rt), reg(state, rt + 1), &old,
                                     &old2);
            if (!error) {
                set_reg(state, rs, old);
                set_reg(state, rs + 1, old2);
            }
        } else {
            /* 32-bit pair = one 64-bit compare-and-swap (little-endian: first register is the low word) */
            uint64_t e = (reg(state, rs) & 0xFFFFFFFFull) | (reg(state, rs + 1) << 32);
            uint64_t d = (reg(state, rt) & 0xFFFFFFFFull) | (reg(state, rt + 1) << 32);
            error = a64x_copyio_cas(addr, 3, e, d, &old);
            if (!error) {
                set_reg(state, rs, old & 0xFFFFFFFFull);
                set_reg(state, rs + 1, old >> 32);
            }
        }
        break;
    case A64X_JSCVT: {
        uint64_t bits = 0;
        if (!a64x_user_neon_d(rn, &bits)) {
            return 0;
        }
        uint32_t res = 0;
        bool exact = a64x_eval_jscvt(bits, &res);
        set_reg(state, rt, res);
        set_nzcv_z(state, exact);
        error = 0;
        break;
    }
    case A64X_PAC_LDRA: {
        int32_t imm = 0;
        bool wb = false;
        uint64_t val = 0;
        a64x_decode_ldra(w, &imm, &wb);
        user_addr_t ea = addr + (int64_t)imm;
        user_addr_t load_ea = (ea & (1ull << 55)) ? ea : (ea & ~0xFF00000000000000ull);
        fault_addr = load_ea;
        /* Single-copy atomic 64-bit load on 8-byte-aligned addresses; LDRAA/LDRAB allows unaligned loads. */
        error = (load_ea & 7) == 0 ? copyin_atomic64(load_ea, &val) : copyin(load_ea, &val, sizeof(val));
        if (!error) {
            if (wb) {
                if (rn == 31) {
                    set_saved_state_sp(state, (register_t)ea);
                } else {
                    set_reg(state, rn, ea);
                }
            }
            set_reg(state, rt, val);
        }
        break;
    }
    case A64X_RCPC_LDAPR: {
        uint32_t bytes = 1u << size;
        uint64_t val = 0;
        if (bytes == 8 && (addr & 7) == 0) {
            error = copyin_atomic64(addr, &val);
        } else if (bytes == 4 && (addr & 3) == 0) {
            uint32_t v32 = 0;
            error = copyin_atomic32(addr, &v32);
            val = v32;
        } else {
            error = copyin(addr, &val, bytes);
        }
        if (!error) {
            __builtin_arm_dmb(DMB_ISHLD);
            set_reg(state, rt, val);
        }
        break;
    }
    case A64X_LOR: {
        uint32_t bytes = 1u << size;
        if (w & (1u << 22)) {
            uint64_t val = 0;
            if (bytes == 8 && (addr & 7) == 0) {
                error = copyin_atomic64(addr, &val);
            } else if (bytes == 4 && (addr & 3) == 0) {
                uint32_t v32 = 0;
                error = copyin_atomic32(addr, &v32);
                val = v32;
            } else {
                error = copyin(addr, &val, bytes);
            }
            if (!error) {
                __builtin_arm_dmb(DMB_ISHLD);
                set_reg(state, rt, val);
            }
        } else {
            uint64_t val = reg(state, rt);
            __builtin_arm_dmb(DMB_ISH);
            if (bytes == 8 && (addr & 7) == 0) {
                error = copyout_atomic64(val, addr);
            } else if (bytes == 4 && (addr & 3) == 0) {
                error = copyout_atomic32((uint32_t)val, addr);
            } else {
                error = copyout(&val, addr, bytes);
            }
        }
        break;
    }
    case A64X_DPB: {
        user_addr_t va = (user_addr_t)reg(state, rt);
        fault_addr = va;
        error = switch_user_cache_maintenance(va, 1, SWITCH_UCM_DC_CVAC);
        break;
    }
    case A64X_SB:
        __builtin_arm_dsb(DSB_NSH);
        __builtin_arm_isb(ISB_SY);
        error = 0;
        break;
    case A64X_PAC_DP:
    case A64X_PAC_XPAC:
        if (!is_jit) {
            goto unhandled;
        }
        error = 0;
        break;
    case A64X_PAC_PACGA:
        if (!is_jit) {
            goto unhandled;
        }
        set_reg(state, rt, 0);
        error = 0;
        break;
    case A64X_PAC_RET:
        if (!is_jit) {
            goto unhandled;
        }
        set_pc(state, get_saved_state_lr(state));
        A64X_INC(a64x_traps);
        switch_a64x_trap_noted(c, false);
        return 1;
    case A64X_PAC_BR:
        if (!is_jit) {
            goto unhandled;
        }
        set_pc(state, reg(state, rn));
        A64X_INC(a64x_traps);
        switch_a64x_trap_noted(c, false);
        return 1;
    case A64X_PAC_BLR: {
        if (!is_jit) {
            goto unhandled;
        }
        uint64_t target = reg(state, rn);
        set_lr(state, pc + 4);
        set_pc(state, target);
        A64X_INC(a64x_traps);
        switch_a64x_trap_noted(c, false);
        return 1;
    }
    default:
    unhandled:
        {
            A64X_INC(a64x_traps_unhandled);
            static int logged;
            if (logged < 32) {
                logged++;
                printf("a64x: %s[%d] pc 0x%llx: %08x (%s) not emulated\n", current_proc()->p_comm,
                       proc_getpid(current_proc()), pc, w, a64x_class_name(c));
            }
            return 0;
        }
    }
    if (error) {
        if (error == EFAULT) {
            mach_exception_data_type_t codes[2] = {KERN_INVALID_ADDRESS, (mach_exception_data_type_t)fault_addr};
            exception_triage(EXC_BAD_ACCESS, codes, 2);
            __builtin_unreachable();
        }
        return 0;
    }
#if XNU_12377_121_6
    add_user_saved_state_pc(state, 4);
#else
    add_saved_state_pc(state, 4);
#endif
    A64X_INC(a64x_traps);
    switch_a64x_trap_noted(c, false);
    return 1;
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
SYSCTL_NODE(_kern, OID_AUTO, a64x, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, 0, "arm64e translation");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_images, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, text_pages, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_pages, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, cache_pages, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_cache_pages, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, stub_words, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_stub_words, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, stub_mismatch, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_mismatch, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, traps, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_traps, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, traps_unhandled, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_traps_unhandled,
            "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, jit_patches, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_jit_patches, "");
/* arm64 images that needed nothing (mapped as is) and images marked as built for this core */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_native, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_images_native, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_marked, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_images_marked, "");
/* images of installed staged system apps mapped from the pretranslated tree */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_installed, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_images_installed,
            "");
/* words of __text found to be data (assembler tables) and left untranslated */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, island_words, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &a64x_island_words, "");
#endif

bool switch_a64x_mmap_refused(vm_map_t map, struct vnode *vp, off_t file_pos, vm_map_offset_t addr, vm_map_size_t size)
{
    if (a64x_mmap_exec(map, vp, file_pos, addr, size) != 0) {
        (void)mach_vm_deallocate(map, addr, size);
        return true;
    }
    return false;
}

#endif /* CONFIG_A64X */
