/*
 * The words of a slice's executable segment that something enters other than by falling into them. Offline stub
 * placement must stay off them: a dead PAC check is a hole only while nothing branches into its freed words.
 */
#include "pretranslate.h"

#include <stdlib.h>
#include <string.h>

struct entries {
    const struct a64x_macho *m;
    const uint8_t *data;
    uint8_t *bits;
};

/* Only code words: a pointer to the end of __text (section$end) names the first word of the stub tail, never code. */
static void mark(struct entries *e, uint64_t va)
{
    const struct a64x_macho *m = e->m;
    if (va < m->exec_vmaddr || va - m->exec_vmaddr >= m->exec_filesize || (va & 3))
        return;
    uint64_t k = (va - m->exec_vmaddr) / 4;
    if (a64x_image_is_code(m, e->data, m->exec_fileoff + 4 * k))
        e->bits[k >> 3] |= (uint8_t)(1u << (k & 7));
}

static const uint8_t *at_vm(const struct a64x_macho *m, uint64_t va, uint64_t len)
{
    for (uint32_t s = 0; s < m->nsegs; s++) {
        uint64_t rel = va - m->seg_vmaddr[s];
        if (va >= m->seg_vmaddr[s] && rel + len <= m->seg_filesize[s] && m->seg_fileoff[s] + rel + len <= m->size)
            return m->base + m->seg_fileoff[s] + rel;
    }
    return NULL;
}

static void mark_function_starts(struct entries *e)
{
    const struct a64x_macho *m = e->m;
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
        mark(e, addr);
    }
}

static void mark_jump_table(struct entries *e, const uint32_t w[9], uint64_t br_va)
{
    uint64_t table, base, count;
    if (!a64x_jump_table(w, br_va, &table, &base, &count) || !count)
        return;
    const uint8_t *t = at_vm(e->m, table, 4 * count);
    for (uint64_t i = 0; t && i < count; i++) {
        int32_t d;
        memcpy(&d, t + 4 * i, 4);
        mark(e, base + (uint64_t)(int64_t)d);
    }
}

/* A rebase holds a vmaddr or a runtime offset depending on its format and kind: both are taken. */
static const char *mark_rebase(void *ctx, uint64_t loc, uint32_t format, uint64_t raw, const struct a64x_fixup *f)
{
    (void)loc;
    (void)format;
    (void)raw;
    struct entries *e = ctx;
    if (!f->bind) {
        mark(e, f->target);
        mark(e, e->m->text_vmaddr + f->target);
    }
    return NULL;
}

uint8_t *a64x_pt_entries(const struct a64x_macho *m, const uint8_t *data)
{
    const uint32_t n = (uint32_t)(m->exec_filesize / 4);
    struct entries e = {m, data, calloc(m->exec_filesize / 32 + 1, 1)};
    uint32_t *w = malloc(4ull * n + 4);
    if (!e.bits || !w) {
        free(e.bits);
        free(w);
        return NULL;
    }
    memcpy(w, m->base + m->exec_fileoff, 4ull * n);
    mark_function_starts(&e);
    for (uint32_t k = 0; k < n; k++) {
        if (!a64x_image_is_code(m, data, m->exec_fileoff + 4ull * k))
            continue;
        const uint64_t va = m->exec_vmaddr + 4ull * k;
        uint64_t to[1];
        if (w[k] == 0xD61F0200u && k >= 8)
            mark_jump_table(&e, &w[k - 8], va);
        if (a64x_code_refs(w, n, k, m->exec_vmaddr, to))
            mark(&e, to[0]);
    }
    free(w);
    if (a64x_fixups_each(m, mark_rebase, &e, NULL, 0)) {
        free(e.bits);
        return NULL;
    }
    return e.bits;
}

bool a64x_pt_entered(const struct a64x_macho *m, const uint8_t *entries, uint64_t off)
{
    if (off < m->exec_fileoff || off - m->exec_fileoff >= m->exec_filesize)
        return false;
    uint64_t k = (off - m->exec_fileoff) / 4;
    return entries[k >> 3] & (1u << (k & 7));
}
