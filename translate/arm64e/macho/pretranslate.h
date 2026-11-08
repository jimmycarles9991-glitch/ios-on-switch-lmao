/*
 * One Mach-O slice translated offline, in place: same size, same layout, same load commands. One-word rewrites as
 * the loader does them; dead PAC failure checks become `b ok` and give their words to stubs; stubs also go into
 * the zero tail of the executable segment (between its last section and its end, already mapped executable and
 * covered by the signature); a stub longer than every hole left is split over several (stub_split.h). x30 dead at a
 * site: `BL shared` to one stub per (word, registers) ending in RET.
 * Liveness is per function (LC_FUNCTION_STARTS). The slice is marked (mach_header_64.reserved) so the kernel maps
 * it as is, and re-signed. A slice whose stubs do not all fit is left as it was: the kernel translates it at load.
 */
#ifndef A64X_PRETRANSLATE_H
#define A64X_PRETRANSLATE_H

#include "codesign.h"
#include "fips.h"
#include "macho.h"

#ifdef __cplusplus
extern "C" {
#endif

enum a64x_pt_status {
    A64X_PT_UNCHANGED,  /* nothing the A57 lacks, not arm64, or already marked */
    A64X_PT_TRANSLATED, /* translated, marked, re-signed */
    A64X_PT_FALLBACK,   /* left as it was (why says why): translated by the kernel at load */
};

struct a64x_pt_result {
    enum a64x_pt_status status;
    char why[128];
    struct a64x_stats stats; /* rewrite1 by class; stubs = sites placed; left = words not translated (A13+) */
    uint64_t sites, shared_sites, shared_copies, stub_words, spill_sites;
    uint64_t dead_checks, hole_words, tail_words, island_words, changed;
    int fips_mac; /* corecrypto's FIPS integrity MAC rewritten for the translated text (fips.h) */
    uint8_t cdhash_in[A64X_CDHASH_SIZE], cdhash_out[A64X_CDHASH_SIZE];
};

/* out: m->size bytes, the slice translated (or copied unchanged). 0, or -1 with a message (unreadable slice). */
int a64x_pretranslate(const struct a64x_macho *m, uint8_t *out, struct a64x_pt_result *r, char *err, size_t errlen);

/*
 * A translated slice against its original, from the bytes: only the executable segment's code words, its tail,
 * reserved and a FIPS integrity MAC (that of the translated text) changed; no A12-baseline instruction in code or stubs; every changed word is a one-word rewrite, a
 * `b ok`, a stub word (in a freed check or the tail) or a site whose every path stays in stub words until it
 * returns to site + 4 (B) or RETs (BL); the signature checks. Returns the sites checked, or -1 with a message.
 */
struct a64x_pt_check {
    uint64_t code_words, rewritten, dead_checks, stub_words, sites, left_baseline;
    uint8_t cdhash[A64X_CDHASH_SIZE];
};

long a64x_pt_verify(const struct a64x_macho *m, const uint8_t *out, struct a64x_pt_check *v, char *err, size_t errlen);

/*
 * The code map of a slice translated offline (x->data, x->npages): the loader's data islands, plus the trailing words
 * of every run that falls into padding (a zero or UDF word, or its section's end) after its last terminator or call:
 * no execution reaches them as code. lzfse's mask tables are such runs (64-bit masks, high halves zero).
 */
int a64x_pt_code_map(const struct a64x_macho *m, uint32_t map_flags, struct a64x_image_xlate *x);

/* The stub space of a slice: the exec segment's tail after its last section, as [lo, hi) slice offsets. */
void a64x_pt_tail(const struct a64x_macho *m, uint64_t *lo, uint64_t *hi);

/* corecrypto's FIPS integrity check of the slice at bytes (fips.h), its text and MAC inside the slice: 1 or 0. */
int a64x_pt_fips(const struct a64x_macho *m, const uint8_t *bytes, struct a64x_fips *fips);

#ifdef __cplusplus
}
#endif

#endif /* A64X_PRETRANSLATE_H */
