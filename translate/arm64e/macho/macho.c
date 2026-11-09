/*
 * Host side of a64x (macho.h): Mach-O parsing, the loader's translation of one image, its verification, and the
 * chained-fixup formats with authenticated pointers. The translation follows xnu/a64x_xnu.c step for step; where the
 * kernel works on absolute file offsets this works on slice-relative ones (stubs are reached PC-relative, and a
 * segment's file offset and address share their 4 KB phase, so the words are the same).
 */
#include "macho.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
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

static int name16(const uint8_t *p, const char *name)
{
    return strncmp((const char *)p, name, 16) == 0;
}

const char *a64x_kind_name(enum a64x_kind kind)
{
    switch (kind) {
    case A64X_KIND_ARM64E:
        return "arm64e";
    case A64X_KIND_ARM64:
        return "arm64";
    case A64X_KIND_MARKED:
        return "marked";
    case A64X_KIND_NATIVE:
        break;
    }
    return "native";
}

enum a64x_kind a64x_kind_of(uint32_t cputype, uint32_t cpusubtype, int marked)
{
    if (cputype != A64X_CPU_TYPE_ARM64)
        return A64X_KIND_NATIVE;
    enum a64x_kind kind;
    /* arm64e is the ptrauth capability bit (a fat binary's versioned-ABI slices do not carry subtype 2 in the low
     * byte, but every one holds PAC): a64x_xnu.h a64x_image_kind decides the same way. */
    if (cpusubtype & A64X_CPU_SUBTYPE_PTRAUTH_ABI) {
        kind = A64X_KIND_ARM64E;
    } else {
        switch (cpusubtype & ~A64X_CPU_SUBTYPE_MASK) {
        case A64X_CPU_SUBTYPE_ARM64E:
            kind = A64X_KIND_ARM64E;
            break;
        case A64X_CPU_SUBTYPE_ARM64_ALL:
        case A64X_CPU_SUBTYPE_ARM64_V8:
            kind = A64X_KIND_ARM64;
            break;
        default:
            return A64X_KIND_NATIVE;
        }
    }
    /* the marker wins over the subtype (a64x_map_image: a marked image is mapped as is) */
    return marked ? A64X_KIND_MARKED : kind;
}

int a64x_macho_parse(const uint8_t *base, size_t size, struct a64x_macho *m, char *err, size_t errlen)
{
    memset(m, 0, sizeof(*m));
    m->base = base;
    m->size = size;
    if (size < 32 || rd32(base) != A64X_MH_MAGIC_64)
        return fail(err, errlen, "not a 64-bit Mach-O");
    m->cputype = rd32(base + 4);
    m->cpusubtype = rd32(base + 8);
    m->filetype = rd32(base + 12);
    m->ncmds = rd32(base + 16);
    m->marked = rd32(base + 28) == A64X_PRETRANSLATED;
    uint32_t sizeofcmds = rd32(base + 20);
    if (sizeofcmds > size - 32)
        return fail(err, errlen, "load commands (%u bytes) past the end", sizeofcmds);
    const uint8_t *p = base + 32, *end = p + sizeofcmds;
    for (uint32_t i = 0; i < m->ncmds; i++) {
        if (end - p < 8)
            return fail(err, errlen, "load command %u past the end", i);
        uint32_t cmd = rd32(p), cmdsize = rd32(p + 4);
        if (cmdsize < 8 || cmdsize > (size_t)(end - p))
            return fail(err, errlen, "load command %u: bad size", i);
        if ((cmd == A64X_LC_FUNCTION_STARTS || cmd == A64X_LC_DATA_IN_CODE || cmd == A64X_LC_DYLD_CHAINED_FIXUPS) &&
            cmdsize >= 16) {
            uint32_t off = rd32(p + 8), len = rd32(p + 12);
            if (off > size || len > size - off)
                return fail(err, errlen, "linkedit data of command %u past the end", i);
            if (cmd == A64X_LC_FUNCTION_STARTS) {
                m->fs_off = off;
                m->fs_size = len;
            } else if (cmd == A64X_LC_DATA_IN_CODE) {
                m->dic_off = off;
                m->dic_size = len;
            } else {
                m->fixups_off = off;
                m->fixups_size = len;
            }
        }
        if (cmd == A64X_LC_SEGMENT_64) {
            if (cmdsize < 72)
                return fail(err, errlen, "segment command %u too short", i);
            uint64_t vmaddr = rd64(p + 24), vmsize = rd64(p + 32), fileoff = rd64(p + 40), filesize = rd64(p + 48);
            uint32_t initprot = rd32(p + 60), nsects = rd32(p + 64);
            if (cmdsize < 72 + (uint64_t)nsects * 80)
                return fail(err, errlen, "segment %u: sections past its command", i);
            if (m->nsegs >= A64X_MAX_SEGMENTS)
                return fail(err, errlen, "more than %d segments", A64X_MAX_SEGMENTS);
            m->seg_vmaddr[m->nsegs] = vmaddr;
            m->seg_fileoff[m->nsegs] = fileoff;
            m->seg_filesize[m->nsegs] = filesize;
            m->nsegs++;
            if (name16(p + 8, "__TEXT"))
                m->text_vmaddr = vmaddr;
            if (!name16(p + 8, "__PAGEZERO") && vmaddr + vmsize > m->vm_end)
                m->vm_end = vmaddr + vmsize;
            const uint8_t *sc = p + 72;
            for (uint32_t s = 0; s < nsects; s++, sc += 80) {
                if (name16(sc + 16, A64X_TARGET_SEGNAME) && name16(sc, A64X_TARGET_SECTNAME))
                    m->marked = 1;
            }
            if (initprot & A64X_VM_PROT_EXECUTE) {
                if (m->nexec++ == 0) {
                    if (fileoff > size || filesize > size - fileoff)
                        return fail(err, errlen, "executable segment past the end of the file");
                    m->exec_fileoff = fileoff;
                    m->exec_filesize = filesize;
                    m->exec_vmaddr = vmaddr;
                    sc = p + 72;
                    for (uint32_t s = 0; s < nsects; s++, sc += 80) {
                        uint64_t ssize = rd64(sc + 40);
                        uint32_t soff = rd32(sc + 48), flags = rd32(sc + 64);
                        if (!(flags & (A64X_S_ATTR_PURE_INSTRUCTIONS | A64X_S_ATTR_SOME_INSTRUCTIONS)) || !ssize)
                            continue;
                        if (m->nranges >= A64X_MAX_SECTIONS)
                            return fail(err, errlen, "more than %d instruction sections", A64X_MAX_SECTIONS);
                        if (soff > size || ssize > size - soff)
                            return fail(err, errlen, "instruction section %u past the end", s);
                        m->ranges[m->nranges].lo = soff;
                        m->ranges[m->nranges].hi = soff + ssize;
                        m->nranges++;
                    }
                }
            }
        }
        p += cmdsize;
    }
    /* sorted for the binary search (they are in file order already, as the kernel assumes too) */
    for (uint32_t i = 1; i < m->nranges; i++) {
        for (uint32_t j = i; j > 0 && m->ranges[j - 1].lo > m->ranges[j].lo; j--) {
            struct a64x_range t = m->ranges[j];
            m->ranges[j] = m->ranges[j - 1];
            m->ranges[j - 1] = t;
        }
    }
    return 0;
}

int a64x_macho_slices(const uint8_t *file, size_t size, struct a64x_slice *out, int max)
{
    if (size < 8)
        return -1;
    if (rd32(file) == A64X_MH_MAGIC_64) {
        if (size < 32)
            return -1;
        if (max < 1)
            return 0;
        out[0].offset = 0;
        out[0].size = size;
        out[0].cputype = rd32(file + 4);
        out[0].cpusubtype = rd32(file + 8);
        return out[0].cputype == A64X_CPU_TYPE_ARM64 ? 1 : 0;
    }
    uint32_t magic = be32(file);
    if (magic != A64X_FAT_MAGIC && magic != A64X_FAT_MAGIC_64)
        return -1;
    uint32_t n = be32(file + 4), entry = magic == A64X_FAT_MAGIC ? 20 : 32;
    if ((uint64_t)n * entry > size - 8)
        return -1;
    int k = 0;
    for (uint32_t i = 0; i < n && k < max; i++) {
        const uint8_t *a = file + 8 + (size_t)i * entry;
        uint32_t ct = be32(a), st = be32(a + 4);
        uint64_t off = magic == A64X_FAT_MAGIC ? be32(a + 8) : be64(a + 8);
        uint64_t len = magic == A64X_FAT_MAGIC ? be32(a + 12) : be64(a + 16);
        if (ct != A64X_CPU_TYPE_ARM64 || off > size || len > size - off)
            continue;
        out[k].offset = off;
        out[k].size = len;
        out[k].cputype = ct;
        out[k].cpusubtype = st;
        k++;
    }
    return k;
}

struct img_ctx {
    const struct a64x_macho *m;
    const uint8_t *data; /* NULL while the islands are being found */
};

/* xnu/a64x_xnu.c img_is_code(): inside an instruction section and not a data word */
static bool img_is_code(void *ctx, uint64_t off)
{
    const struct img_ctx *c = ctx;
    const struct a64x_macho *m = c->m;
    if (c->data && off >= m->exec_fileoff) {
        uint64_t k = (off - m->exec_fileoff) / 4;
        uint64_t nwords = (m->exec_filesize + A64X_XLATE_PAGE - 1) / A64X_XLATE_PAGE * (A64X_XLATE_PAGE / 4);
        if (k < nwords && (c->data[k >> 3] & (1u << (k & 7))))
            return false;
    }
    uint32_t lo = 0, hi = m->nranges;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->ranges[mid].hi <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < m->nranges && off >= m->ranges[lo].lo;
}

/* One 4 KB page of the slice at off, zero past the end (the kernel reads it with vn_rdwr into a zeroed page). */
static void read_page(const struct a64x_macho *m, uint64_t off, uint32_t *page)
{
    memset(page, 0, A64X_XLATE_PAGE);
    if (off < m->size) {
        size_t n = m->size - off < A64X_XLATE_PAGE ? (size_t)(m->size - off) : A64X_XLATE_PAGE;
        memcpy(page, m->base + off, n);
    }
}

/* Must find what xnu/a64x_xnu.c a64x_find_islands() finds: the translation depends on it. */
static int find_islands(const struct a64x_macho *m, struct a64x_image_xlate *x, uint32_t map_flags)
{
    size_t nbits = (size_t)x->npages * (A64X_XLATE_PAGE / 4), nbytes = nbits / 8;
    uint8_t *fs = NULL;
    uint32_t page[A64X_XLATE_PAGE / 4];
    x->data = calloc(nbytes ? nbytes : 1, 1);
    if (!x->data)
        return -1;
    if (m->fs_size && m->fs_size <= (1u << 24)) {
        fs = calloc(nbytes ? nbytes : 1, 1);
        if (!fs)
            return -1;
        const uint8_t *buf = m->base + m->fs_off;
        uint64_t addr = m->text_vmaddr;
        for (uint32_t q = 0; q < m->fs_size;) {
            uint64_t v = 0;
            int sh = 0;
            while (q < m->fs_size && sh < 64) {
                uint8_t b = buf[q++];
                v |= (uint64_t)(b & 0x7F) << sh;
                sh += 7;
                if (!(b & 0x80))
                    break;
            }
            if (!v)
                break;
            addr += v;
            if (addr >= m->exec_vmaddr) {
                uint64_t k = (addr - m->exec_vmaddr) / 4;
                if (k < nbits)
                    fs[k >> 3] |= (uint8_t)(1u << (k & 7));
            }
        }
    }
    if (m->dic_size && m->dic_size <= (1u << 20)) {
        const uint8_t *buf = m->base + m->dic_off;
        for (uint32_t q = 0; q + 8 <= m->dic_size; q += 8) {
            uint64_t lo = rd32(buf + q), len = rd16(buf + q + 4);
            for (uint64_t o = lo; o < lo + len; o += 4) {
                if (o >= m->exec_fileoff && (o - m->exec_fileoff) / 4 < nbits) {
                    uint64_t k = (o - m->exec_fileoff) / 4;
                    x->data[k >> 3] |= (uint8_t)(1u << (k & 7));
                }
            }
        }
    }
    /* the pass sees instruction sections only, not the islands it is finding; it adds them to the same bitmap */
    struct a64x_islands is = {x->data, fs, 0, 0, false, false, 0, 0, 0, 0};
    struct img_ctx c = {m, NULL};
    struct a64x_code_map cm = {&c, img_is_code, map_flags, NULL};
    for (uint32_t pg = 0; pg < x->npages; pg++) {
        uint64_t off = m->exec_fileoff + (uint64_t)pg * A64X_XLATE_PAGE;
        read_page(m, off, page);
        a64x_islands_page(&is, page, A64X_XLATE_PAGE / 4, (uint64_t)pg * (A64X_XLATE_PAGE / 4), off, &cm);
    }
    a64x_islands_end(&is, nbits);
    x->island_runs = is.runs;
    x->island_words = is.words;
    free(fs);
    return 0;
}

int a64x_image_islands(const struct a64x_macho *m, uint32_t map_flags, struct a64x_image_xlate *x)
{
    memset(x, 0, sizeof(*x));
    x->npages = (uint32_t)((m->exec_filesize + A64X_XLATE_PAGE - 1) / A64X_XLATE_PAGE);
    return find_islands(m, x, map_flags);
}

bool a64x_image_is_code(const struct a64x_macho *m, const uint8_t *data, uint64_t off)
{
    struct img_ctx c = {m, data};
    return img_is_code(&c, off);
}

int a64x_translate_image(const struct a64x_macho *m, uint32_t map_page, uint32_t extra_flags, uint8_t *out,
                         struct a64x_image_xlate *x, char *err, size_t errlen)
{
    memset(x, 0, sizeof(*x));
    x->kind = a64x_kind_of(m->cputype, m->cpusubtype, m->marked);
    if (out)
        memcpy(out, m->base, m->size);
    if (x->kind == A64X_KIND_NATIVE || x->kind == A64X_KIND_MARKED)
        return 0;
    if (m->nexec != 1) {
        /* a plain arm64 image the loader cannot lay out runs as is; arm64e text cannot */
        if (x->kind == A64X_KIND_ARM64) {
            x->kind = A64X_KIND_NATIVE;
            return 0;
        }
        return fail(err, errlen, m->nexec ? "more than one executable segment" : "no executable segment");
    }
    if (!map_page || (map_page & (map_page - 1)))
        return fail(err, errlen, "map page size %u", map_page);
    x->map_flags = (x->kind == A64X_KIND_ARM64 ? A64X_MAP_NO_PAC : 0) | extra_flags;
    x->npages = (uint32_t)((m->exec_filesize + A64X_XLATE_PAGE - 1) / A64X_XLATE_PAGE);
    /* stubs go right after the image (same distance in every process) */
    uint64_t image_size = m->vm_end - m->exec_vmaddr;
    x->stub_delta = (int64_t)((image_size + map_page - 1) & ~(uint64_t)(map_page - 1));
    x->cursor = calloc((size_t)x->npages + 1, sizeof(uint32_t));
    if (!x->cursor || find_islands(m, x, x->map_flags)) {
        a64x_image_xlate_free(x);
        return fail(err, errlen, "out of memory");
    }
    /* xnu/a64x_xnu.c a64x_scan(): one pass lays out the stubs and gives each page its output */
    struct img_ctx c = {m, x->data};
    struct a64x_code_map cm = {&c, img_is_code, x->map_flags, NULL};
    void *walk_buf = malloc(A64X_WALK_BYTES(A64X_XLATE_PAGE / 4));
    if (!walk_buf) {
        a64x_image_xlate_free(x);
        return fail(err, errlen, "out of memory");
    }
    struct a64x_walk walk = a64x_walk_in(walk_buf, A64X_XLATE_PAGE / 4);
    uint32_t page[A64X_XLATE_PAGE / 4], dst[A64X_XLATE_PAGE / 4], cursor = 0, cap = 0;
    for (uint32_t pg = 0; pg < x->npages; pg++) {
        uint64_t off = m->exec_fileoff + (uint64_t)pg * A64X_XLATE_PAGE;
        read_page(m, off, page);
        if (cap - cursor < (A64X_XLATE_PAGE / 4) * A64X_STUB_MAX) {
            uint32_t ncap = cap ? cap * 2 : 4 * (A64X_XLATE_PAGE / 4) * A64X_STUB_MAX;
            uint32_t *n = realloc(x->stubs, (size_t)ncap * sizeof(uint32_t));
            if (!n) {
                free(walk_buf);
                a64x_image_xlate_free(x);
                return fail(err, errlen, "out of memory");
            }
            x->stubs = n;
            cap = ncap;
        }
        struct a64x_stub_region sr = {m->exec_fileoff + (uint64_t)x->stub_delta, x->stubs, cap, 0, &walk};
        x->cursor[pg] = cursor;
        cursor = a64x_translate_page(page, dst, A64X_XLATE_PAGE / 4, off, &cm, &sr, cursor, &x->stats);
        uint64_t in_file = m->exec_filesize - (uint64_t)pg * A64X_XLATE_PAGE;
        size_t keep = in_file < A64X_XLATE_PAGE ? (size_t)in_file : A64X_XLATE_PAGE;
        for (size_t i = 0; i < keep / 4; i++)
            x->changed += dst[i] != page[i];
        if (out)
            memcpy(out + off, dst, keep);
    }
    free(walk_buf);
    x->cursor[x->npages] = cursor;
    x->nstubs = cursor;
    if (x->kind == A64X_KIND_ARM64 && x->changed == 0) {
        /* plain arm64 with nothing the A57 lacks: the file's own mapping (byte-identical, no pager) */
        x->kind = A64X_KIND_NATIVE;
    }
    return 0;
}

void a64x_image_xlate_free(struct a64x_image_xlate *x)
{
    free(x->data);
    free(x->cursor);
    free(x->stubs);
    x->data = NULL;
    x->cursor = NULL;
    x->stubs = NULL;
}

/* PC-relative branch: 1 and the target, 2 = conditional (falls through too), 0 = not a direct branch. */
static int branch_target(uint32_t w, uint64_t pc, uint64_t *t)
{
    if ((w & 0x7C000000u) == 0x14000000u) { /* B, BL */
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 6) >> 6) * 4);
        return (w & 0x80000000u) ? 3 : 1;
    }
    if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) { /* B.cond, CBZ/CBNZ */
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 8) >> 13) * 4);
        return ((w & 0xFF00000Fu) == 0x5400000Eu) ? 1 : 2; /* B.AL is unconditional */
    }
    if ((w & 0x7E000000u) == 0x36000000u) { /* TBZ/TBNZ */
        *t = pc + (uint64_t)((int64_t)((int32_t)(w << 13) >> 18) * 4);
        return 2;
    }
    return 0;
}

long a64x_verify_image(const struct a64x_macho *m, const uint8_t *out, const struct a64x_image_xlate *x, char *err,
                       size_t errlen)
{
    if (x->kind == A64X_KIND_NATIVE || x->kind == A64X_KIND_MARKED) {
        return memcmp(out, m->base, m->size) ? fail(err, errlen, "an untranslated image changed") : 0;
    }
    uint64_t stub_lo = m->exec_fileoff + (uint64_t)x->stub_delta, stub_hi = stub_lo + 4ull * x->nstubs;
    for (uint32_t i = 0; i < x->nstubs; i++) {
        enum a64x_class c = a64x_classify(x->stubs[i]);
        if (a64x_class_is_baseline(c))
            return fail(err, errlen, "stub word %u (%08x) is %s", i, x->stubs[i], a64x_class_name(c));
    }
    if (memcmp(out, m->base, m->exec_fileoff) ||
        memcmp(out + m->exec_fileoff + m->exec_filesize, m->base + m->exec_fileoff + m->exec_filesize,
               m->size - m->exec_fileoff - m->exec_filesize))
        return fail(err, errlen, "bytes outside the executable segment changed");
    uint8_t *seen = calloc(x->nstubs + 1, 1);
    uint32_t *stack = malloc((x->nstubs + 1) * 2 * sizeof(uint32_t));
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return fail(err, errlen, "out of memory");
    }
    long sites = 0;
    int bad = 0;
    for (uint64_t off = m->exec_fileoff; off + 4 <= m->exec_fileoff + m->exec_filesize && !bad; off += 4) {
        uint32_t was = rd32(m->base + off), now = rd32(out + off);
        if (was == now)
            continue;
        struct img_ctx c = {m, x->data};
        if (!img_is_code(&c, off)) {
            bad = fail(err, errlen, "data word at 0x%llx changed", (unsigned long long)off);
            break;
        }
        uint64_t t;
        int kind = branch_target(now, off, &t);
        if (kind != 1 || t < stub_lo || t >= stub_hi)
            continue;
        sites++;
        /* every path from the entry stays in the region until it branches to off + 4 */
        memset(seen, 0, x->nstubs);
        size_t sp = 0;
        stack[sp++] = (uint32_t)((t - stub_lo) / 4);
        while (sp && !bad) {
            uint32_t k = stack[--sp];
            for (;;) {
                if (k >= x->nstubs) {
                    bad = fail(err, errlen, "site 0x%llx: its stub runs off the region", (unsigned long long)off);
                    break;
                }
                if (seen[k])
                    break;
                seen[k] = 1;
                uint64_t pc = stub_lo + 4ull * k;
                uint32_t w = x->stubs[k];
                if ((w & 0xFE000000u) == 0xD6000000u) {
                    bad = fail(err, errlen, "site 0x%llx: register branch in its stub", (unsigned long long)off);
                    break;
                }
                int bk = branch_target(w, pc, &t);
                if (bk == 3) {
                    bad = fail(err, errlen, "site 0x%llx: BL in its stub", (unsigned long long)off);
                    break;
                }
                if (bk == 0) {
                    k++;
                    continue;
                }
                if (t == off + 4) {
                    if (bk == 2) {
                        k++;
                        continue;
                    }
                    break;
                }
                if (t < stub_lo || t >= stub_hi) {
                    bad = fail(err, errlen, "site 0x%llx: stub branches to 0x%llx", (unsigned long long)off,
                               (unsigned long long)t);
                    break;
                }
                if (bk == 2)
                    stack[sp++] = k + 1;
                k = (uint32_t)((t - stub_lo) / 4);
            }
        }
    }
    free(seen);
    free(stack);
    return bad ? -1 : sites;
}

static int arm64e_family(uint32_t format)
{
    return format == A64X_PTR_ARM64E || format == A64X_PTR_ARM64E_KERNEL || format == A64X_PTR_ARM64E_USERLAND ||
           format == A64X_PTR_ARM64E_FIRMWARE || format == A64X_PTR_ARM64E_USERLAND24;
}

uint32_t a64x_fixup_stride(uint32_t format)
{
    switch (format) {
    case A64X_PTR_ARM64E:
    case A64X_PTR_ARM64E_USERLAND:
    case A64X_PTR_ARM64E_USERLAND24:
    case A64X_PTR_64:
    case A64X_PTR_64_OFFSET:
        return 8;
    case A64X_PTR_ARM64E_KERNEL:
    case A64X_PTR_ARM64E_FIRMWARE:
    case A64X_PTR_64_KERNEL_CACHE:
        return 4;
    default:
        return 0;
    }
}

static uint64_t bits(uint64_t v, unsigned lo, unsigned n)
{
    return (v >> lo) & ((1ull << n) - 1);
}

int a64x_fixup_decode(uint32_t format, uint64_t raw, struct a64x_fixup *f)
{
    memset(f, 0, sizeof(*f));
    if (arm64e_family(format)) {
        f->auth = (int)bits(raw, 63, 1);
        f->bind = (int)bits(raw, 62, 1);
        f->next = (uint32_t)bits(raw, 51, 11);
        unsigned ordbits = format == A64X_PTR_ARM64E_USERLAND24 ? 24 : 16;
        if (f->auth) {
            f->diversity = (uint32_t)bits(raw, 32, 16);
            f->addr_div = (uint32_t)bits(raw, 48, 1);
            f->key = (uint32_t)bits(raw, 49, 2);
            if (f->bind)
                f->ordinal = (uint32_t)bits(raw, 0, ordbits);
            else
                f->target = bits(raw, 0, 32);
        } else if (f->bind) {
            f->ordinal = (uint32_t)bits(raw, 0, ordbits);
            f->addend = (int64_t)(bits(raw, 32, 19) << 45) >> 45;
        } else {
            f->target = bits(raw, 0, 43);
            f->high8 = (uint32_t)bits(raw, 43, 8);
        }
        return 0;
    }
    if (format == A64X_PTR_64_KERNEL_CACHE) {
        f->target = bits(raw, 0, 30);
        f->cache_level = (uint32_t)bits(raw, 30, 2);
        f->diversity = (uint32_t)bits(raw, 32, 16);
        f->addr_div = (uint32_t)bits(raw, 48, 1);
        f->key = (uint32_t)bits(raw, 49, 2);
        f->next = (uint32_t)bits(raw, 51, 12);
        f->auth = (int)bits(raw, 63, 1);
        return 0;
    }
    if (format == A64X_PTR_64 || format == A64X_PTR_64_OFFSET) {
        f->bind = (int)bits(raw, 63, 1);
        f->next = (uint32_t)bits(raw, 51, 12);
        if (f->bind) {
            f->ordinal = (uint32_t)bits(raw, 0, 24);
            f->addend = (int64_t)bits(raw, 24, 8);
        } else {
            f->target = bits(raw, 0, 36);
            f->high8 = (uint32_t)bits(raw, 36, 8);
        }
        return 0;
    }
    return -1;
}

int a64x_fixup_strip_auth(uint32_t format, uint64_t raw, uint64_t preferred_base, uint64_t *out)
{
    struct a64x_fixup f;
    if (a64x_fixup_decode(format, raw, &f))
        return -1;
    if (!f.auth) {
        *out = raw;
        return 0;
    }
    if (format == A64X_PTR_64_KERNEL_CACHE) {
        /* same target and cache level, no key/diversity, isAuth clear */
        *out = f.target | (uint64_t)f.cache_level << 30 | (uint64_t)f.next << 51;
        return 0;
    }
    uint64_t next = (uint64_t)f.next << 51;
    if (f.bind) {
        /* the plain bind of the same import, addend 0 (an auth bind has none) */
        *out = f.ordinal | next | 1ull << 62;
        return 0;
    }
    /* an auth rebase holds a runtime offset; the plain rebase of DYLD_CHAINED_PTR_ARM64E(_FIRMWARE) a vmaddr */
    uint64_t target = f.target;
    if (format == A64X_PTR_ARM64E || format == A64X_PTR_ARM64E_FIRMWARE)
        target += preferred_base;
    if (target >> 43)
        return -1;
    *out = target | next;
    return 0;
}

int a64x_fixups_each(const struct a64x_macho *m, a64x_fixup_fn each, void *ctx, char *err, size_t errlen)
{
    if (!m->fixups_size)
        return 0;
    const uint8_t *h = m->base + m->fixups_off;
    if (m->fixups_size < 28)
        return fail(err, errlen, "chained fixups header too short");
    uint32_t starts = rd32(h + 4);
    if (starts > m->fixups_size - 4)
        return fail(err, errlen, "chained starts past the fixups");
    const uint8_t *si = h + starts;
    uint32_t seg_count = rd32(si);
    if (seg_count > m->nsegs || (uint64_t)starts + 4 + 4ull * seg_count > m->fixups_size)
        return fail(err, errlen, "chained starts for %u segments, the image has %u", seg_count, m->nsegs);
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so = rd32(si + 4 + 4 * s);
        if (!so)
            continue;
        if ((uint64_t)starts + so + 22 > m->fixups_size)
            return fail(err, errlen, "segment %u: starts past the end", s);
        const uint8_t *ss = si + so;
        uint16_t page_size = rd16(ss + 4), format = rd16(ss + 6), page_count = rd16(ss + 20);
        uint32_t stride = a64x_fixup_stride(format);
        if (!stride)
            return fail(err, errlen, "segment %u: pointer format %u", s, format);
        if ((uint64_t)starts + so + 22 + 2ull * page_count > m->fixups_size)
            return fail(err, errlen, "segment %u: page starts past the end", s);
        for (uint32_t p = 0; p < page_count; p++) {
            uint16_t start = rd16(ss + 22 + 2 * p);
            if (start == 0xFFFF)
                continue; /* DYLD_CHAINED_PTR_START_NONE */
            uint32_t idx = p, nstarts = 1;
            uint16_t list[64];
            if (start & 0x8000) { /* DYLD_CHAINED_PTR_START_MULTI: a list ending in one with the LAST bit */
                nstarts = 0;
                for (uint32_t q = start & 0x7FFF; nstarts < 64; q++) {
                    if ((uint64_t)starts + so + 22 + 2ull * q + 2 > m->fixups_size)
                        return fail(err, errlen, "segment %u page %u: multi-start list past the end", s, p);
                    uint16_t v = rd16(ss + 22 + 2 * q);
                    list[nstarts++] = v & 0x7FFF;
                    if (v & 0x8000)
                        break;
                }
            } else {
                list[0] = start;
            }
            for (uint32_t n = 0; n < nstarts; n++) {
                uint64_t loc = m->seg_fileoff[s] + (uint64_t)idx * page_size + list[n];
                for (;;) {
                    if (loc < m->seg_fileoff[s] || loc - m->seg_fileoff[s] + 8 > m->seg_filesize[s] ||
                        loc + 8 > m->size)
                        return fail(err, errlen, "segment %u: chain past the segment at 0x%llx", s,
                                    (unsigned long long)loc);
                    uint64_t raw = rd64(m->base + loc);
                    struct a64x_fixup f;
                    if (a64x_fixup_decode(format, raw, &f))
                        return fail(err, errlen, "format %u", format);
                    const char *why = each(ctx, loc, format, raw, &f);
                    if (why)
                        return fail(err, errlen, "0x%llx: %s", (unsigned long long)loc, why);
                    if (!f.next)
                        break;
                    loc += (uint64_t)f.next * stride;
                }
            }
        }
    }
    return 0;
}

struct strip_ctx {
    const struct a64x_macho *m;
    uint8_t *out;
    struct a64x_fixup_stats *st;
};

static const char *strip_one(void *ctx, uint64_t loc, uint32_t format, uint64_t raw, const struct a64x_fixup *f)
{
    struct strip_ctx *c = ctx;
    uint64_t plain;
    if (f->bind)
        c->st->binds++;
    else
        c->st->rebases++;
    if (!f->auth)
        return NULL;
    if (f->bind)
        c->st->auth_binds++;
    else
        c->st->auth_rebases++;
    if (a64x_fixup_strip_auth(format, raw, c->m->text_vmaddr, &plain))
        return "auth target does not fit a plain rebase";
    if (c->out)
        memcpy(c->out + loc, &plain, sizeof(plain));
    c->st->stripped++;
    return NULL;
}

int a64x_fixups_strip_auth(const struct a64x_macho *m, uint8_t *out, struct a64x_fixup_stats *st, char *err,
                           size_t errlen)
{
    memset(st, 0, sizeof(*st));
    struct strip_ctx c = {m, out, st};
    return a64x_fixups_each(m, strip_one, &c, err, errlen);
}
