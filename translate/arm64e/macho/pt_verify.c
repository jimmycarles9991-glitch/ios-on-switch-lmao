#include "pretranslate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE A64X_XLATE_PAGE
#define RET  0xD65F03C0u

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
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

struct vctx {
    const struct a64x_macho *m;
    const uint8_t *data, *out, *entries;
    uint8_t *freed; /* 1 bit per word of the executable segment */
    uint64_t lo, hi;
    uint64_t mac_lo, mac_hi; /* the FIPS integrity MAC, checked against the translated text */
};

int a64x_pt_fips(const struct a64x_macho *m, const uint8_t *bytes, struct a64x_fips *fips)
{
    return a64x_fips_find(bytes, m->size, fips) && fips->text_fileoff <= m->size &&
           fips->text_size <= m->size - fips->text_fileoff && fips->mac_fileoff <= m->size - A64X_FIPS_MAC_SIZE;
}

static bool code_at(void *ctx, uint64_t off)
{
    const struct vctx *c = ctx;
    return a64x_image_is_code(c->m, c->data, off);
}

static bool freed_at(const struct vctx *c, uint64_t off)
{
    if (off < c->lo || off >= c->hi)
        return false;
    uint64_t k = (off - c->lo) / 4;
    return c->freed[k >> 3] & (1u << (k & 7));
}

static void set_freed(struct vctx *c, uint64_t off)
{
    uint64_t k = (off - c->lo) / 4;
    c->freed[k >> 3] |= (uint8_t)(1u << (k & 7));
}

struct hole_ctx {
    struct vctx *c;
    uint64_t page;
};

static void mark_hole(void *ctx, uint32_t index, uint32_t nwords)
{
    struct hole_ctx *h = ctx;
    for (uint32_t i = 0; i < nwords; i++)
        set_freed(h->c, h->page + 4ull * (index + i));
}

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

/* every path from the site's branch stays in freed words until site + 4 (B form) or RET (BL form) */
static bool follow(const struct vctx *c, uint64_t site, uint32_t w)
{
    uint64_t start, work[64], seen[512];
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
            if (ns == 512 || !freed_at(c, pc))
                return false;
            seen[ns++] = pc;
            uint32_t x = rd32(c->out + pc);
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

static bool stub_site(enum a64x_class c, uint32_t w)
{
    uint32_t one;
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_JSCVT ||
           c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM || c == A64X_SB ||
           (c == A64X_PAC_LDRA && !a64x_rewrite1(w, &one));
}

static int check_words(struct vctx *c, bool no_pac, struct a64x_pt_check *v, char *err, size_t errlen)
{
    for (uint64_t off = c->lo; off + 4 <= c->hi; off += 4) {
        uint32_t was = rd32(c->m->base + off), now = rd32(c->out + off), one;
        bool code = code_at(c, off), freed = freed_at(c, off);
        enum a64x_class cn = a64x_classify(now), cw = a64x_classify(was);
        bool counts = !(no_pac && a64x_class_is_pac(cn));
        if (code) {
            v->code_words++;
            if (a64x_class_is_baseline(cn) && counts)
                v->left_baseline++;
        }
        if (was == now || off == 28 || (off >= c->mac_lo && off < c->mac_hi))
            continue;
        if (!code && !freed)
            return fail(err, errlen, "data word at 0x%llx changed", (unsigned long long)off);
        if (freed && a64x_class_is_baseline(cn))
            return fail(err, errlen, "stub word at 0x%llx is %s", (unsigned long long)off, a64x_class_name(cn));
        if ((now == 0x14000004u || now == 0x14000006u) && !freed && freed_at(c, off + 4)) {
            v->dead_checks++;
        } else if (code && a64x_rewrite1(was, &one) && one == now) {
            v->rewritten++;
        } else if (freed) {
            v->stub_words++;
            v->entered_stub_words += a64x_pt_entered(c->m, c->entries, off);
        } else if (stub_site(cw, was) && follow(c, off, now)) {
            v->sites++;
        } else {
            return fail(err, errlen, "word at 0x%llx: %08x became %08x", (unsigned long long)off, was, now);
        }
    }
    return 0;
}

long a64x_pt_verify(const struct a64x_macho *m, const uint8_t *out, struct a64x_pt_check *v, char *err, size_t errlen)
{
    memset(v, 0, sizeof(*v));
    uint64_t sig_off, sig_size, tlo, thi;
    if (a64x_cs_find(out, m->size, &sig_off, &sig_size))
        return fail(err, errlen, "no code signature");
    if (rd32(out + 28) != A64X_PRETRANSLATED)
        return fail(err, errlen, "not marked");
    uint64_t mac_lo = 0, mac_hi = 0;
    struct a64x_fips fips;
    if (a64x_pt_fips(m, m->base, &fips)) {
        if (!a64x_fips_kept(m->base + fips.text_fileoff, out + fips.text_fileoff, fips.text_size,
                            m->base + fips.mac_fileoff, out + fips.mac_fileoff))
            return fail(err, errlen, "FIPS integrity MAC not as the translation leaves it");
        mac_lo = fips.mac_fileoff;
        mac_hi = mac_lo + A64X_FIPS_MAC_SIZE;
    }
    for (uint64_t at = 0; at < m->size; at++) {
        bool exec = at >= m->exec_fileoff && at < m->exec_fileoff + m->exec_filesize;
        if (exec || (at >= 28 && at < 32) || (at >= sig_off && at < sig_off + sig_size) ||
            (at >= mac_lo && at < mac_hi))
            continue;
        if (out[at] != m->base[at])
            return fail(err, errlen, "byte 0x%llx outside the code changed", (unsigned long long)at);
    }
    enum a64x_kind kind = a64x_kind_of(m->cputype, m->cpusubtype, 0);
    uint32_t flags = kind == A64X_KIND_ARM64 ? A64X_MAP_NO_PAC : 0;
    struct a64x_image_xlate islands;
    if (a64x_pt_code_map(m, flags, &islands))
        return fail(err, errlen, "out of memory");
    struct vctx c = {m,
                     islands.data,
                     out,
                     a64x_pt_entries(m, islands.data),
                     calloc(m->exec_filesize / 32 + 1, 1),
                     m->exec_fileoff,
                     m->exec_fileoff + m->exec_filesize,
                     mac_lo,
                     mac_hi};
    int rc = c.freed && c.entries ? 0 : fail(err, errlen, "out of memory or unreadable fixups");
    struct a64x_code_map cm = {&c, code_at, flags, NULL};
    for (uint64_t off = c.lo; off < c.hi && !rc && !(flags & A64X_MAP_NO_PAC); off += PAGE) {
        uint32_t in[PAGE / 4] = {0}, scratch[PAGE / 4];
        memcpy(in, m->base + off, c.hi - off < PAGE ? c.hi - off : PAGE);
        struct hole_ctx h = {&c, off};
        a64x_dead_checks(in, scratch, PAGE / 4, off, &cm, mark_hole, &h);
    }
    a64x_pt_tail(m, &tlo, &thi);
    for (uint64_t off = tlo; off < thi && !rc; off += 4)
        set_freed(&c, off);
    if (!rc)
        rc = check_words(&c, flags & A64X_MAP_NO_PAC, v, err, errlen);
    if (!rc && v->entered_stub_words)
        rc = fail(err, errlen, "%llu stub words where something branches", (unsigned long long)v->entered_stub_words);
    if (!rc && v->left_baseline)
        rc = fail(err, errlen, "%llu A12-baseline words left", (unsigned long long)v->left_baseline);
    if (!rc)
        rc = a64x_cs_check(out, m->size, sig_off, sig_size, v->cdhash, err, errlen);
    free(c.freed);
    free((void *)c.entries);
    free(islands.data);
    return rc ? -1 : (long)v->sites;
}
