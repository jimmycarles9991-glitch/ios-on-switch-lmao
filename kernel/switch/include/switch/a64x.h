// SPDX-License-Identifier: GPL-2.0-only
/*
 * a64x: run arm64e (Apple A12, ARMv8.3) user code on an ARMv8.0 core (Cortex-A57) by translating it at load time.
 *
 * Every executable page of an arm64e image is produced by an apple_protect pager whose "decrypt" step is the
 * translator: the source page is first validated against the image's code signature (vm_page_validate_cs_mapped in
 * apple_protect_pager_data_request), then rewritten. Pointer authentication is the identity; LSE atomics and other
 * multi-word cases branch to stubs generated from the same validated words.
 */
#ifndef SWITCH_A64X_H
#define SWITCH_A64X_H

#ifdef XNU_KERNEL_PRIVATE
#if CONFIG_A64X

#include <sys/types.h>
#include <sys/vnode.h>
#include <mach/vm_types.h>
#include <mach/machine.h>
#include <mach-o/loader.h>

struct arm_saved_state;

/* true for arm64e: the subtype whose code is always translated (PAC and all); also: its JIT output is not */
static inline int a64x_subtype_translated(cpu_type_t type, cpu_subtype_t subtype)
{
    return type == CPU_TYPE_ARM64 &&
           ((subtype & CPU_SUBTYPE_PTRAUTH_ABI) || (subtype & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E);
}

/*
 * What the loader does with an image:
 *   A64X_IMAGE_ARM64E  arm64e: full translation (pointer authentication as the identity, LSE, FP16, ...).
 *   A64X_IMAGE_ARM64   plain arm64 (ALL or V8): scanned at load; only what the A57 lacks is translated (no PAC
 *                      rewrites). An image with nothing to translate is mapped as is: byte-identical, no pager.
 *   A64X_IMAGE_NATIVE  anything else: untouched.
 * An image carrying the target marker (a section __TEXT,__a64x_target, see A64X_TARGET_SECTNAME) is mapped as is
 * whatever its subtype: it was built for this core (-march=armv8-a -mcpu=cortex-a57, no PAC, no LSE). It is not
 * scanned; if it still executes an instruction the A57 lacks, the undefined-instruction path emulates LSE (counted
 * in kern.a64x.traps) and reports anything else. Apple's binaries translated offline (translate/arm64e pretranslate)
 * have no room for that section after their load commands: they carry A64X_PRETRANSLATED in mach_header_64.reserved
 * instead (0 in every Apple binary), with the same meaning.
 */
#define A64X_IMAGE_NATIVE    0
#define A64X_IMAGE_ARM64E    1
#define A64X_IMAGE_ARM64     2
#define A64X_TARGET_SEGNAME  "__TEXT"
#define A64X_TARGET_SECTNAME "__a64x_target"
#define A64X_PRETRANSLATED   0x78343661u

static inline int a64x_image_kind(cpu_type_t type, cpu_subtype_t subtype)
{
    if (type != CPU_TYPE_ARM64) {
        return A64X_IMAGE_NATIVE;
    }
    /*
	 * arm64e is the pointer-authentication capability bit, not a subtype value: a fat arm64e binary carries one
	 * slice per ptrauth ABI version (CPU_SUBTYPE_ARM64E and the versioned ones, whose low byte is not 2), and
	 * every one holds PAC. Grade them all as arm64e, so whichever the loader picks is translated.
	 */
    if (subtype & CPU_SUBTYPE_PTRAUTH_ABI) {
        return A64X_IMAGE_ARM64E;
    }
    switch (subtype & ~CPU_SUBTYPE_MASK) {
    case CPU_SUBTYPE_ARM64E:
        return A64X_IMAGE_ARM64E;
    case CPU_SUBTYPE_ARM64_ALL:
    case CPU_SUBTYPE_ARM64_V8:
        return A64X_IMAGE_ARM64;
    default:
        return A64X_IMAGE_NATIVE;
    }
}

/*
 * After mach_loader mapped an arm64e or arm64 image (main executable or dyld): translate its executable segment as
 * a64x_image_kind() and the target marker say (nothing, for a marked image or a clean arm64 one).
 * `lcmds` holds the mach_header_64 and its load commands as read from the file.
 */
int a64x_load_image(vm_map_t map, struct vnode *vp, off_t slice_offset, const void *lcmds, size_t lcmds_size,
                    int64_t slide);

/*
 * After mmap() mapped an executable range of a file (dyld mapping a dylib's __TEXT): if the range starts with an
 * arm64e or arm64 Mach-O header, translate it as above. Returns 0 (nothing to do, or translated) or an errno for
 * mmap to fail with.
 */
int a64x_mmap_exec(vm_map_t map, struct vnode *vp, off_t file_pos, vm_map_offset_t addr, vm_map_size_t size);

/*
 * The dyld shared cache: memory object to map instead of the file's pager for an executable cache mapping, or
 * MEMORY_OBJECT_NULL if the cache cannot be translated (then the mapping must fail: untranslated arm64e text
 * cannot run here).
 */
bool a64x_cache_pretranslated(struct vnode *vp);
void *a64x_shared_cache_pager(void *file_object, uint64_t file_offset, uint64_t size, uint64_t vmaddr,
                              struct vnode *vp);

/* Undefined-instruction fallback (EL0): emulate what the static rewriter left; true if handled. */
int a64x_emulate_user(struct arm_saved_state *state, uint32_t instr);

/* kern_mman: an arm64e mapping that cannot be translated fails (it would fault on its first RETAB). */
bool switch_a64x_mmap_refused(vm_map_t map, struct vnode *vp, off_t file_pos, vm_map_offset_t addr, vm_map_size_t size);

/*
 * exec's apple[] for an arm64e main executable. With no PAC hardware its code runs translated, pointer authentication
 * the identity: what it does with its keys off. dyld then grades for keys off (arm64e first, then arm64), so it loads
 * our plain arm64 plug-ins into Apple's processes as it loads arm64 code into any keys-off process.
 */
#define SWITCH_A64X_PTRAUTH_DISABLED "ptrauth_disabled=1"

static inline bool switch_a64x_keys_off(cpu_type_t type, cpu_subtype_t subtype)
{
    return a64x_subtype_translated(type, subtype);
}

/* load_machfile: arm64e text runs only translated; arm64 text is scanned, and translated if it needs it. */
static inline bool switch_a64x_load_refused(const struct mach_header *header, vm_map_t map, struct vnode *vp,
                                            off_t slice_offset, const void *lcmds, size_t lcmds_size, int64_t slide)
{
    return a64x_image_kind(header->cputype, header->cpusubtype) != A64X_IMAGE_NATIVE &&
           a64x_load_image(map, vp, slice_offset, lcmds, lcmds_size, slide) != 0;
}

#else /* CONFIG_A64X */
struct mach_header;
struct vnode;

static inline int a64x_subtype_translated(int type, int subtype)
{
    (void)type, (void)subtype;
    return 0;
}

#define SWITCH_A64X_PTRAUTH_DISABLED ""

static inline bool switch_a64x_keys_off(int type, int subtype)
{
    (void)type, (void)subtype;
    return false;
}

static inline bool switch_a64x_mmap_refused(void *map, struct vnode *vp, long long file_pos, unsigned long long addr,
                                            unsigned long long size)
{
    (void)map, (void)vp, (void)file_pos, (void)addr, (void)size;
    return false;
}

static inline bool switch_a64x_load_refused(const struct mach_header *header, void *map, struct vnode *vp,
                                            long long slice_offset, const void *lcmds, unsigned long lcmds_size,
                                            long long slide)
{
    (void)header, (void)map, (void)vp, (void)slice_offset, (void)lcmds, (void)lcmds_size, (void)slide;
    return false;
}
#endif /* CONFIG_A64X */
#endif /* XNU_KERNEL_PRIVATE */
#endif /* SWITCH_A64X_H */
