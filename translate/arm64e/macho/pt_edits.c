#include "pt_edits.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static bool freed_word(const struct a64x_macho *m, const uint8_t *freed, uint64_t off)
{
    if (!freed || off < m->exec_fileoff || off >= m->exec_fileoff + m->exec_filesize)
        return false;
    uint64_t k = (off - m->exec_fileoff) / 4;
    return (freed[k >> 3] & (1u << (k & 7))) != 0;
}

static int check_common(const struct a64x_macho *m, const uint8_t *data, const uint8_t *freed, uint64_t tlo,
                        uint64_t thi, const struct a64x_pt_edit *edits, size_t nedits, char *err, size_t errlen)
{
    for (size_t i = 0; i < nedits; i++) {
        const struct a64x_pt_edit *e = &edits[i];
        if (!e->stock || !e->stock_len || e->fileoff > m->size || e->stock_len > m->size - e->fileoff)
            return fail(err, errlen, "edit %zu at 0x%llx: out of bounds", i, (unsigned long long)e->fileoff);
        if (e->at > e->stock_len || e->patched_len > e->stock_len - e->at)
            return fail(err, errlen, "edit %zu at 0x%llx: patch range exceeds stock length", i,
                        (unsigned long long)e->fileoff);
        if (e->patched_len && !e->patched)
            return fail(err, errlen, "edit %zu at 0x%llx: missing patched bytes", i, (unsigned long long)e->fileoff);
        if (memcmp(m->base + e->fileoff, e->stock, e->stock_len) != 0)
            return fail(err, errlen, "edit %zu at 0x%llx: stock bytes differ from input", i,
                        (unsigned long long)e->fileoff);
        if (!e->patched_len)
            continue;
        uint64_t dst = e->fileoff + e->at;
        if (dst < m->exec_fileoff || dst + e->patched_len > m->exec_fileoff + m->exec_filesize || (dst & 3) ||
            (e->patched_len & 3))
            return fail(err, errlen, "edit %zu at 0x%llx: patch outside aligned executable code", i,
                        (unsigned long long)dst);
        for (size_t off = 0; off < e->patched_len; off += 4) {
            uint64_t at = dst + off;
            if (at == 28 || (at >= tlo && at < thi) || !a64x_image_is_code(m, data, at) || freed_word(m, freed, at))
                return fail(err, errlen, "edit %zu at 0x%llx: patch outside executable code", i,
                            (unsigned long long)at);
            uint32_t w = rd32(e->patched + off);
            enum a64x_class c = a64x_classify(w);
            if (c != A64X_OK || a64x_unallocated(w) || !(w >> 16))
                return fail(err, errlen, "edit %zu at 0x%llx: patched word %08x is %s", i, (unsigned long long)at, w,
                            c != A64X_OK ? a64x_class_name(c) : "unallocated");
        }
    }
    for (size_t i = 0; i < nedits; i++) {
        uint64_t s_lo = edits[i].fileoff, s_hi = s_lo + edits[i].stock_len;
        for (size_t j = 0; j < nedits; j++) {
            if (i == j || !edits[j].patched_len)
                continue;
            uint64_t p_lo = edits[j].fileoff + edits[j].at, p_hi = p_lo + edits[j].patched_len;
            if (p_lo < s_hi && s_lo < p_hi)
                return fail(err, errlen, "edit %zu overlaps edit %zu", j, i);
        }
    }
    return 0;
}

int a64x_pt_apply_edits(const struct a64x_macho *m, const uint8_t *data, uint64_t tlo, uint64_t thi, uint8_t *out,
                        const struct a64x_pt_edit *edits, size_t nedits, char *err, size_t errlen)
{
    if (check_common(m, data, NULL, tlo, thi, edits, nedits, err, errlen))
        return -1;
    for (size_t i = 0; i < nedits; i++) {
        const struct a64x_pt_edit *e = &edits[i];
        if (memcmp(out + e->fileoff, e->stock, e->stock_len) != 0)
            return fail(err, errlen, "edit %zu at 0x%llx: translation modified edited range", i,
                        (unsigned long long)e->fileoff);
    }
    for (size_t i = 0; i < nedits; i++) {
        const struct a64x_pt_edit *e = &edits[i];
        if (e->patched_len)
            memcpy(out + e->fileoff + e->at, e->patched, e->patched_len);
    }
    return 0;
}

int a64x_pt_check_edits(const struct a64x_macho *m, const uint8_t *data, const uint8_t *freed, uint64_t tlo,
                        uint64_t thi, const uint8_t *out, const struct a64x_pt_edit *edits, size_t nedits, char *err,
                        size_t errlen)
{
    if (check_common(m, data, freed, tlo, thi, edits, nedits, err, errlen))
        return -1;
    for (size_t i = 0; i < nedits; i++) {
        const struct a64x_pt_edit *e = &edits[i];
        if (e->at && memcmp(out + e->fileoff, e->stock, e->at) != 0)
            return fail(err, errlen, "edit %zu at 0x%llx: prefix changed", i, (unsigned long long)e->fileoff);
        if (e->patched_len && memcmp(out + e->fileoff + e->at, e->patched, e->patched_len) != 0)
            return fail(err, errlen, "edit %zu at 0x%llx: patched bytes missing", i,
                        (unsigned long long)(e->fileoff + e->at));
        size_t tail = e->at + e->patched_len;
        if (tail < e->stock_len && memcmp(out + e->fileoff + tail, e->stock + tail, e->stock_len - tail) != 0)
            return fail(err, errlen, "edit %zu at 0x%llx: suffix changed", i, (unsigned long long)(e->fileoff + tail));
    }
    return 0;
}

bool a64x_pt_edited_word(const struct a64x_pt_edit *edits, size_t nedits, uint64_t off, uint32_t *expected)
{
    for (size_t i = 0; i < nedits; i++) {
        const struct a64x_pt_edit *e = &edits[i];
        uint64_t lo = e->fileoff + e->at, hi = lo + e->patched_len;
        if (e->patched_len && off >= lo && off + 4 <= hi) {
            *expected = rd32(e->patched + (off - lo));
            return true;
        }
    }
    return false;
}
