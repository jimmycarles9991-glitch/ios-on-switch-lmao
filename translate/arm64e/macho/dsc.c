#include "dsc.h"

#include "../a64x/a64x.h"
#include "macho.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE              4096u
#define MH_DYLIB_IN_CACHE 0x80000000u
#define WORDS             (PAGE / 4)

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int a64x_plan_open(const uint8_t *buf, size_t size, struct a64x_plan *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof(*p));
    if (size < 64 || memcmp(buf, "A64XPLN1", 8))
        return fail(err, errlen, "not an a64x plan");
    memcpy(p->cache_uuid, buf + 8, 16);
    p->nranges = rd32(buf + 24);
    p->npages = rd32(buf + 28);
    uint64_t ranges_off = rd64(buf + 32), pages_off = rd64(buf + 40), words_off = rd64(buf + 48),
             total = rd64(buf + 56);
    if (total != size || ranges_off + 16ull * p->nranges > pages_off || pages_off + 16ull * p->npages > words_off ||
        words_off > size || (size - words_off) % 8 || ranges_off % 8)
        return fail(err, errlen, "plan layout is inconsistent");
    p->ranges = (const uint64_t *)(const void *)(buf + ranges_off);
    p->pages = buf + pages_off;
    p->words = buf + words_off;
    p->nwords = (size - words_off) / 8;
    for (uint32_t i = 0; i < p->npages; i++) {
        const uint8_t *e = p->pages + 16ull * i;
        if (rd32(e + 8) + (uint64_t)rd32(e + 12) > p->nwords || (i && rd64(e) <= rd64(e - 16)))
            return fail(err, errlen, "plan page %u is out of order or past the words", i);
    }
    return 0;
}

bool a64x_plan_is_code(const struct a64x_plan *p, uint64_t va)
{
    uint32_t lo = 0, hi = p->nranges;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (p->ranges[2 * mid + 1] <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < p->nranges && va >= p->ranges[2 * lo];
}

void a64x_plan_page(const struct a64x_plan *p, uint64_t va, uint32_t out[WORDS])
{
    uint32_t lo = 0, hi = p->npages;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rd64(p->pages + 16ull * mid) < va)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == p->npages || rd64(p->pages + 16ull * lo) != va)
        return;
    const uint8_t *e = p->pages + 16ull * lo;
    for (uint32_t i = 0, first = rd32(e + 8); i < rd32(e + 12); i++) {
        const uint8_t *w = p->words + 8ull * (first + i);
        uint16_t index;
        memcpy(&index, w, 2);
        if (index < WORDS)
            out[index] = rd32(w + 4);
    }
}

int a64x_dsc_parse(const uint8_t *buf, size_t size, struct a64x_dsc_file *f, char *err, size_t errlen)
{
    memset(f, 0, sizeof(*f));
    if (size < 0x100 || memcmp(buf, "dyld_v1", 7))
        return fail(err, errlen, "not a dyld cache file");
    f->base = buf;
    f->size = size;
    memcpy(f->uuid, buf + 0x58, 16);
    uint32_t moff = rd32(buf + 0x10), mcnt = rd32(buf + 0x14);
    if (mcnt > A64X_DSC_MAX_MAPPINGS || moff + 32ull * mcnt > size)
        return fail(err, errlen, "%u mappings", mcnt);
    for (uint32_t i = 0; i < mcnt; i++) {
        const uint8_t *m = buf + moff + 32ull * i;
        struct a64x_dsc_mapping *d = &f->maps[f->nmaps++];
        d->addr = rd64(m);
        d->size = rd64(m + 8);
        d->fileoff = rd64(m + 16);
        d->prot = rd32(m + 28);
        if (d->fileoff > size || d->size > size - d->fileoff || (d->addr | d->size | d->fileoff) % PAGE)
            return fail(err, errlen, "mapping %u is not page aligned inside the file", i);
    }
    uint32_t soff = rd32(buf + 0x138), scnt = rd32(buf + 0x13C);
    for (uint32_t i = 0; i < scnt && soff + 56ull * (i + 1) <= size; i++) {
        const uint8_t *e = buf + soff + 56ull * i;
        for (uint32_t m = 0; m < f->nmaps; m++)
            if (f->maps[m].addr == rd64(e))
                f->maps[m].flags = rd64(e + 40);
    }
    f->sig_off = rd64(buf + 0x28);
    f->sig_size = rd64(buf + 0x30);
    return 0;
}

struct plan_map {
    const struct a64x_plan *plan;
};

static bool code_of_plan(void *ctx, uint64_t va)
{
    return a64x_plan_is_code(((struct plan_map *)ctx)->plan, va);
}

/* xnu/bsd/kern/a64x_xnu.c a64x_cache_page(), on one 4 KB page */
static void translate_page(const struct a64x_plan *plan, const uint32_t *in, uint32_t *out, uint64_t va,
                           struct a64x_dsc_stats *st)
{
    struct plan_map pm = {plan};
    struct a64x_code_map code = {&pm, code_of_plan, 0};
    uint32_t planned[WORDS];
    (void)a64x_translate_page(in, out, WORDS, va, &code, NULL, 0, NULL);
    st->dead_checks += a64x_dead_checks(in, out, WORDS, va, &code, NULL, NULL);
    memcpy(planned, out, sizeof(planned));
    a64x_plan_page(plan, va, planned);
    for (uint32_t i = 0; i < WORDS; i++) {
        st->plan_words += planned[i] != out[i];
        out[i] = planned[i];
        st->words_changed += out[i] != in[i];
    }
    st->pages++;
}

static int cmp_pair(const void *a, const void *b)
{
    return memcmp(a, b, A64X_CDHASH_SIZE);
}

/*
 * dyld's PrebuiltLoader::FileValidationInfo { u64 sliceOffset, deviceID, inode, mtime; u8 cdHash[20]; bool
 * checkInodeMtime, checkCdHash; }: cdHash is 8-byte aligned and only records with checkCdHash set are taken.
 */
static uint64_t replace_cdhashes(const struct a64x_dsc_file *f, uint8_t *out, const struct a64x_cdhash_pair *pairs,
                                 size_t npairs)
{
    uint64_t replaced = 0;
    for (uint32_t m = 0; m < f->nmaps; m++) {
        if (f->maps[m].prot & 4)
            continue;
        uint64_t lo = f->maps[m].fileoff, hi = lo + f->maps[m].size;
        for (uint64_t at = lo; at + A64X_CDHASH_SIZE + 2 <= hi; at += 8) {
            if (out[at + A64X_CDHASH_SIZE + 1] != 1 || out[at + A64X_CDHASH_SIZE] > 1)
                continue;
            const struct a64x_cdhash_pair *p = bsearch(out + at, pairs, npairs, sizeof(*pairs), cmp_pair);
            if (!p)
                continue;
            memcpy(out + at, p->now, A64X_CDHASH_SIZE);
            replaced++;
        }
    }
    return replaced;
}

static bool file_offset(const struct a64x_dsc_file *f, uint64_t va, uint64_t size, uint64_t *off)
{
    for (uint32_t m = 0; m < f->nmaps; m++) {
        const struct a64x_dsc_mapping *d = &f->maps[m];
        if (va >= d->addr && size <= d->size && va - d->addr <= d->size - size) {
            *off = d->fileoff + (va - d->addr);
            return true;
        }
    }
    return false;
}

uint64_t a64x_dsc_fips_each(const struct a64x_dsc_file *f, const uint8_t *bytes,
                            void (*found)(void *ctx, const struct a64x_fips *fips), void *ctx)
{
    uint64_t n = 0;
    for (uint32_t m = 0; m < f->nmaps; m++) {
        const struct a64x_dsc_mapping *d = &f->maps[m];
        if (!(d->prot & 4))
            continue;
        /* Every image's __TEXT, which its mach_header starts, is page aligned in the cache. */
        for (uint64_t k = 0; k + 32 <= d->size; k += PAGE) {
            const uint8_t *h = bytes + d->fileoff + k;
            struct a64x_fips fips;
            if (rd32(h) != A64X_MH_MAGIC_64 || !(rd32(h + 24) & MH_DYLIB_IN_CACHE) ||
                !a64x_fips_find(h, d->size - k, &fips) ||
                !file_offset(f, fips.text_vmaddr, fips.text_size, &fips.text_fileoff) ||
                !file_offset(f, fips.mac_vmaddr, A64X_FIPS_MAC_SIZE, &fips.mac_fileoff))
                continue;
            found(ctx, &fips);
            n++;
        }
    }
    return n;
}

struct fips_pair {
    const uint8_t *original;
    uint8_t *out;
    uint64_t rewritten;
};

static void rewrite_fips_mac(void *ctx, const struct a64x_fips *fips)
{
    struct fips_pair *p = ctx;
    p->rewritten += (uint64_t)a64x_fips_rewrite(p->original + fips->text_fileoff, p->out + fips->text_fileoff,
                                                fips->text_size, p->out + fips->mac_fileoff);
}

int a64x_dsc_apply(const struct a64x_plan *plan, const struct a64x_dsc_file *f, const struct a64x_cdhash_pair *pairs,
                   size_t npairs, uint8_t *out, uint8_t cdhash[A64X_CDHASH_SIZE], struct a64x_dsc_stats *st, char *err,
                   size_t errlen)
{
    memset(st, 0, sizeof(*st));
    memcpy(out, f->base, f->size);
    for (uint32_t m = 0; m < f->nmaps; m++) {
        if (!(f->maps[m].prot & 4))
            continue;
        for (uint64_t k = 0; k < f->maps[m].size; k += PAGE) {
            uint32_t in[WORDS], now[WORDS];
            memcpy(in, f->base + f->maps[m].fileoff + k, PAGE);
            translate_page(plan, in, now, f->maps[m].addr + k, st);
            memcpy(out + f->maps[m].fileoff + k, now, PAGE);
        }
    }
    struct fips_pair fp = {f->base, out, 0};
    (void)a64x_dsc_fips_each(f, f->base, rewrite_fips_mac, &fp);
    st->fips_macs = fp.rewritten;
    if (pairs && npairs)
        st->cdhashes_replaced = replace_cdhashes(f, out, pairs, npairs);
    if (!f->sig_off)
        return fail(err, errlen, "cache file has no code signature");
    return a64x_cs_rehash(out, f->size, f->sig_off, f->sig_size, cdhash, err, errlen);
}
