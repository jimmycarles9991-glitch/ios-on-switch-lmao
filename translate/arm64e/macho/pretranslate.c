#include "pretranslate.h"

#include "stub_split.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE  A64X_XLATE_PAGE
#define WORDS (PAGE / 4)
#define RET   0xD65F03C0u
/* x0-x17, x19-x28, x30: never x18 (platform) or x29 (frame); a site's own operands are excluded by the analysis */
#define LIVE_CANDIDATES ((1u << 16) | (1u << 17) | (1u << 30) | (0x7Fu << 9) | 0x1FFu | (0x3FFu << 19))
#define LIVE_BUDGET     2000
#define FN_MAX_WORDS    65536u

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

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
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

void a64x_pt_tail(const struct a64x_macho *m, uint64_t *lo, uint64_t *hi)
{
    const uint8_t *p = m->base + 32;
    *lo = *hi = 0;
    for (uint32_t i = 0; i < m->ncmds; i++, p += rd32(p + 4)) {
        if (rd32(p) != A64X_LC_SEGMENT_64 || !(rd32(p + 60) & A64X_VM_PROT_EXECUTE))
            continue;
        uint64_t fileoff = rd64(p + 40), filesize = rd64(p + 48), vmsize = rd64(p + 32);
        uint64_t end = fileoff + (filesize < vmsize ? filesize : vmsize),
                 last = fileoff ? fileoff : 32 + rd32(m->base + 20);
        for (uint32_t s = 0; s < rd32(p + 64); s++) {
            const uint8_t *sc = p + 72 + 80 * s;
            uint32_t off = rd32(sc + 48), type = rd32(sc + 64) & 0xFF;
            if (off && type != 0x1 && type != 0xC && type != 0x12 && off + rd64(sc + 40) > last)
                last = off + rd64(sc + 40);
        }
        for (uint64_t b = end; b > last; b--) {
            if (b <= m->size && m->base[b - 1]) {
                last = b;
                break;
            }
        }
        last = (last + 3) & ~3ull;
        if (last < end) {
            *lo = last;
            *hi = end;
        }
        return;
    }
}

static bool ends_flow(uint32_t w)
{
    uint32_t one;
    if (a64x_rewrite1(w, &one))
        w = one;
    return (w & 0x7C000000u) == 0x14000000u || (w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u ||
           w == RET || (w & 0xFFE0001Fu) == 0xD4200000u;
}

static void mark_data(struct a64x_image_xlate *x, uint64_t from, uint64_t to)
{
    for (uint64_t k = from; k < to; k++) {
        if (x->data[k >> 3] & (1u << (k & 7)))
            continue;
        x->data[k >> 3] |= (uint8_t)(1u << (k & 7));
        x->island_words++;
    }
}

int a64x_pt_code_map(const struct a64x_macho *m, uint32_t map_flags, struct a64x_image_xlate *x)
{
    if (a64x_image_islands(m, map_flags, x))
        return -1;
    for (uint32_t r = 0; r < m->nranges; r++) {
        uint64_t lo = m->ranges[r].lo, hi = m->ranges[r].hi;
        if (lo < m->exec_fileoff || hi > m->exec_fileoff + m->exec_filesize)
            continue;
        uint64_t k0 = (lo - m->exec_fileoff) / 4, k1 = (hi - m->exec_fileoff) / 4, piece = k0;
        for (uint64_t k = k0; k < k1; k++) {
            uint32_t w = rd32(m->base + m->exec_fileoff + 4 * k);
            bool data = x->data[k >> 3] & (1u << (k & 7));
            if (data || !(w >> 16)) {
                mark_data(x, piece, k);
                piece = k + 1;
            } else if (ends_flow(w)) {
                piece = k + 1;
            }
        }
        mark_data(x, piece, k1);
    }
    return 0;
}

struct hole {
    uint64_t off;
};

struct shared {
    uint32_t word;
    uint8_t variant;
    uint64_t off;
};

struct pt {
    const struct a64x_macho *m;
    uint8_t *out;
    const uint8_t *data, *entries;
    uint64_t *fstarts;
    size_t nfs;
    struct hole *h3, *h5;
    size_t n3, n5, cap3, cap5;
    uint64_t tail, tail_end;
    struct shared *sh;
    size_t nsh, capsh;
    struct a64x_pt_result *r;
    void *walk_buf;
    struct a64x_walk walk; /* FN_MAX_WORDS words: a whole function, or the window after a site */
    bool oom;
};

struct hole_ctx {
    struct pt *p;
    uint64_t page;
};

static void add_hole(void *ctx, uint32_t index, uint32_t nwords)
{
    struct hole_ctx *hc = ctx;
    struct pt *p = hc->p;
    struct hole **h = nwords >= 5 ? &p->h5 : &p->h3;
    size_t *n = nwords >= 5 ? &p->n5 : &p->n3, *cap = nwords >= 5 ? &p->cap5 : &p->cap3;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        struct hole *nh = realloc(*h, nc * sizeof(**h));
        if (!nh) {
            p->oom = true;
            return;
        }
        *h = nh;
        *cap = nc;
    }
    (*h)[(*n)++] = (struct hole){hc->page + 4ull * index};
    p->r->hole_words += nwords;
}

static bool is_code(void *ctx, uint64_t off)
{
    struct pt *p = ctx;
    return a64x_image_is_code(p->m, p->data, off);
}

static bool is_entered(void *ctx, uint64_t off)
{
    struct pt *p = ctx;
    return a64x_pt_entered(p->m, p->entries, off);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void load_function_starts(struct pt *p)
{
    const struct a64x_macho *m = p->m;
    if (!m->fs_size)
        return;
    p->fstarts = malloc((size_t)m->fs_size * sizeof(uint64_t));
    if (!p->fstarts)
        return;
    uint64_t addr = m->text_vmaddr;
    for (uint32_t q = 0; q < m->fs_size;) {
        uint64_t v = 0;
        for (int sh = 0; q < m->fs_size && sh < 64; sh += 7) {
            uint8_t b = m->base[m->fs_off + q++];
            v |= (uint64_t)(b & 0x7F) << sh;
            if (!(b & 0x80))
                break;
        }
        if (!v)
            break;
        addr += v;
        if (addr >= m->exec_vmaddr && addr < m->exec_vmaddr + m->exec_filesize)
            p->fstarts[p->nfs++] = addr - m->exec_vmaddr + m->exec_fileoff;
    }
    qsort(p->fstarts, p->nfs, sizeof(uint64_t), cmp_u64);
}

/* registers dead at the site (x30 included), over its whole function when its bounds are known */
static void liveness(const struct pt *p, uint64_t site, bool vtemps, uint32_t *dead, uint32_t *vdead)
{
    const struct a64x_macho *m = p->m;
    uint64_t exec_end = m->exec_fileoff + m->exec_filesize;
    size_t lo = 0, hi = p->nfs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (p->fstarts[mid] <= site)
            lo = mid + 1;
        else
            hi = mid;
    }
    *vdead = 0;
    if (lo > 0) {
        uint64_t start = p->fstarts[lo - 1], end = lo < p->nfs ? p->fstarts[lo] : exec_end;
        if ((end - start) / 4 <= FN_MAX_WORDS) {
            const uint32_t *fn = (const uint32_t *)(const void *)(m->base + start);
            int n = (int)((end - start) / 4), at = (int)((site - start) / 4);
            *dead = a64x_dead_regs_flow_fn(fn, n, at, LIVE_CANDIDATES, LIVE_BUDGET, &p->walk);
            if (vtemps)
                *vdead = a64x_dead_vregs_flow(fn, n, at, 0xFFFFFFFFu, LIVE_BUDGET, true, &p->walk);
            return;
        }
    }
    uint64_t avail = (exec_end - site) / 4;
    int n = (int)(avail < 16384 ? avail : 16384);
    const uint32_t *w = (const uint32_t *)(const void *)(m->base + site);
    *dead = a64x_dead_regs_flow(w, n, 0, LIVE_CANDIDATES, LIVE_BUDGET, &p->walk);
    if (vtemps)
        *vdead = a64x_dead_vregs_flow(w, n, 0, 0xFFFFFFFFu, LIVE_BUDGET, false, &p->walk);
}

/* the V registers a stub of w uses as temporaries */
static uint32_t vtemps_of(uint32_t w)
{
    uint32_t st[A64X_STUB_MAX], m = 0, r, wr;
    int n = a64x_stub_v(w, 0x100000, 0x200000, (1u << 16) | (1u << 17), 0xFFFF0000u, st, A64X_STUB_MAX);
    for (int k = 0; k < n && k < A64X_STUB_MAX; k++) {
        a64x_vregs_public(st[k], &r, &wr);
        m |= r | wr;
    }
    uint32_t ops = (1u << (w & 31)) | (1u << ((w >> 5) & 31)) | (1u << ((w >> 16) & 31)) | (1u << ((w >> 16) & 15)) |
                   (1u << ((w >> 10) & 31));
    return m & 0xFFFF0000u & ~ops;
}

static uint64_t take(struct pt *p, uint32_t n)
{
    if (p->tail + 4ull * n <= p->tail_end) {
        uint64_t at = p->tail;
        p->tail += 4ull * n;
        return at;
    }
    if (n <= 3 && p->n3)
        return p->h3[--p->n3].off;
    if (n <= 5 && p->n5)
        return p->h5[--p->n5].off;
    return 0;
}

/*
 * Holes for an n-word stub that no single place holds, from the top of one stack: neighbours in the file, so the
 * stub's conditional branches still reach across them. Each hole but the last gives its last word to `B next`.
 * Returns how many holes, or 0.
 */
static int pieces(const struct pt *p, uint32_t n, bool big, uint64_t *addr, uint64_t *links, int *nlinks)
{
    const struct hole *h = big ? p->h5 : p->h3;
    const size_t avail = big ? p->n5 : p->n3;
    const uint32_t size = big ? 5 : 3;
    uint32_t k = 0;
    int used = 0;
    *nlinks = 0;
    while (k < n && (size_t)used < avail) {
        uint64_t off = h[avail - 1 - (size_t)used++].off;
        uint32_t room = n - k <= size ? n - k : size - 1;
        for (uint32_t j = 0; j < room; j++)
            addr[k + j] = off + 4ull * j;
        k += room;
        if (k < n)
            links[(*nlinks)++] = off + 4ull * room;
    }
    return k == n ? used : 0;
}

/* st[0..n), laid out for `laid`, split over holes; returns where it starts and the words it took, or 0 */
static uint64_t place_split(struct pt *p, const uint32_t *st, int n, uint64_t laid, uint64_t *words)
{
    uint64_t addr[A64X_STUB_MAX], links[A64X_STUB_MAX];
    struct a64x_placed placed[2 * A64X_STUB_MAX];
    for (int big = 1; big >= 0; big--) {
        int nlinks, used = pieces(p, (uint32_t)n, big, addr, links, &nlinks);
        int np = used ? a64x_stub_split(st, n, laid, addr, links, nlinks, placed) : -1;
        if (np < 0)
            continue;
        for (int i = 0; i < np; i++)
            wr32(p->out + placed[i].at, placed[i].word);
        *(big ? &p->n5 : &p->n3) -= (size_t)used;
        *words = (uint64_t)np;
        return addr[0];
    }
    return 0;
}

/* a stub for w at a place of its own; shared: ends in RET instead of `b site + 4`. Returns its offset or 0. */
static uint64_t emit(struct pt *p, uint32_t w, uint64_t site, uint32_t dead, uint32_t vdead, bool shared)
{
    uint32_t st[A64X_STUB_MAX];
    int n = a64x_stub_v(w, site, site + 8, dead, vdead, st, A64X_STUB_MAX);
    if (n <= 0 || n > A64X_STUB_MAX)
        return 0;
    uint64_t at = take(p, (uint32_t)n), words = (uint64_t)n;
    if (at && a64x_stub_v(w, site, at, dead, vdead, st, A64X_STUB_MAX) != n)
        return 0;
    if (shared)
        st[n - 1] = RET;
    if (at) {
        for (int k = 0; k < n; k++)
            wr32(p->out + at + 4ull * k, st[k]);
    } else if (!(at = place_split(p, st, n, site + 8, &words))) {
        return 0;
    }
    p->r->stub_words += words;
    if ((st[0] & 0xFFC003FFu) == 0xD10003FFu)
        p->r->spill_sites++;
    return at;
}

static uint32_t enc_bl(uint64_t from, uint64_t to)
{
    int64_t d = (int64_t)(to - from);
    if (d < -(1ll << 27) || d >= (1ll << 27))
        return 0;
    return 0x94000000u | (uint32_t)((d >> 2) & 0x3FFFFFF);
}

static bool needs_stub(enum a64x_class c, uint32_t w)
{
    uint32_t one;
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_JSCVT ||
           c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM || c == A64X_SB ||
           (c == A64X_PAC_LDRA && !a64x_rewrite1(w, &one));
}

static bool place_site(struct pt *p, uint64_t site, uint32_t w, enum a64x_class c)
{
    uint32_t dead, vdead, branch = 0;
    bool vt = a64x_class_uses_vtemps(c);
    liveness(p, site, vt, &dead, &vdead);
    if ((dead >> 30) & 1 && c != A64X_PAC_LDRA) {
        uint32_t need = vt ? vtemps_of(w) : 0;
        bool two = (dead & ((1u << 16) | (1u << 17))) == ((1u << 16) | (1u << 17)),
             vfree = need && (vdead & need) == need;
        uint8_t variant = (uint8_t)((two ? 1 : 0) | (vfree ? 2 : 0));
        uint64_t at = 0;
        for (size_t i = 0; i < p->nsh && !at; i++)
            if (p->sh[i].word == w && p->sh[i].variant == variant)
                at = p->sh[i].off;
        if (!at) {
            at = emit(p, w, site, two ? (1u << 16) | (1u << 17) : 0, vfree ? 0xFFFF0000u : 0, true);
            if (at && p->nsh == p->capsh) {
                size_t nc = p->capsh ? p->capsh * 2 : 64;
                struct shared *ns = realloc(p->sh, nc * sizeof(*ns));
                if (!ns)
                    return false;
                p->sh = ns;
                p->capsh = nc;
            }
            if (at) {
                p->sh[p->nsh++] = (struct shared){w, variant, at};
                p->r->shared_copies++;
            }
        }
        if (at && (branch = enc_bl(site, at)))
            p->r->shared_sites++;
    }
    if (!branch) {
        uint64_t at = emit(p, w, site, dead & ~(1u << 30), vdead, false);
        branch = at ? a64x_branch(site, at) : 0;
    }
    if (!branch)
        return false;
    wr32(p->out + site, branch);
    p->r->stats.stubs[c]++;
    p->r->stats.left[c]--;
    p->r->sites++;
    return true;
}

static void free_pt(struct pt *p)
{
    free(p->fstarts);
    free(p->h3);
    free(p->h5);
    free(p->sh);
    free(p->walk_buf);
    free((void *)p->data);
    free((void *)p->entries);
}

/* pass 1: one-word rewrites and dead checks, page by page as the loader would; pass 2: every stub site */
static int translate(struct pt *p, uint32_t map_flags, char *why, size_t whylen)
{
    const struct a64x_macho *m = p->m;
    struct a64x_code_map cm = {p, is_code, map_flags, is_entered};
    for (uint64_t off = m->exec_fileoff; off < m->exec_fileoff + m->exec_filesize; off += PAGE) {
        uint32_t in[WORDS] = {0}, now[WORDS];
        uint64_t keep =
            m->exec_fileoff + m->exec_filesize - off < PAGE ? m->exec_fileoff + m->exec_filesize - off : PAGE;
        memcpy(in, m->base + off, keep);
        (void)a64x_translate_page(in, now, WORDS, off, &cm, NULL, 0, &p->r->stats);
        struct hole_ctx hc = {p, off};
        if (!(map_flags & A64X_MAP_NO_PAC))
            p->r->dead_checks += a64x_dead_checks(in, now, WORDS, off, &cm, add_hole, &hc);
        memcpy(p->out + off, now, keep);
    }
    if (p->oom)
        return fail(why, whylen, "out of memory");
    for (uint64_t off = m->exec_fileoff; off + 4 <= m->exec_fileoff + m->exec_filesize; off += 4) {
        uint32_t w = rd32(m->base + off);
        enum a64x_class c = a64x_classify(w);
        if (c == A64X_OK || !needs_stub(c, w) || !is_code(p, off))
            continue;
        if ((map_flags & A64X_MAP_NO_PAC) && a64x_class_is_pac(c))
            continue;
        if (!place_site(p, off, w, c))
            return fail(why, whylen, "no room for the stub of %08x at 0x%llx (%llu tail words, holes used)", w,
                        (unsigned long long)off, (unsigned long long)p->r->tail_words);
    }
    return 0;
}

static int fallback(struct a64x_pt_result *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int fallback(struct a64x_pt_result *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why, sizeof(r->why), fmt, ap);
    va_end(ap);
    r->status = A64X_PT_FALLBACK;
    return 0;
}

int a64x_pretranslate(const struct a64x_macho *m, uint8_t *out, struct a64x_pt_result *r, char *err, size_t errlen)
{
    memset(r, 0, sizeof(*r));
    memcpy(out, m->base, m->size);
    uint64_t sig_off, sig_size;
    bool signed_ok = !a64x_cs_find(m->base, m->size, &sig_off, &sig_size) &&
                     !a64x_cs_check(m->base, m->size, sig_off, sig_size, r->cdhash_in, NULL, 0);
    enum a64x_kind kind = a64x_kind_of(m->cputype, m->cpusubtype, m->marked);
    if (kind == A64X_KIND_NATIVE || kind == A64X_KIND_MARKED) {
        r->status = A64X_PT_UNCHANGED;
        return 0;
    }
    if (!signed_ok)
        return fallback(r, "no valid ad hoc SHA-256 signature");
    if (m->nexec != 1)
        return fallback(r, "%u executable segments", m->nexec);
    uint32_t map_flags = kind == A64X_KIND_ARM64 ? A64X_MAP_NO_PAC : 0;
    struct a64x_image_xlate islands;
    if (a64x_pt_code_map(m, map_flags, &islands))
        return fail(err, errlen, "out of memory");
    struct pt p = {.m = m, .out = out, .data = islands.data, .r = r};
    r->island_words = islands.island_words;
    a64x_pt_tail(m, &p.tail, &p.tail_end);
    r->tail_words = (p.tail_end - p.tail) / 4;
    load_function_starts(&p);
    p.walk_buf = malloc(A64X_WALK_BYTES(FN_MAX_WORDS));
    p.walk = a64x_walk_in(p.walk_buf, FN_MAX_WORDS);
    p.entries = a64x_pt_entries(m, p.data);
    int rc = !p.walk_buf ? fail(r->why, sizeof(r->why), "out of memory")
             : p.entries ? translate(&p, map_flags, r->why, sizeof(r->why))
                         : fail(r->why, sizeof(r->why), "no entry map (out of memory or unreadable fixups)");
    free_pt(&p);
    if (rc) {
        memcpy(out, m->base, m->size);
        r->status = A64X_PT_FALLBACK;
        return 0;
    }
    for (uint64_t off = m->exec_fileoff; off + 4 <= m->exec_fileoff + m->exec_filesize; off += 4)
        r->changed += rd32(out + off) != rd32(m->base + off);
    struct a64x_fips fips;
    if (a64x_pt_fips(m, m->base, &fips))
        r->fips_mac = a64x_fips_rewrite(m->base + fips.text_fileoff, out + fips.text_fileoff, fips.text_size,
                                        out + fips.mac_fileoff);
    wr32(out + 28, A64X_PRETRANSLATED);
    if (a64x_cs_rehash(out, m->size, sig_off, sig_size, r->cdhash_out, r->why, sizeof(r->why))) {
        memcpy(out, m->base, m->size);
        r->status = A64X_PT_FALLBACK;
        return 0;
    }
    r->status = A64X_PT_TRANSLATED;
    r->why[0] = 0;
    return 0;
}
