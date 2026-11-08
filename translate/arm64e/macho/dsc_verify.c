#include "dsc.h"

#include "../a64x/a64x.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE  4096u
#define WORDS (PAGE / 4)
#define RET   0xD65F03C0u

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

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* every executable mapping of the cache, sorted by address, with a bit per word: freed by a dead check */
struct xmap {
    uint64_t addr, size;
    const uint8_t *in, *out;
    uint8_t *freed;
    bool all_code; /* a TEXT_STUBS mapping */
};

#define MAX_FIPS 16

struct cache {
    struct xmap *maps;
    int n;
    const struct a64x_plan *plan;
    uint64_t fips[MAX_FIPS]; /* the FIPS integrity MACs' addresses, each the one a64x_fips_rewrite leaves */
    int nfips, bad_fips;
    const uint8_t *orig, *out; /* the file a64x_dsc_fips_each is walking, before and after */
};

static void check_fips_mac(void *ctx, const struct a64x_fips *fips)
{
    struct cache *c = ctx;
    if (!a64x_fips_kept(c->orig + fips->text_fileoff, c->out + fips->text_fileoff, fips->text_size,
                        c->orig + fips->mac_fileoff, c->out + fips->mac_fileoff) ||
        c->nfips == MAX_FIPS)
        c->bad_fips++;
    else
        c->fips[c->nfips++] = fips->mac_vmaddr;
}

static bool fips_mac_at(const struct cache *c, uint64_t va)
{
    for (int i = 0; i < c->nfips; i++)
        if (va - c->fips[i] < A64X_FIPS_MAC_SIZE)
            return true;
    return false;
}

static int cmp_xmap(const void *a, const void *b)
{
    const struct xmap *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static struct xmap *map_of(const struct cache *c, uint64_t va)
{
    int lo = 0, hi = c->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->maps[mid].addr + c->maps[mid].size <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < c->n && va >= c->maps[lo].addr ? &c->maps[lo] : NULL;
}

static bool freed_at(const struct cache *c, uint64_t va, uint32_t *w)
{
    struct xmap *m = map_of(c, va);
    if (!m)
        return false;
    uint64_t k = (va - m->addr) / 4;
    *w = rd32(m->out + 4 * k);
    return m->freed[k >> 3] & (1u << (k & 7));
}

static bool code_of_plan(void *ctx, uint64_t va)
{
    return a64x_plan_is_code(ctx, va);
}

struct hole_ctx {
    uint8_t *freed;
    uint64_t first; /* word index of the page in its mapping */
};

static void mark_hole(void *ctx, uint32_t index, uint32_t nwords)
{
    struct hole_ctx *h = ctx;
    for (uint64_t k = h->first + index; k < h->first + index + nwords; k++)
        h->freed[k >> 3] |= (uint8_t)(1u << (k & 7));
}

/* PC-relative branch: 1 = B, 3 = BL, 2 = conditional (falls through too), 0 = none */
static int branch(uint32_t w, uint64_t pc, uint64_t *t)
{
    if ((w & 0x7C000000u) == 0x14000000u) {
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 6) >> 6) * 4);
        return (w & 0x80000000u) ? 3 : 1;
    }
    if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) {
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 8) >> 13) * 4);
        return (w & 0xFF00000Fu) == 0x5400000Eu ? 1 : 2;
    }
    if ((w & 0x7E000000u) == 0x36000000u) {
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 13) >> 18) * 4);
        return 2;
    }
    return 0;
}

/* every path from the site's branch stays in freed words until it reaches site + 4 (B form) or RET (BL form) */
static bool follow(const struct cache *c, uint64_t site, uint32_t w)
{
    uint64_t start, work[64], seen[1024];
    int kind = branch(w, site, &start), nw = 0, ns = 0;
    if (kind != 1 && kind != 3)
        return false;
    bool bl = kind == 3;
    work[nw++] = start;
    while (nw) {
        uint64_t pc = work[--nw];
        for (;;) {
            bool dup = false;
            for (int i = 0; i < ns && !dup; i++)
                dup = seen[i] == pc;
            if (dup)
                break;
            if (ns == 1024)
                return false;
            seen[ns++] = pc;
            uint32_t x;
            if (!freed_at(c, pc, &x))
                return false;
            if (a64x_class_is_baseline(a64x_classify(x)))
                return false;
            if (bl && x == RET)
                break;
            if ((x & 0xFE000000u) == 0xD6000000u)
                return false;
            uint64_t t;
            int k = branch(x, pc, &t);
            if (k == 3)
                return false;
            if (k == 0) {
                pc += 4;
                continue;
            }
            if (!bl && t == site + 4) {
                if (k == 1)
                    break;
                pc += 4;
                continue;
            }
            if (k == 2) {
                if (nw == 64)
                    return false;
                work[nw++] = pc + 4;
            }
            pc = t;
        }
    }
    return true;
}

static bool is_stub_class(enum a64x_class c, uint32_t w)
{
    uint32_t one;
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_JSCVT ||
           c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM || c == A64X_SB ||
           (c == A64X_PAC_LDRA && !a64x_rewrite1(w, &one));
}

static int check_code(const struct cache *c, const struct xmap *m, struct a64x_dsc_verify *v, char *err, size_t errlen)
{
    for (uint64_t k = 0; k < m->size / 4; k++) {
        uint64_t va = m->addr + 4 * k;
        uint32_t was = rd32(m->in + 4 * k), now = rd32(m->out + 4 * k), one;
        bool freed = m->freed[k >> 3] & (1u << (k & 7));
        if (!m->all_code && !a64x_plan_is_code(c->plan, va)) {
            if (was != now && !fips_mac_at(c, va))
                return fail(err, errlen, "data word at 0x%llx changed", (unsigned long long)va);
            continue;
        }
        v->code_words++;
        enum a64x_class cn = a64x_classify(now), cw = a64x_classify(was);
        if (a64x_class_is_baseline(cn)) {
            v->left_baseline++;
            v->left[cn]++;
        }
        if (was == now)
            continue;
        bool next_freed = k + 1 < m->size / 4 && (m->freed[(k + 1) >> 3] & (1u << ((k + 1) & 7)));
        if ((now == 0x14000004u || now == 0x14000006u) && next_freed && !freed) {
            v->dead_checks++;
        } else if (a64x_rewrite1(was, &one) && one == now) {
            v->rewritten++;
        } else if (freed) {
            v->stub_words++;
        } else if (is_stub_class(cw, was)) {
            if (follow(c, va, now)) {
                v->sites_ok++;
                v->sites[cw]++;
            } else {
                v->sites_bad++;
            }
        } else {
            return fail(err, errlen, "code word at 0x%llx: %08x became %08x for no reason", (unsigned long long)va, was,
                        now);
        }
    }
    return 0;
}

/* non-executable bytes: equal, but for 20-byte prebuilt-loader cdhashes with checkCdHash set */
static int check_data(const struct a64x_dsc_file *o, const struct a64x_dsc_file *n, char *err, size_t errlen)
{
    uint64_t lo_sig = o->sig_off, hi_sig = o->sig_off + o->sig_size;
    for (uint64_t at = 0; at < o->size; at++) {
        if (!(at % PAGE) && o->size - at >= PAGE && !memcmp(o->base + at, n->base + at, PAGE)) {
            at += PAGE - 1;
            continue;
        }
        if (o->base[at] == n->base[at] || (at >= lo_sig && at < hi_sig))
            continue;
        bool exec = false;
        for (uint32_t m = 0; m < o->nmaps && !exec; m++)
            exec = (o->maps[m].prot & 4) && at >= o->maps[m].fileoff && at < o->maps[m].fileoff + o->maps[m].size;
        if (exec)
            continue;
        uint64_t rec = at & ~7ull;
        while (rec + A64X_CDHASH_SIZE <= at)
            rec += 8;
        for (; rec + A64X_CDHASH_SIZE > at; rec -= 8) {
            if (rec + A64X_CDHASH_SIZE + 1 < n->size && n->base[rec + A64X_CDHASH_SIZE + 1] == 1 &&
                n->base[rec + A64X_CDHASH_SIZE] <= 1)
                break;
            if (rec < 8)
                return fail(err, errlen, "data byte at 0x%llx changed", (unsigned long long)at);
        }
        if (rec + A64X_CDHASH_SIZE <= at)
            return fail(err, errlen, "data byte at 0x%llx changed", (unsigned long long)at);
        at = rec + A64X_CDHASH_SIZE - 1;
    }
    return 0;
}

int a64x_dsc_verify(const struct a64x_plan *plan, const struct a64x_dsc_file *orig, const struct a64x_dsc_file *out,
                    int nfiles, struct a64x_dsc_verify *v, char *err, size_t errlen)
{
    memset(v, 0, sizeof(*v));
    struct cache c = {
        calloc((size_t)nfiles * A64X_DSC_MAX_MAPPINGS, sizeof(struct xmap)), 0, plan, {0}, 0, 0, NULL, NULL};
    int rc = c.maps ? 0 : fail(err, errlen, "out of memory");
    for (int f = 0; f < nfiles && !rc; f++) {
        if (orig[f].size != out[f].size || orig[f].nmaps != out[f].nmaps)
            rc = fail(err, errlen, "file %d changed its layout", f);
        for (uint32_t m = 0; m < orig[f].nmaps && !rc; m++) {
            const struct a64x_dsc_mapping *d = &orig[f].maps[m];
            if (!(d->prot & 4))
                continue;
            struct xmap *x = &c.maps[c.n++];
            *x = (struct xmap){d->addr,
                               d->size,
                               orig[f].base + d->fileoff,
                               out[f].base + d->fileoff,
                               calloc(d->size / 32 + 1, 1),
                               (d->flags & A64X_DSC_MAPPING_TEXT_STUBS) != 0};
            if (!x->freed)
                rc = fail(err, errlen, "out of memory");
        }
    }
    qsort(c.maps, (size_t)c.n, sizeof(*c.maps), cmp_xmap);
    for (int i = 0; i < c.n && !rc; i++) {
        struct a64x_code_map code = {(void *)plan, code_of_plan, 0};
        for (uint64_t k = 0; k < c.maps[i].size; k += PAGE) {
            uint32_t in[WORDS], scratch[WORDS];
            memcpy(in, c.maps[i].in + k, PAGE);
            struct hole_ctx h = {c.maps[i].freed, k / 4};
            a64x_dead_checks(in, scratch, WORDS, c.maps[i].addr + k, &code, mark_hole, &h);
        }
    }
    for (int f = 0; f < nfiles && !rc; f++) {
        c.orig = orig[f].base;
        c.out = out[f].base;
        (void)a64x_dsc_fips_each(&orig[f], orig[f].base, check_fips_mac, &c);
    }
    v->fips_macs = (uint64_t)c.nfips;
    if (!rc && c.bad_fips)
        rc = fail(err, errlen, "%d FIPS integrity MACs not as the translation leaves them", c.bad_fips);
    for (int i = 0; i < c.n && !rc; i++)
        rc = check_code(&c, &c.maps[i], v, err, errlen);
    for (int f = 0; f < nfiles && !rc; f++) {
        uint8_t h[A64X_CDHASH_SIZE];
        rc = check_data(&orig[f], &out[f], err, errlen);
        if (!rc)
            rc = a64x_cs_check(out[f].base, out[f].size, out[f].sig_off, out[f].sig_size, h, err, errlen);
    }
    for (int i = 0; i < c.n; i++)
        free(c.maps[i].freed);
    free(c.maps);
    if (!rc && (v->sites_bad || v->left_baseline))
        rc = fail(err, errlen, "%llu broken sites, %llu A12-baseline words left", (unsigned long long)v->sites_bad,
                  (unsigned long long)v->left_baseline);
    return rc;
}
