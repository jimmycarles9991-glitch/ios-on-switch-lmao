/*
 * The host side of a64x: Mach-O images in, translated bytes out, decided and laid out exactly as the kernel's loader
 * does it (xnu/a64x_xnu.c: a64x_parse, a64x_find_islands, a64x_scan, a64x_text_page), so an offline translation is
 * byte for byte what the device maps. Also the chained-fixup formats with authenticated pointers, decoded and
 * rewritten as plain ones (what a prelinker or an offline rewrite of arm64e code writes for a core without PAC).
 *
 * Plain C11 with libc, no SDK headers (the few Mach-O constants are below), so it builds on any host.
 */
#ifndef A64X_MACHO_H
#define A64X_MACHO_H

#include "../a64x/a64x.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A64X_MH_MAGIC_64              0xFEEDFACFu
#define A64X_FAT_MAGIC                0xCAFEBABEu /* big-endian on disk */
#define A64X_FAT_MAGIC_64             0xCAFEBABFu
#define A64X_CPU_TYPE_ARM64           0x0100000Cu
#define A64X_CPU_SUBTYPE_MASK         0xFF000000u
#define A64X_CPU_SUBTYPE_PTRAUTH_ABI  0x80000000u /* arm64e: pointer authentication with a versioned ABI */
#define A64X_CPU_SUBTYPE_ARM64_ALL    0u
#define A64X_CPU_SUBTYPE_ARM64_V8     1u
#define A64X_CPU_SUBTYPE_ARM64E       2u
#define A64X_MH_FILESET               0xCu
#define A64X_LC_REQ_DYLD              0x80000000u
#define A64X_LC_SEGMENT_64            0x19u
#define A64X_LC_FUNCTION_STARTS       0x26u
#define A64X_LC_DATA_IN_CODE          0x29u
#define A64X_LC_DYLD_CHAINED_FIXUPS   (0x34u | A64X_LC_REQ_DYLD)
#define A64X_LC_FILESET_ENTRY         (0x35u | A64X_LC_REQ_DYLD)
#define A64X_VM_PROT_EXECUTE          4u
#define A64X_S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define A64X_S_ATTR_SOME_INSTRUCTIONS 0x00000400u
/* The marker of an image built for the A57 (-mcpu=cortex-a57, no PAC, no LSE): mapped as is, never scanned. */
#define A64X_TARGET_SEGNAME  "__TEXT"
#define A64X_TARGET_SECTNAME "__a64x_target"
/*
 * mach_header_64.reserved of a slice translated offline ("a64x"): mapped as is, like the section marker. Apple's
 * binaries have no room after their load commands for a section, and keep reserved at 0.
 */
#define A64X_PRETRANSLATED 0x78343661u

/* What the loader does with an image (the kernel's a64x_image_kind() plus the marker). */
enum a64x_kind {
    A64X_KIND_NATIVE = 0, /* not arm64, or an arm64 subtype the loader leaves alone: untouched */
    A64X_KIND_ARM64E,     /* full translation: PAC as the identity, LSE, FP16, ... */
    A64X_KIND_ARM64,      /* plain arm64 (ALL/V8): only what the A57 lacks, no PAC rewrites */
    A64X_KIND_MARKED,     /* __TEXT,__a64x_target or the reserved marker: built or translated for this core */
};

const char *a64x_kind_name(enum a64x_kind kind);
enum a64x_kind a64x_kind_of(uint32_t cputype, uint32_t cpusubtype, int marked);

/* An instruction section of the executable segment: slice-relative file offsets [lo, hi). */
struct a64x_range {
    uint64_t lo, hi;
};

#define A64X_MAX_SECTIONS 64
#define A64X_MAX_SEGMENTS 256

/* One thin Mach-O (a fat file's slice, or a whole thin file), as a64x_parse() reads its load commands. */
struct a64x_macho {
    const uint8_t *base; /* the slice: its mach_header_64 is at base[0] */
    size_t size;
    uint32_t cputype, cpusubtype, filetype, ncmds;
    int marked;                           /* has __TEXT,__a64x_target, or reserved == A64X_PRETRANSLATED */
    uint64_t text_vmaddr;                 /* __TEXT's (function starts count from it) */
    uint32_t nexec;                       /* executable segments (the loader translates images with one) */
    uint64_t exec_fileoff, exec_filesize; /* the executable segment, slice-relative */
    uint64_t exec_vmaddr, vm_end;         /* its address, and the end of the image's address space */
    uint32_t nsegs;                       /* every LC_SEGMENT_64, in load-command order (chained fixups) */
    uint64_t seg_vmaddr[A64X_MAX_SEGMENTS], seg_fileoff[A64X_MAX_SEGMENTS], seg_filesize[A64X_MAX_SEGMENTS];
    uint32_t nranges; /* instruction sections, sorted by offset */
    struct a64x_range ranges[A64X_MAX_SECTIONS];
    uint32_t fs_off, fs_size;         /* LC_FUNCTION_STARTS, slice-relative */
    uint32_t dic_off, dic_size;       /* LC_DATA_IN_CODE, slice-relative */
    uint32_t fixups_off, fixups_size; /* LC_DYLD_CHAINED_FIXUPS, slice-relative */
};

/*
 * Reads one slice. 0 on success; otherwise a message in err: not a 64-bit Mach-O, load commands or sections out of
 * bounds, too many segments or instruction sections.
 */
int a64x_macho_parse(const uint8_t *base, size_t size, struct a64x_macho *m, char *err, size_t errlen);

/* The arm64 slices of a file (fat or thin): offsets and sizes in the file. Returns how many (at most max), -1 = not
 * a Mach-O. */
struct a64x_slice {
    uint64_t offset, size;
    uint32_t cputype, cpusubtype;
};

int a64x_macho_slices(const uint8_t *file, size_t size, struct a64x_slice *out, int max);

#define A64X_XLATE_PAGE 4096u /* the translator's page: the kernel's A64X_PAGE */

struct a64x_image_xlate {
    enum a64x_kind kind;
    uint32_t map_flags; /* A64X_MAP_NO_PAC for plain arm64, A64X_MAP_KERNEL on request */
    uint32_t npages;    /* 4 KB pages of the executable segment */
    int64_t stub_delta; /* stub region address - executable segment address */
    uint8_t *data;      /* 1 bit per word of the segment, set = data (data in code, data islands) */
    uint32_t *cursor;   /* [npages + 1]: stub word index at the start of each page */
    uint32_t *stubs;    /* the stub region's words */
    uint32_t nstubs;
    uint64_t changed; /* words of the segment the translation changes */
    uint64_t island_runs, island_words;
    struct a64x_stats stats;
};

/*
 * Translates the image's executable segment as the kernel does at load: data islands from function starts and data
 * in code, then a64x_translate_page() over each 4 KB page with the stubs laid out after the image's address space
 * (rounded up to map_page, the process map's page size: 16 KB, the VM page on the Switch as on an iPad). `out`
 * receives the whole slice with the segment translated (size m->size; NULL: layout only). A marked or native image is
 * copied unchanged.
 * extra_flags: A64X_MAP_KERNEL for kernel code. 0 on success.
 */
int a64x_translate_image(const struct a64x_macho *m, uint32_t map_page, uint32_t extra_flags, uint8_t *out,
                         struct a64x_image_xlate *x, char *err, size_t errlen);
void a64x_image_xlate_free(struct a64x_image_xlate *x);
/* The data islands of the image's executable segment as the loader finds them: x->npages and x->data only. */
int a64x_image_islands(const struct a64x_macho *m, uint32_t map_flags, struct a64x_image_xlate *x);
/* The word at slice offset off is inside an instruction section and not data (data: a64x_image_islands' bitmap). */
bool a64x_image_is_code(const struct a64x_macho *m, const uint8_t *data, uint64_t off);

/*
 * Checks a translation from its output bytes: every changed word that is a branch into the stub region reaches stub
 * words whose control flow (both ways of every conditional branch) returns to site + 4 and stays in the region; no
 * branch leaves the image or the region. Returns the number of sites checked, or -1 with a message.
 */
long a64x_verify_image(const struct a64x_macho *m, const uint8_t *out, const struct a64x_image_xlate *x, char *err,
                       size_t errlen);

/* Pointer formats (mach-o/fixup-chains.h). */
#define A64X_PTR_ARM64E              1u
#define A64X_PTR_64                  2u
#define A64X_PTR_64_OFFSET           6u
#define A64X_PTR_ARM64E_KERNEL       7u
#define A64X_PTR_64_KERNEL_CACHE     8u
#define A64X_PTR_ARM64E_USERLAND     9u
#define A64X_PTR_ARM64E_FIRMWARE     10u
#define A64X_PTR_ARM64E_USERLAND24   12u
#define A64X_PTR_ARM64E_SHARED_CACHE 13u

/* One chain entry, decoded. */
struct a64x_fixup {
    int bind;         /* a bind (import) rather than a rebase */
    int auth;         /* authenticated: signed with key/diversity on a PAC core */
    uint64_t target;  /* rebase: the target as the format stores it (see a64x_fixup_strip_auth) */
    uint32_t ordinal; /* bind: the import's index */
    int64_t addend;   /* bind: the addend (plain binds only) */
    uint32_t high8;   /* rebase: the top byte (plain rebases only) */
    uint32_t key, diversity, addr_div, cache_level;
    uint32_t next; /* to the next entry, in strides; 0 = last */
};

/* Stride of a format in bytes (4 or 8), 0 if a64x does not know the format. */
uint32_t a64x_fixup_stride(uint32_t format);
/* Decodes one 64-bit entry. Returns 0, or -1 for a format a64x does not know. */
int a64x_fixup_decode(uint32_t format, uint64_t raw, struct a64x_fixup *f);
/*
 * The same entry without authentication: an auth rebase becomes the plain rebase of the same target (for
 * DYLD_CHAINED_PTR_ARM64E, whose plain rebases hold a vmaddr, preferred_base is added to the runtime offset an auth
 * rebase holds), an auth bind the plain bind of the same import with addend 0; key, diversity and address diversity
 * go. `next` is kept, so the chain is unchanged. A plain entry comes back as it was. Returns 0, -1 if the target does
 * not fit the plain layout or the format is unknown.
 */
int a64x_fixup_strip_auth(uint32_t format, uint64_t raw, uint64_t preferred_base, uint64_t *out);

struct a64x_fixup_stats {
    uint64_t rebases, binds, auth_rebases, auth_binds, stripped;
};

/*
 * Calls each() for every entry of every chain of the slice's LC_DYLD_CHAINED_FIXUPS (thin images and MH_FILESET
 * kernel collections), at its slice offset. each() returns NULL, or why the walk stops. 0, or -1 with a message.
 */
typedef const char *(*a64x_fixup_fn)(void *ctx, uint64_t loc, uint32_t format, uint64_t raw,
                                     const struct a64x_fixup *f);
int a64x_fixups_each(const struct a64x_macho *m, a64x_fixup_fn each, void *ctx, char *err, size_t errlen);

/*
 * Walks every chain of the slice's LC_DYLD_CHAINED_FIXUPS (thin images and MH_FILESET kernel collections) and,
 * when `out` is not NULL (a copy of the slice, m->size bytes), writes each authenticated entry there as its plain
 * equivalent. 0 on success (also for an image without chained fixups), -1 with a message.
 */
int a64x_fixups_strip_auth(const struct a64x_macho *m, uint8_t *out, struct a64x_fixup_stats *st, char *err,
                           size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* A64X_MACHO_H */
