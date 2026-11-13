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

#define A64X_PAGE     4096u
#define A64X_WORDS    (A64X_PAGE / 4)
#define A64X_BRK_STUB 0xD4200020u /* brk #1: never executed (no branch reaches unused stub words) */

/* statistics (sysctl kern.a64x.*) */
static uint64_t a64x_images, a64x_pages, a64x_stub_words, a64x_mismatch, a64x_traps, a64x_traps_unhandled;
static uint64_t a64x_cache_pages, a64x_images_native, a64x_images_marked, a64x_island_words;
static uint64_t a64x_images_installed;

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
    struct a64x_range *ranges; /* instruction sections, absolute file offsets, sorted */
    uint32_t map_flags;        /* A64X_MAP_NO_PAC for plain arm64 */
    uint64_t changed;          /* words the scan rewrote */
    uint8_t *data;             /* [npages * 128]: 1 bit per word of the segment, set = data in __text */
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
        kfree_data(im->cursor, (im->npages + 1) * sizeof(uint32_t));
    }
    if (im->stubs) {
        kfree_data(im->stubs, im->stubcap * sizeof(uint32_t));
    }
    if (im->ranges) {
        kfree_data(im->ranges, im->nranges * sizeof(struct a64x_range));
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
    size_t chk_bytes = (c1 - c0) * sizeof(uint32_t), scratch_bytes = chk_bytes + A64X_WALK_BYTES(A64X_WORDS);
    uint32_t *chk = NULL;
    struct a64x_walk walk = {NULL, NULL, 0};
    bool ok = true;
    if (c1 > c0) {
        chk = kalloc_data(scratch_bytes, Z_NOWAIT);
        ok = chk != NULL;
        if (ok) {
            walk = a64x_walk_in(chk + (c1 - c0), A64X_WORDS);
        }
    }
    if (ok) {
        struct a64x_stub_region sr = {im->text_off + (uint64_t)im->stub_delta, chk, c1, c0, &walk};
        uint32_t end = a64x_translate_page(src, dst, A64X_WORDS, off, &cm, c1 > c0 ? &sr : NULL, c0, NULL);
        if (end != c1 || (chk && bcmp(chk, im->stubs + c0, chk_bytes))) {
            ok = false;
            OSIncrementAtomic64((SInt64 *)&a64x_mismatch);
            printf("a64x: text page 0x%llx: stubs differ from the validated words; leaving LSE to the trap path\n",
                   off);
        }
    }
    if (!ok) {
        /* one-word rewrites only: what needs a stub keeps its original word */
        (void)a64x_translate_page(src, dst, A64X_WORDS, off, &cm, NULL, 0, NULL);
    }
    if (chk) {
        kfree_data(chk, scratch_bytes);
    }
    OSIncrementAtomic64((SInt64 *)&a64x_pages);
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
struct a64x_layout {
    uint64_t exec_fileoff, exec_filesize, exec_vmaddr;
    uint64_t vm_end;
    uint32_t nsect;
    bool marked;                               /* has the __TEXT,__a64x_target section: built for this core */
    uint64_t text_vmaddr;                      /* __TEXT (function starts count from it) */
    uint32_t fs_off, fs_size;                  /* LC_FUNCTION_STARTS, slice-relative */
    uint32_t dic_off, dic_size;                /* LC_DATA_IN_CODE, slice-relative */
    uint32_t cs_off, cs_size;                  /* LC_CODE_SIGNATURE, slice-relative */
    struct a64x_range sect[A64X_MAX_SECTIONS]; /* instruction sections, absolute file offsets, file order */
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
                lay->exec_fileoff = (uint64_t)slice_off + sg->fileoff;
                lay->exec_filesize = sg->filesize;
                lay->exec_vmaddr = sg->vmaddr;
                const struct section_64 *sc = (const void *)(sg + 1);
                for (uint32_t s = 0; s < sg->nsects; s++) {
                    if ((sc[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) && sc[s].size) {
                        if (lay->nsect >= A64X_MAX_SECTIONS) {
                            return ENOTSUP;
                        }
                        lay->sect[lay->nsect].lo = (uint64_t)slice_off + sc[s].offset;
                        lay->sect[lay->nsect].hi = (uint64_t)slice_off + sc[s].offset + sc[s].size;
                        lay->nsect++;
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
static void a64x_find_islands(struct a64x_image *im, const struct a64x_layout *lay, off_t slice_off)
{
    size_t nbits = (size_t)im->npages * A64X_WORDS, nbytes = nbits / 8;
    uint8_t *fs = NULL;
    uint32_t *page = kalloc_data(A64X_PAGE, Z_WAITOK);
    im->data = kalloc_data(nbytes, Z_WAITOK | Z_ZERO);
    if (!page || !im->data) {
        goto out;
    }
    int resid = 0;
    if (lay->fs_size && lay->fs_size <= (1u << 24)) {
        uint8_t *buf = kalloc_data(lay->fs_size, Z_WAITOK);
        fs = kalloc_data(nbytes, Z_WAITOK | Z_ZERO);
        if (buf && fs &&
            !vn_rdwr(UIO_READ, im->vp, (caddr_t)buf, (int)lay->fs_size, slice_off + lay->fs_off, UIO_SYSSPACE,
                     IO_NODELOCKED, vfs_context_ucred(vfs_context_kernel()), &resid, current_proc()) &&
            !resid) {
            uint64_t addr = lay->text_vmaddr;
            for (uint32_t q = 0; q < lay->fs_size;) {
                uint64_t v = 0;
                int sh = 0;
                while (q < lay->fs_size && sh < 64) {
                    uint8_t b = buf[q++];
                    v |= (uint64_t)(b & 0x7F) << sh;
                    sh += 7;
                    if (!(b & 0x80)) {
                        break;
                    }
                }
                if (!v) {
                    break;
                }
                addr += v;
                if (addr >= lay->exec_vmaddr) {
                    uint64_t k = (addr - lay->exec_vmaddr) / 4;
                    if (k < nbits) {
                        fs[k >> 3] |= (uint8_t)(1u << (k & 7));
                    }
                }
            }
        }
        if (buf) {
            kfree_data(buf, lay->fs_size);
        }
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
    for (uint32_t pg = 0; pg < im->npages; pg++) {
        uint64_t off = im->text_off + (uint64_t)pg * A64X_PAGE;
        bzero(page, A64X_PAGE);
        if (vn_rdwr(UIO_READ, im->vp, (caddr_t)page, A64X_PAGE, (off_t)off, UIO_SYSSPACE, IO_NODELOCKED,
                    vfs_context_ucred(vfs_context_kernel()), &resid, current_proc())) {
            break;
        }
        a64x_islands_page(&is, page, A64X_WORDS, (uint64_t)pg * A64X_WORDS, off, &cm);
    }
    a64x_islands_end(&is, nbits);
    im->data = dic_only;
    if (is.runs) {
        OSAddAtomic64((SInt64)is.words, (SInt64 *)&a64x_island_words);
    }
out:
    if (fs) {
        kfree_data(fs, nbytes);
    }
    if (page) {
        kfree_data(page, A64X_PAGE);
    }
}

/* read the executable segment once and lay out its stubs; stub_delta must be fixed before */
static int a64x_scan(struct a64x_image *im)
{
    uint32_t *page = kalloc_data(A64X_PAGE, Z_WAITOK);
    uint32_t *out = kalloc_data(A64X_PAGE, Z_WAITOK);
    void *walk_buf = kalloc_data(A64X_WALK_BYTES(A64X_WORDS), Z_WAITOK | Z_NOFAIL);
    struct a64x_walk walk = a64x_walk_in(walk_buf, A64X_WORDS);
    struct a64x_code_map cm = {im, img_is_code, im->map_flags, NULL};
    uint32_t cursor = 0;
    int error = 0;
    for (uint32_t pg = 0; pg < im->npages && !error; pg++) {
        int resid = 0;
        uint64_t off = im->text_off + (uint64_t)pg * A64X_PAGE;
        bzero(page, A64X_PAGE);
        error = vn_rdwr(UIO_READ, im->vp, (caddr_t)page, A64X_PAGE, (off_t)off, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
        if (error) {
            break;
        }
        if (im->stubcap - cursor < A64X_WORDS * A64X_STUB_MAX) {
            uint32_t ncap = im->stubcap ? im->stubcap * 2 : 4 * A64X_WORDS * A64X_STUB_MAX;
            uint32_t *n = kalloc_data(ncap * sizeof(uint32_t), Z_WAITOK);
            if (!n) {
                error = ENOMEM;
                break;
            }
            if (im->stubs) {
                bcopy(im->stubs, n, cursor * sizeof(uint32_t));
                kfree_data(im->stubs, im->stubcap * sizeof(uint32_t));
            }
            im->stubs = n;
            im->stubcap = ncap;
        }
        struct a64x_stub_region sr = {im->text_off + (uint64_t)im->stub_delta, im->stubs, im->stubcap, 0, &walk};
        im->cursor[pg] = cursor;
        cursor = a64x_translate_page(page, out, A64X_WORDS, off, &cm, &sr, cursor, NULL);
        for (uint32_t i = 0; i < A64X_WORDS; i++) {
            im->changed += out[i] != page[i];
        }
    }
    im->cursor[im->npages] = cursor;
    im->nstub = cursor;
    kfree_data(page, A64X_PAGE);
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
    OSIncrementAtomic64((SInt64 *)&a64x_images_installed);
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
        OSIncrementAtomic64((SInt64 *)&a64x_images_marked);
        return 0;
    }
    if (kind == A64X_IMAGE_ARM64E) {
        int error = a64x_map_translation(map, vp, slice_off, lay, exec_va);
        if (error != ENOENT) {
            return error;
        }
    }
    struct a64x_image *im = kalloc_type(struct a64x_image, Z_WAITOK | Z_ZERO);
    im->map_flags = kind == A64X_IMAGE_ARM64 ? A64X_MAP_NO_PAC : 0;
    im->refs = 2; /* one per pager kind (text, stubs); each crypt_end drops one */
    im->text_off = lay->exec_fileoff;
    im->text_size = lay->exec_filesize;
    im->npages = (uint32_t)((lay->exec_filesize + A64X_PAGE - 1) / A64X_PAGE);
    im->cursor = kalloc_data((im->npages + 1) * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
    im->nranges = lay->nsect;
    im->ranges = kalloc_data(lay->nsect * sizeof(struct a64x_range), Z_WAITOK);
    bcopy(lay->sect, im->ranges, lay->nsect * sizeof(struct a64x_range));
    /* sections are in file order already; keep the table sorted for the binary search */
    for (uint32_t i = 1; i < im->nranges; i++) {
        for (uint32_t j = i; j > 0 && im->ranges[j - 1].lo > im->ranges[j].lo; j--) {
            struct a64x_range t = im->ranges[j];
            im->ranges[j] = im->ranges[j - 1];
            im->ranges[j - 1] = t;
        }
    }
    if (vnode_ref(vp)) {
        im->refs = 1;
        a64x_image_release(im);
        return EIO;
    }
    im->vp = vp;

    /* stubs go right after the image (same distance in every process) */
    vm_map_offset_t image_end = exec_va + (lay->vm_end - lay->exec_vmaddr);
    vm_map_offset_t stub_va = vm_map_round_page(image_end, vm_map_page_mask(map));
    im->stub_delta = (int64_t)(stub_va - exec_va);
    a64x_find_islands(im, lay, slice_off);
    int error = a64x_scan(im);
    if (!error && kind == A64X_IMAGE_ARM64 && im->changed == 0) {
        /* plain arm64 with nothing the A57 lacks: keep the file's own mapping (byte-identical, no pager) */
        im->refs = 1;
        a64x_image_release(im);
        OSIncrementAtomic64((SInt64 *)&a64x_images_native);
        return 0;
    }
    vm_map_size_t stub_size = vm_map_round_page((vm_map_size_t)im->nstub * 4, vm_map_page_mask(map));
    if (!error && im->nstub) {
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
            bzero(im->cursor, (im->npages + 1) * sizeof(uint32_t));
        } else {
            struct pager_crypt_info ci = {a64x_stub_page, a64x_image_release, im, 0};
            kr = vm_map_apple_protected(map, stub_va, stub_va + stub_size, (vm_object_offset_t)-1, &ci,
                                        CRYPTID_APP_ENCRYPTION);
            if (kr != KERN_SUCCESS) {
                printf("a64x: stub mapping not translated (kr %d)\n", kr);
                error = EIO;
            }
        }
    }
    if (!im->nstub) {
        a64x_image_release(im); /* the stub pager's reference: there is none */
    }
    if (!error) {
        struct pager_crypt_info ci = {a64x_text_page, a64x_image_release, im, 0};
        vm_map_offset_t end = exec_va + vm_map_round_page(im->text_size, vm_map_page_mask(map));
        kern_return_t kr =
            vm_map_apple_protected(map, exec_va, end, (vm_object_offset_t)-1, &ci, CRYPTID_APP_ENCRYPTION);
        if (kr != KERN_SUCCESS) {
            printf("a64x: text not translated (kr %d)\n", kr);
            error = EIO;
        }
    }
    if (error) {
        printf("a64x: %s: translation failed (%d)\n", vp->v_name ? vp->v_name : "?", error);
        /* references held by pagers that exist are dropped by their crypt_end; a failed setup leaks im */
        return error;
    }
    OSIncrementAtomic64((SInt64 *)&a64x_images);
    OSAddAtomic64((SInt64)im->nstub, (SInt64 *)&a64x_stub_words);
    return 0;
}

int a64x_load_image(vm_map_t map, struct vnode *vp, off_t slice_offset, const void *lcmds, size_t lcmds_size,
                    int64_t slide)
{
    const struct mach_header_64 *mh = lcmds;
    struct a64x_layout *lay = kalloc_type(struct a64x_layout, Z_WAITOK | Z_NOFAIL);
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
        OSIncrementAtomic64((SInt64 *)&a64x_images_marked);
        return 0;
    }
    size_t lsize = sizeof(mh) + mh.sizeofcmds;
    if (lsize > 256 * 1024) {
        return EBADMACHO;
    }
    void *buf = kalloc_data(lsize, Z_WAITOK);
    int error = vn_rdwr(UIO_READ, vp, buf, (int)lsize, file_pos, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
    if (!error && resid) {
        error = EBADMACHO;
    }
    struct a64x_layout *lay = kalloc_type(struct a64x_layout, Z_WAITOK | Z_NOFAIL);
    if (!error) {
        error = a64x_parse(buf, lsize, file_pos, lay);
    }
    if (!error && (lay->exec_fileoff != (uint64_t)file_pos || lay->exec_filesize > size)) {
        error = ENOTSUP; /* only a mapping of the whole __TEXT from its start is translated */
    }
    if (!error) {
        error = a64x_map_image(map, vp, file_pos, buf, lay, addr);
    }
    kfree_type(struct a64x_layout, lay);
    kfree_data(buf, lsize);
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
    OSIncrementAtomic64((SInt64 *)&a64x_cache_pages);
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

int a64x_emulate_user(arm_saved_state_t *state, uint32_t w)
{
    enum a64x_class c = a64x_classify(w);
    uint32_t rs = (w >> 16) & 31, rn = (w >> 5) & 31, rt = w & 31, size = w >> 30;
    user_addr_t addr = rn == 31 ? get_saved_state_sp(state) : get_saved_state_reg(state, rn);
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
    default: {
        /*
		 * Not emulated: pointer authentication (by design: it is rewritten, never trapped; JIT is refused so no
		 * untranslated arm64e code exists), FP16/FCMA/FJCVTZS/RDM (the translators give them stubs; here only a
		 * marked image breaking its promise or a form without a stub arrives), and A13+ features (code that uses
		 * them checked hw.optional first). One line each for the first ones, then SIGILL as before.
		 */
        OSIncrementAtomic64((SInt64 *)&a64x_traps_unhandled);
        static int logged;
        if (logged < 32) {
            logged++;
            printf("a64x: %s[%d] pc 0x%llx: %08x (%s) not emulated\n", current_proc()->p_comm,
                   proc_getpid(current_proc()), get_saved_state_pc(state), w, a64x_class_name(c));
        }
        return 0;
    }
    }
    if (error) {
        return 0; /* the fault is reported as the undefined instruction it was */
    }
#if XNU_12377_121_6
    add_user_saved_state_pc(state, 4);
#else
    add_saved_state_pc(state, 4);
#endif
    OSIncrementAtomic64((SInt64 *)&a64x_traps);
    return 1;
}

SYSCTL_NODE(_kern, OID_AUTO, a64x, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "arm64e translation");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_images, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, text_pages, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_pages, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, cache_pages, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_cache_pages, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, stub_words, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_stub_words, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, stub_mismatch, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_mismatch, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, traps, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_traps, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, traps_unhandled, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_traps_unhandled, "");
/* arm64 images that needed nothing (mapped as is) and images marked as built for this core */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_native, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_images_native, "");
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_marked, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_images_marked, "");
/* images of installed staged system apps mapped from the pretranslated tree */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, images_installed, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_images_installed, "");
/* words of __text found to be data (assembler tables) and left untranslated */
SYSCTL_QUAD(_kern_a64x, OID_AUTO, island_words, CTLFLAG_RD | CTLFLAG_LOCKED, &a64x_island_words, "");

bool switch_a64x_mmap_refused(vm_map_t map, struct vnode *vp, off_t file_pos, vm_map_offset_t addr, vm_map_size_t size)
{
    if (a64x_mmap_exec(map, vp, file_pos, addr, size) != 0) {
        (void)mach_vm_deallocate(map, addr, size);
        return true;
    }
    return false;
}

#endif /* CONFIG_A64X */
