/*
 * The dyld shared cache, translated offline: each cache file gets exactly the words the kernel's cache pager would
 * map (xnu/bsd/kern/a64x_xnu.c a64x_cache_page: one-word rewrites in the plan's code ranges, dead PAC checks to
 * `b ok`, then the plan's words), is re-signed in place, and the cdhashes the cache's prebuilt loaders check for
 * on-disk binaries are replaced by those of the translated binaries.
 */
#ifndef A64X_DSC_H
#define A64X_DSC_H

#include "codesign.h"
#include "fips.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A plan file of `arm64e plan` (format: xnu/bsd/kern/a64x_xnu.c struct a64x_plan_hdr), checked and indexed. */
struct a64x_plan {
    uint8_t cache_uuid[16];
    uint32_t nranges, npages;
    const uint64_t *ranges; /* [lo, hi) pairs, sorted */
    const uint8_t *pages;   /* 16 bytes each: va, first, count */
    const uint8_t *words;   /* 8 bytes each: index (u16), pad, word */
    uint64_t nwords;
};

int a64x_plan_open(const uint8_t *buf, size_t size, struct a64x_plan *p, char *err, size_t errlen);
bool a64x_plan_is_code(const struct a64x_plan *p, uint64_t va);
/* The plan's words for the 4 KB page at va: out[index] = word. */
void a64x_plan_page(const struct a64x_plan *p, uint64_t va, uint32_t out[1024]);

#define A64X_DSC_MAX_MAPPINGS       16
#define A64X_DSC_MAPPING_TEXT_STUBS (1u << 3)

struct a64x_dsc_mapping {
    uint64_t addr, size, fileoff, flags; /* flags: dyld_cache_mapping_and_slide_info's */
    uint32_t prot;
};

/* One file of the cache (main or subcache), from its own header. */
struct a64x_dsc_file {
    const uint8_t *base;
    size_t size;
    uint8_t uuid[16];
    uint32_t nmaps;
    struct a64x_dsc_mapping maps[A64X_DSC_MAX_MAPPINGS];
    uint64_t sig_off, sig_size;
};

int a64x_dsc_parse(const uint8_t *buf, size_t size, struct a64x_dsc_file *f, char *err, size_t errlen);

struct a64x_dsc_stats {
    uint64_t pages, words_changed, plan_words, dead_checks, cdhashes_replaced, fips_macs;
};

/*
 * corecrypto's FIPS integrity check of every image whose header is in this file's executable mappings
 * (libcorecrypto's, macho/fips.h), its text and MAC in this file too: calls found(ctx, fips) with file offsets.
 * Returns how many.
 */
uint64_t a64x_dsc_fips_each(const struct a64x_dsc_file *f, const uint8_t *bytes,
                            void (*found)(void *ctx, const struct a64x_fips *fips), void *ctx);

/* Old and new cdhash of one translated on-disk binary slice (sorted by old for a64x_dsc_apply). */
struct a64x_cdhash_pair {
    uint8_t old[A64X_CDHASH_SIZE], now[A64X_CDHASH_SIZE];
};

/*
 * out: a copy of f (f->size bytes), translated, FIPS integrity MACs of its translated text, prebuilt-loader cdhashes
 * replaced (pairs may be NULL), re-signed; cdhash: the new one. 0, or -1 with a message.
 */
int a64x_dsc_apply(const struct a64x_plan *plan, const struct a64x_dsc_file *f, const struct a64x_cdhash_pair *pairs,
                   size_t npairs, uint8_t *out, uint8_t cdhash[A64X_CDHASH_SIZE], struct a64x_dsc_stats *st, char *err,
                   size_t errlen);

/*
 * The whole translated cache against the original, from the output bytes only: no A12-baseline instruction left in
 * code (the plan's code ranges and every TEXT_STUBS mapping, whatever the plan says); every changed code word is a one-word rewrite, a `b ok`, a word freed by a dead check, or a stub site whose
 * every path stays in freed words until it returns to site + 4 (B) or RETs (BL); data bytes unchanged except
 * replaced cdhashes and FIPS integrity MACs, each as a64x_fips_rewrite leaves it; every file's signature checks.
 */
struct a64x_dsc_verify {
    uint64_t code_words, rewritten, dead_checks, stub_words, sites_ok, sites_bad, left_baseline, fips_macs;
    uint64_t sites[64], left[64]; /* by class */
};

int a64x_dsc_verify(const struct a64x_plan *plan, const struct a64x_dsc_file *orig, const struct a64x_dsc_file *out,
                    int nfiles, struct a64x_dsc_verify *v, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* A64X_DSC_H */
