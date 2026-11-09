/*
 * a64plan2: translation plan for an arm64e dyld shared cache, second generation.
 *
 * Stub space comes from dead PAC failure checks inside the cache text (a64x_dead_checks: 3-word LR-check holes,
 * 5-word pointer-check holes). Per site, in this order:
 *   1. x30 dead at the site -> `BL shared` + `RET`: one stub per (instruction word, register set) per 128 MB window,
 *      shared by every site in range.
 *   2. a per-site stub (`B stub` ... `B site+4`) in ONE hole within +-128 MB if it fits (no extra branch),
 *   3. else split over holes of one 1 MB bin (each piece ends with `B next`),
 *   4. else through a veneer: site -> `B` in a hole up to 128 MB away -> stub up to 128 MB beyond -> back the same way.
 * Hot libraries are planned first. Sites that still fail keep their word (kernel trap path).
 *
 *   a64plan2 [-o PLAN] [-d DUMP_DISTINCT] DIR/dyld_shared_cache_arm64e
 */
#include "../a64x/a64x.h"
#include "../macho/stub_split.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t u32(const uint8_t *p, uint64_t o)
{
    uint32_t v;
    memcpy(&v, p + o, 4);
    return v;
}

static uint64_t u64(const uint8_t *p, uint64_t o)
{
    uint64_t v;
    memcpy(&v, p + o, 8);
    return v;
}

static const uint8_t *mapfile(const char *path, uint64_t *sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    struct stat st;
    fstat(fd, &st);
    void *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    *sz = st.st_size;
    return p;
}

struct fmap {
    uint64_t addr, size, off;
    int file;
    uint32_t prot;
};
static struct fmap fm[4096];
static int nfm;
static const uint8_t *fb[512];
static uint64_t fsz[512];
static uint8_t *codebits[512], *hotbits[512];

struct vrange {
    uint64_t lo, hi;
};

struct sec {
    uint64_t lo, hi;
    char name[34];
};
static struct sec *secs;
static size_t nsecs, capsecs;

static int cmp_sec(const void *x, const void *y)
{
    const struct sec *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static struct vrange *vr;
static size_t nvr, capvr;
static const char *const hot_images[] = {"/usr/lib/system/",
                                         "/usr/lib/libobjc",
                                         "/usr/lib/libc++",
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         "/usr/lib/libSystem",
                                         "/usr/lib/libz",
                                         "/Frameworks/CoreFoundation.framework/",
                                         "/Frameworks/Foundation.framework/",
                                         "/Frameworks/QuartzCore.framework/",
                                         "/UIKitCore.framework/",
                                         "/Frameworks/CoreGraphics.framework/",
                                         "/Frameworks/IOKit.framework/",
                                         "/Frameworks/Metal.framework/",
                                         "/IOSurface.framework/",
                                         "/BackBoardServices.framework/",
                                         "/FrontBoardServices.framework/",
                                         "/FrontBoard.framework/",
                                         "/SpringBoard",
                                         "/BaseBoard.framework/",
                                         "/RunningBoardServices.framework/",
                                         "/CoreText.framework/",
                                         "/libxpc",
                                         "/Security.framework/",
                                         "/CFNetwork.framework/",
                                         "/ImageIO.framework/",
                                         "/libsqlite3",
                                         "/CoreServices.framework/",
                                         "/MobileCoreServices.framework/",
                                         "/UIFoundation.framework/",
                                         "/libswiftDispatch",
                                         "/libswift_Concurrency",
                                         "/SwiftUI.framework/",
                                         "/AttributeGraph.framework/",
                                         "/CoreUI.framework/",
                                         "/libdyld",
                                         NULL};

static void mark_code(const uint8_t *m)
{
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    for (uint32_t i = 0; i < img_cnt; i++) {
        uint64_t addr = u64(m + img_off + 32 * i, 0);
        const char *ipath = (const char *)(m + u32(m + img_off + 32 * i, 24));
        bool hot = false;
        for (int h = 0; hot_images[h]; h++)
            if (strstr(ipath, hot_images[h]))
                hot = true;
        int hf = -1;
        uint64_t hoff = 0;
        for (int q = 0; q < nfm; q++)
            if (addr >= fm[q].addr && addr < fm[q].addr + fm[q].size) {
                hf = fm[q].file;
                hoff = fm[q].off + addr - fm[q].addr;
            }
        if (hf < 0)
            continue;
        const uint8_t *mh = fb[hf] + hoff;
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        for (uint32_t c = 0; c < ncmds; c++) {
            if (u32(lc, 0) == 0x19) {
                uint32_t ns = u32(lc, 64);
                for (uint32_t k = 0; k < ns; k++) {
                    const uint8_t *sp = lc + 72 + 80 * k;
                    uint64_t sa = u64(sp, 32), ss = u64(sp, 40);
                    if (!(u32(sp, 64) & 0x80000400u) || !ss)
                        continue;
                    if (nvr == capvr) {
                        capvr = capvr ? capvr * 2 : 65536;
                        vr = realloc(vr, capvr * sizeof(*vr));
                    }
                    vr[nvr].lo = sa;
                    vr[nvr].hi = sa + ss;
                    nvr++;
                    if (nsecs == capsecs) {
                        capsecs = capsecs ? capsecs * 2 : 65536;
                        secs = realloc(secs, capsecs * sizeof(*secs));
                    }
                    secs[nsecs].lo = sa;
                    secs[nsecs].hi = sa + ss;
                    snprintf(secs[nsecs].name, sizeof(secs[nsecs].name), "%.16s,%.16s", (const char *)(sp + 16),
                             (const char *)sp);
                    nsecs++;
                    for (int q = 0; q < nfm; q++)
                        if (sa >= fm[q].addr && sa + ss <= fm[q].addr + fm[q].size) {
                            uint64_t fo = fm[q].off + sa - fm[q].addr;
                            for (uint64_t x = fo / 4; x < (fo + ss) / 4; x++) {
                                codebits[fm[q].file][x >> 3] |= 1 << (x & 7);
                                if (hot)
                                    hotbits[fm[q].file][x >> 3] |= 1 << (x & 7);
                            }
                        }
                }
            }
            lc += u32(lc, 4);
        }
    }
}

/*
 * TEXT_STUBS mappings (dyld_cache_mapping_and_slide_info flag 1 << 3) hold the cross-image stubs of the text next
 * to them (adrp/add/ldr/braa): code that belongs to no image's sections.
 */
static void mark_stub_mappings(int nfiles)
{
    for (int f = 0; f < nfiles; f++) {
        uint32_t so = u32(fb[f], 0x138), sc = u32(fb[f], 0x13C);
        for (uint32_t i = 0; i < sc; i++) {
            const uint8_t *e = fb[f] + so + 56 * i;
            uint64_t addr = u64(e, 0), size = u64(e, 8), off = u64(e, 16);
            if (!(u64(e, 40) & (1u << 3)))
                continue;
            if (nvr == capvr) {
                capvr = capvr ? capvr * 2 : 65536;
                vr = realloc(vr, capvr * sizeof(*vr));
            }
            vr[nvr++] = (struct vrange){addr, addr + size};
            if (nsecs == capsecs) {
                capsecs = capsecs ? capsecs * 2 : 65536;
                secs = realloc(secs, capsecs * sizeof(*secs));
            }
            secs[nsecs].lo = addr;
            secs[nsecs].hi = addr + size;
            snprintf(secs[nsecs].name, sizeof(secs[nsecs].name), "[TEXT_STUBS]");
            nsecs++;
            for (uint64_t x = off / 4; x < (off + size) / 4; x++)
                codebits[f][x >> 3] |= 1 << (x & 7);
        }
    }
}

/* every function start of every image (LC_FUNCTION_STARTS, read through the cache's LINKEDIT mapping) */
static uint64_t *fstarts;
static size_t nfstarts, capfs;
static int map_of(uint64_t va);

static const uint8_t *vmptr(uint64_t va, uint64_t len)
{
    int q = map_of(va);
    if (q < 0 || va + len > fm[q].addr + fm[q].size)
        return NULL;
    return fb[fm[q].file] + fm[q].off + (va - fm[q].addr);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void load_function_starts(const uint8_t *m)
{
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    for (uint32_t i = 0; i < img_cnt; i++) {
        uint64_t addr = u64(m + img_off + 32 * i, 0);
        const uint8_t *mh = vmptr(addr, 32);
        if (!mh)
            continue;
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        uint64_t text_vm = 0, le_vm = 0, le_fo = 0;
        uint32_t fs_off = 0, fs_size = 0;
        for (uint32_t c = 0; c < ncmds; c++) {
            uint32_t cmd = u32(lc, 0);
            if (cmd == 0x19) {
                if (!memcmp(lc + 8, "__TEXT\0", 7))
                    text_vm = u64(lc, 24);
                if (!memcmp(lc + 8, "__LINKEDIT", 10)) {
                    le_vm = u64(lc, 24);
                    le_fo = u64(lc, 40);
                }
            } else if (cmd == 0x26) {
                fs_off = u32(lc, 8);
                fs_size = u32(lc, 12);
            }
            lc += u32(lc, 4);
        }
        if (!fs_size || !le_vm)
            continue;
        const uint8_t *data = vmptr(le_vm + (fs_off - le_fo), fs_size);
        if (!data)
            continue;
        uint64_t cur = text_vm;
        for (uint32_t q = 0; q < fs_size;) {
            uint64_t v = 0;
            int sh = 0;
            while (q < fs_size) {
                uint8_t b = data[q++];
                v |= (uint64_t)(b & 0x7f) << sh;
                sh += 7;
                if (!(b & 0x80))
                    break;
            }
            if (!v)
                break;
            cur += v;
            if (nfstarts == capfs) {
                capfs = capfs ? capfs * 2 : 1 << 22;
                fstarts = realloc(fstarts, capfs * sizeof(uint64_t));
            }
            fstarts[nfstarts++] = cur;
        }
    }
    qsort(fstarts, nfstarts, sizeof(uint64_t), cmp_u64);
    printf("function starts: %zu\n", nfstarts);
}

/* [start, end) of the function holding va, or 0 if unknown */
static uint64_t function_of(uint64_t va, uint64_t *end)
{
    size_t lo = 0, hi = nfstarts;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (fstarts[mid] <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    *end = lo < nfstarts ? fstarts[lo] : va + 4;
    return fstarts[lo - 1];
}

static int map_of(uint64_t va)
{
    for (int q = 0; q < nfm; q++)
        if (va >= fm[q].addr && va < fm[q].addr + fm[q].size)
            return q;
    return -1;
}

static bool plan_is_code(void *ctx, uint64_t va)
{
    (void)ctx;
    int q = map_of(va);
    if (q < 0)
        return false;
    uint64_t x = (fm[q].off + va - fm[q].addr) / 4;
    return codebits[fm[q].file][x >> 3] & (1 << (x & 7));
}

static uint8_t *refbits[512];

static bool plan_is_target(void *ctx, uint64_t va)
{
    (void)ctx;
    int q = map_of(va);
    if (q < 0)
        return false;
    uint64_t x = (fm[q].off + va - fm[q].addr) / 4;
    return refbits[fm[q].file] && (refbits[fm[q].file][x >> 3] & (1 << (x & 7)));
}
static const struct a64x_code_map plan_code_map = {NULL, plan_is_code, 0, plan_is_target};

static uint32_t word_at(uint64_t va)
{
    int q = map_of(va);
    return q < 0 ? 0 : u32(fb[fm[q].file], fm[q].off + va - fm[q].addr);
}

static uint8_t *reachbits[512];
static uint64_t *dic_lo, *dic_hi;
static size_t ndic, capdic;

static bool __attribute__((unused)) reached(uint64_t va)
{
    int q = map_of(va);
    if (q < 0)
        return false;
    uint64_t x = (fm[q].off + va - fm[q].addr) / 4;
    return reachbits[fm[q].file][x >> 3] & (1 << (x & 7));
}

static void load_data_in_code(const uint8_t *m)
{
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    for (uint32_t i = 0; i < img_cnt; i++) {
        uint64_t addr = u64(m + img_off + 32 * i, 0);
        const uint8_t *mh = vmptr(addr, 32);
        if (!mh)
            continue;
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        uint64_t le_vm = 0, le_fo = 0;
        uint32_t off = 0, size = 0;
        for (uint32_t c = 0; c < ncmds; c++) {
            if (u32(lc, 0) == 0x19 && !memcmp(lc + 8, "__LINKEDIT", 10)) {
                le_vm = u64(lc, 24);
                le_fo = u64(lc, 40);
            }
            if (u32(lc, 0) == 0x29) {
                off = u32(lc, 8);
                size = u32(lc, 12);
            }
            lc += u32(lc, 4);
        }
        if (!size || !le_vm)
            continue;
        const uint8_t *d = vmptr(le_vm + (off - le_fo), size);
        if (!d)
            continue;
        for (uint32_t k = 0; k + 8 <= size; k += 8) {
            if (ndic == capdic) {
                capdic = capdic ? capdic * 2 : 65536;
                dic_lo = realloc(dic_lo, capdic * 8);
                dic_hi = realloc(dic_hi, capdic * 8);
            }
            dic_lo[ndic] = addr + u32(d, k);
            dic_hi[ndic] = addr + u32(d, k) + (u32(d, k + 4) & 0xFFFF);
            ndic++;
        }
    }
    printf("data-in-code ranges: %zu\n", ndic);
}

static bool in_dic(uint64_t va)
{
    for (size_t i = 0; i < ndic; i++)
        if (va >= dic_lo[i] && va < dic_hi[i])
            return true;
    return false;
}

/* landing pads: __unwind_info's LSDA index, then each LSDA's call-site table (offsets from the function start) */
static uint64_t *lpads;
static size_t nlpads, caplpads;
static uint64_t n_lsda, n_lsda_bad;

static uint64_t read_enc(const uint8_t **pp, const uint8_t *end, uint8_t enc, bool *ok)
{
    const uint8_t *p = *pp;
    uint64_t v = 0;
    int sh = 0;
    switch (enc & 0x0F) {
    case 0x01:
    case 0x09:
        do {
            if (p >= end) {
                *ok = false;
                return 0;
            }
            v |= (uint64_t)(*p & 0x7F) << sh;
            sh += 7;
        } while (*p++ & 0x80);
        if ((enc & 0x0F) == 0x09 && sh < 64 && (p[-1] & 0x40))
            v |= ~0ull << sh;
        break;
    case 0x02:
    case 0x0A:
        if (p + 2 > end) {
            *ok = false;
            return 0;
        }
        v = p[0] | p[1] << 8;
        if (enc & 8)
            v = (uint64_t)(int16_t)v;
        p += 2;
        break;
    case 0x03:
    case 0x0B:
        if (p + 4 > end) {
            *ok = false;
            return 0;
        }
        memcpy(&v, p, 4);
        v &= 0xFFFFFFFFu;
        if (enc & 8)
            v = (uint64_t)(int32_t)v;
        p += 4;
        break;
    case 0x04:
    case 0x0C:
        if (p + 8 > end) {
            *ok = false;
            return 0;
        }
        memcpy(&v, p, 8);
        p += 8;
        break;
    default:
        *ok = false;
        return 0;
    }
    *pp = p;
    return v;
}

static void load_landing_pads(const uint8_t *m)
{
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    for (uint32_t i = 0; i < img_cnt; i++) {
        uint64_t base = u64(m + img_off + 32 * i, 0);
        const uint8_t *mh = vmptr(base, 32);
        if (!mh)
            continue;
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        uint64_t ui = 0, uisz = 0;
        for (uint32_t c = 0; c < ncmds; c++) {
            if (u32(lc, 0) == 0x19 && !memcmp(lc + 8, "__TEXT\0", 7)) {
                for (uint32_t k = 0; k < u32(lc, 64); k++) {
                    const uint8_t *sp = lc + 72 + 80 * k;
                    if (!strncmp((const char *)sp, "__unwind_info", 16)) {
                        ui = u64(sp, 32);
                        uisz = u64(sp, 40);
                    }
                }
            }
            lc += u32(lc, 4);
        }
        const uint8_t *u = ui ? vmptr(ui, uisz) : NULL;
        if (!u || uisz < 28 || u32(u, 0) != 1)
            continue;
        uint32_t idx = u32(u, 20), nidx = u32(u, 24);
        if (!nidx || idx + 12ull * nidx > uisz)
            continue;
        uint32_t l0 = u32(u, idx + 8), l1 = u32(u, idx + 12 * (nidx - 1) + 8);
        if (l1 < l0 || l1 > uisz)
            continue;
        for (uint32_t o = l0; o + 8 <= l1; o += 8) {
            uint64_t fstart = base + u32(u, o), lsda = base + u32(u, o + 4);
            const uint8_t *p = vmptr(lsda, 16), *end;
            if (!p) {
                n_lsda_bad++;
                continue;
            }
            int q = map_of(lsda);
            end = fb[fm[q].file] + fm[q].off + fm[q].size;
            bool ok = true;
            n_lsda++;
            uint8_t lpenc = *p++;
            uint64_t lpstart = fstart;
            if (lpenc != 0xFF) {
                lpstart = read_enc(&p, end, lpenc, &ok);
            }
            uint8_t ttenc = *p++;
            if (ttenc != 0xFF)
                (void)read_enc(&p, end, 0x01, &ok);
            uint8_t csenc = *p++;
            uint64_t cslen = read_enc(&p, end, 0x01, &ok);
            const uint8_t *cs_end = p + cslen;
            if (!ok || cs_end > end) {
                n_lsda_bad++;
                continue;
            }
            while (ok && p < cs_end) {
                (void)read_enc(&p, cs_end, csenc, &ok);
                (void)read_enc(&p, cs_end, csenc, &ok);
                uint64_t lp = read_enc(&p, cs_end, csenc, &ok);
                (void)read_enc(&p, cs_end, 0x01, &ok);
                if (ok && lp) {
                    if (nlpads == caplpads) {
                        caplpads = caplpads ? caplpads * 2 : 1 << 20;
                        lpads = realloc(lpads, caplpads * 8);
                    }
                    lpads[nlpads++] = lpstart + lp;
                }
            }
            if (!ok)
                n_lsda_bad++;
        }
    }
    printf("landing pads: %zu from %llu LSDAs (%llu unreadable)\n", nlpads, (unsigned long long)n_lsda,
           (unsigned long long)n_lsda_bad);
}

static uint64_t n_indirect_br, n_jt, n_jt_targets, n_jt_unresolved;

/* The targets of the arm64e hardened jump table whose BR is at va (a64x_jump_table): how many were pushed, 0 for an
 * unresolved one, -1 if va is no such dispatch. */
static int jump_table(uint64_t va, uint64_t **stack, size_t *sp, size_t *cap)
{
    uint32_t w[9];
    for (int k = 0; k < 9; k++)
        w[k] = word_at(va - 4 * (8 - k));
    uint64_t table, bse, n;
    if (!a64x_jump_table(w, va, &table, &bse, &n))
        return -1;
    const uint8_t *t = n ? vmptr(table, 4 * n) : NULL;
    if (!t)
        return 0;
    for (uint64_t i = 0; i < n; i++) {
        int32_t d;
        memcpy(&d, t + 4 * i, 4);
        if (*sp == *cap) {
            *cap *= 2;
            *stack = realloc(*stack, *cap * 8);
        }
        (*stack)[(*sp)++] = bse + (int64_t)d;
    }
    return (int)n;
}

/* a word no A12-class code can hold: unallocated top-level groups, non-zero reserved space, SVE/SME, Apple AMX */
static bool impossible_word(uint32_t w)
{
    uint32_t op0 = (w >> 25) & 0xF;
    return (op0 == 0 && (w >> 16)) || op0 == 1 || op0 == 2 || op0 == 3;
}

/*
 * Unallocated in every A-profile ISA up to v9.6 (SVE/SME/AMX are allocated: real code for newer cores sits behind feature
 * checks in functions A12-class cores also run): the top-level groups 0001 and 0011, and the reserved group without SME
 * (bit 31 clear) other than UDF and Apple's AMX page.
 */
static bool unallocated_word(uint32_t w)
{
    uint32_t op0 = (w >> 25) & 0xF;
    return op0 == 1 || op0 == 3 || (op0 == 0 && !(w >> 31) && (w >> 16) && (w & 0xFFFFF000u) != 0x00201000u);
}

static uint8_t *fsbits[512];
static uint64_t n_roots_rejected;
#define BITV(b, x) ((b)[(x) >> 3] & (1 << ((x) & 7)))

static bool va_index(uint64_t va, int *f, uint64_t *x)
{
    int q = map_of(va);
    if (q < 0)
        return false;
    *f = fm[q].file;
    *x = (fm[q].off + va - fm[q].addr) / 4;
    return true;
}

static bool is_terminator(uint32_t e)
{
    return (e & 0xFFE0001Fu) == 0xD4200000u || (e & 0xFFFF0000u) == 0 || e == 0xD65F03C0u ||
           (e & 0xFFFFFC1Fu) == 0xD61F0000u || (e & 0xFC000000u) == 0x14000000u;
}

/*
 * A function start is taken as code only if its first straight-line run (to the first terminator, the next function start, or
 * 64 words) holds no word that A12-class code cannot hold: assembler data tables in __text carry symbols, hence starts.
 */
static bool probe_root(uint64_t va)
{
    int f;
    uint64_t x;
    if (!va_index(va, &f, &x))
        return false;
    for (int k = 0; k < 64; k++, x++) {
        if (!BITV(codebits[f], x))
            return k > 0;
        if (k && BITV(fsbits[f], x))
            return true;
        uint32_t e = u32(fb[f], 4 * x), c;
        if (unallocated_word(e))
            return false;
        if (a64x_rewrite1(e, &c))
            e = c;
        if (is_terminator(e))
            return true;
    }
    return true;
}

static void reach_pass(void)
{
    size_t cap = 1 << 25, sp = 0;
    uint64_t *stack = malloc(cap * 8);
    for (int f = 0; f < 512; f++)
        if (fb[f]) {
            reachbits[f] = calloc(fsz[f] / 32 + 1, 1);
            fsbits[f] = calloc(fsz[f] / 32 + 1, 1);
        }
    for (size_t i = 0; i < nfstarts; i++) {
        int f;
        uint64_t x;
        if (va_index(fstarts[i], &f, &x))
            fsbits[f][x >> 3] |= 1 << (x & 7);
    }
    for (size_t i = 0; i < nfstarts; i++) {
        if (!probe_root(fstarts[i])) {
            n_roots_rejected++;
            continue;
        }
        if (sp == cap) {
            cap *= 2;
            stack = realloc(stack, cap * 8);
        }
        stack[sp++] = fstarts[i];
    }
    for (size_t i = 0; i < nlpads; i++) {
        if (sp == cap) {
            cap *= 2;
            stack = realloc(stack, cap * 8);
        }
        stack[sp++] = lpads[i];
    }
    while (sp) {
        uint64_t va = stack[--sp];
        bool fell = false;
        for (;;) {
            int q = map_of(va);
            if (q < 0)
                break;
            uint64_t x = (fm[q].off + va - fm[q].addr) / 4;
            int f = fm[q].file;
            if (!BITV(codebits[f], x))
                break;
            if (BITV(reachbits[f], x))
                break;
            if (fell && BITV(fsbits[f], x))
                break; /* falling into the next function start: it is judged as its own root */
            reachbits[f][x >> 3] |= 1 << (x & 7);
            uint32_t e = u32(fb[f], 4 * x), c;
            if (a64x_rewrite1(e, &c))
                e = c;
            if ((e & 0xFFE0001Fu) == 0xD4200000u || (e & 0xFFFF0000u) == 0 || e == 0xD65F03C0u)
                break;                              /* BRK, UDF, RET */
            if ((e & 0xFFFFFC1Fu) == 0xD61F0000u) { /* BR */
                int t = jump_table(va, &stack, &sp, &cap);
                if (t > 0) {
                    n_jt++;
                    n_jt_targets += (uint64_t)t;
                } else if (t == 0)
                    n_jt_unresolved++;
                else
                    n_indirect_br++;
                break;
            }
            int64_t tgt = 0;
            bool br = false, uncond = false;
            if ((e & 0xFF000010u) == 0x54000000u) {
                tgt = (int32_t)(((e >> 5) & 0x7FFFF) << 13) >> 13;
                br = true;
            } else if ((e & 0x7E000000u) == 0x34000000u) {
                tgt = (int32_t)(((e >> 5) & 0x7FFFF) << 13) >> 13;
                br = true;
            } else if ((e & 0x7E000000u) == 0x36000000u) {
                tgt = (int32_t)(((e >> 5) & 0x3FFF) << 18) >> 18;
                br = true;
            } else if ((e & 0xFC000000u) == 0x14000000u) {
                tgt = (int32_t)((e & 0x3FFFFFF) << 6) >> 6;
                br = uncond = true;
            } else if ((e & 0xFC000000u) == 0x94000000u) {
                tgt = (int32_t)((e & 0x3FFFFFF) << 6) >> 6;
                br = true;
            }
            if (br) {
                if (sp == cap) {
                    cap *= 2;
                    stack = realloc(stack, cap * 8);
                }
                stack[sp++] = va + 4 * tgt;
                if (uncond)
                    break;
            }
            va += 4;
            fell = true;
        }
    }
    free(stack);
    printf("reach roots: %zu function starts (%llu rejected as data), %zu landing pads\n", nfstarts,
           (unsigned long long)n_roots_rejected, nlpads);
    printf("jump tables: %llu resolved (%llu targets), %llu unresolved; other indirect BR %llu\n",
           (unsigned long long)n_jt, (unsigned long long)n_jt_targets, (unsigned long long)n_jt_unresolved,
           (unsigned long long)n_indirect_br);
}

/*
 * Data islands in __text (assembler tables such as corecrypto's K256, libm's sincos coefficients, AudioCodecs' Hann
 * windows). Recursive descent from the validated function starts, the landing pads and the resolved jump tables marks code.
 * What it does not reach is cut into runs at UDF/zero padding; a run dense in unallocated encodings (at least one word in
 * 32), starting at a function start that failed its probe, or running into padding without ending its code (lzfse's
 * 64-bit mask table, single words between zero halves) is data and leaves the code map: nothing in it is rewritten, here
 * or by the kernel, which takes its code ranges from the plan. Other unreached runs (dead __auth_stubs left by the cache
 * builder's stub elimination, code behind unresolved indirect branches) stay code.
 */
static uint64_t isl_runs, isl_words, isl_mod, isl_sites, code_runs, code_runs_mod, code_mod_words, code_sites,
    reached_impossible, tail_runs, tail_words;
static FILE *isl_out;
static uint64_t ri_hist[4];
static uint64_t cls_unreached_code[64];

static const char *sec_of(uint64_t va)
{
    size_t lo = 0, hi = nsecs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (secs[mid].lo <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo && va < secs[lo - 1].hi ? secs[lo - 1].name : "?";
}

static void drop_data_islands(void)
{
    struct {
        char name[34];
        uint64_t words;
    } bysec[64];

    int nbysec = 0;
    for (int q = 0; q < nfm; q++) {
        if (!(fm[q].prot & 4))
            continue;
        int f = fm[q].file;
        uint64_t x0 = fm[q].off / 4, xn = x0 + fm[q].size / 4;
        for (uint64_t x = x0; x < xn;) {
            if (!BITV(codebits[f], x)) {
                x++;
                continue;
            }
            if (BITV(reachbits[f], x)) {
                uint32_t w = u32(fb[f], 4 * x);
                if (impossible_word(w)) {
                    reached_impossible++;
                    uint32_t op0 = (w >> 25) & 0xF;
                    int k = op0 == 2 ? 0 : op0 == 0 && (w >> 31) ? 1 : (w & 0xFFFFF000u) == 0x00201000u ? 2 : 3;
                    if (k == 3 && isl_out)
                        fprintf(isl_out, "REACHED-UNALLOC %llx %08x\n", (unsigned long long)(fm[q].addr + 4 * (x - x0)),
                                w);
                    ri_hist[k]++;
                }
                x++;
                continue;
            }
            if (!(u32(fb[f], 4 * x) >> 16)) {
                x++;
                continue;
            } /* UDF / zero padding: never rewritten */
            uint64_t y = x;
            bool bad = BITV(fsbits[f], x);
            int nmod = 0, nsite = 0, nun = 0;
            for (; y < xn && BITV(codebits[f], y) && !BITV(reachbits[f], y); y++) {
                uint32_t w = u32(fb[f], 4 * y), one;
                if (!(w >> 16))
                    break;
                if (unallocated_word(w))
                    nun++;
                enum a64x_class c = a64x_classify(w);
                bool st = c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP;
                bool md = st || a64x_rewrite1(w, &one);
                nmod += md;
                nsite += st;
            }
            uint64_t va = fm[q].addr + 4 * (x - x0);
            /*
			 * Tables are dense in unallocated words (8-50% here); obfuscated code (CoreALD, MFAAuthentication) reached only
			 * through register branches carries a stray junk word per few hundred instructions. One in 32 separates them.
			 */
            if (nun && (uint64_t)nun * 32 >= y - x)
                bad = true;
            /*
			 * Into padding (not into reached code) without ending its code: the words after the last one that ends code
			 * are data (a64x_ends_code), the code before them stays (dav1d's assembly keeps each jump table right behind
			 * the blocks it dispatches to).
			 */
            const bool padded = y == xn || !BITV(codebits[f], y) || !(u32(fb[f], 4 * y) >> 16);
            if (!bad && padded && !a64x_ends_code(u32(fb[f], 4 * (y - 1)))) {
                uint64_t t = y - 1;
                while (t > x && !a64x_ends_code(u32(fb[f], 4 * (t - 1))))
                    t--;
                if (t == x) {
                    bad = true;
                } else {
                    tail_runs++;
                    tail_words += y - t;
                    if (isl_out)
                        fprintf(isl_out, "TAIL %llx +%llu %s\n", (unsigned long long)(fm[q].addr + 4 * (t - x0)),
                                (unsigned long long)(y - t), sec_of(va));
                    for (uint64_t z = t; z < y; z++)
                        codebits[f][z >> 3] &= (uint8_t)~(1 << (z & 7));
                }
            }
            if (bad) {
                isl_runs++;
                isl_words += y - x;
                isl_mod += nmod;
                isl_sites += nsite;
                if (isl_out && nmod)
                    fprintf(isl_out, "DATA %llx +%llu rewrites %d sites %d %s unalloc %d fs %d\n",
                            (unsigned long long)va, (unsigned long long)(y - x), nmod, nsite, sec_of(va), nun,
                            BITV(fsbits[f], x) ? 1 : 0);
                for (uint64_t z = x; z < y; z++)
                    codebits[f][z >> 3] &= (uint8_t)~(1 << (z & 7));
            } else {
                code_runs++;
                code_runs_mod += nmod > 0;
                code_mod_words += nmod;
                code_sites += nsite;
                if (nmod) {
                    const char *sn = sec_of(va);
                    int b;
                    for (b = 0; b < nbysec && strcmp(bysec[b].name, sn); b++) {
                    }
                    if (b == nbysec && nbysec < 64) {
                        strcpy(bysec[nbysec].name, sn);
                        bysec[nbysec++].words = 0;
                    }
                    if (b < nbysec)
                        bysec[b].words += nmod;
                    for (uint64_t z = x; z < y; z++) {
                        uint32_t w = u32(fb[f], 4 * z), one;
                        enum a64x_class c = a64x_classify(w);
                        if (c != A64X_OK || a64x_rewrite1(w, &one))
                            cls_unreached_code[c & 63]++;
                    }
                }
                if (isl_out && nmod && strstr(sec_of(va), "__text"))
                    fprintf(isl_out, "CODE %llx +%llu rewrites %d sites %d %s\n", (unsigned long long)va,
                            (unsigned long long)(y - x), nmod, nsite, sec_of(va));
            }
            x = y;
        }
    }
    printf("impossible words in reached code: sve %llu sme %llu amx %llu unallocated %llu\n",
           (unsigned long long)ri_hist[0], (unsigned long long)ri_hist[1], (unsigned long long)ri_hist[2],
           (unsigned long long)ri_hist[3]);
    printf(
        "data islands: %llu runs, %llu words, %llu would-be-rewritten words (%llu LSE sites) left alone; data behind "
        "code: %llu runs, %llu words\n",
        (unsigned long long)isl_runs, (unsigned long long)isl_words, (unsigned long long)isl_mod,
        (unsigned long long)isl_sites, (unsigned long long)tail_runs, (unsigned long long)tail_words);
    printf("unreached code-like runs: %llu (%llu with %llu rewrites, %llu LSE sites); by section:",
           (unsigned long long)code_runs, (unsigned long long)code_runs_mod, (unsigned long long)code_mod_words,
           (unsigned long long)code_sites);
    for (int b = 0; b < nbysec; b++)
        printf(" %s %llu", bysec[b].name, (unsigned long long)bysec[b].words);
    printf("\n  by class:");
    for (int c = 0; c < 64; c++)
        if (cls_unreached_code[c])
            printf(" %s %llu", a64x_class_name(c), (unsigned long long)cls_unreached_code[c]);
    printf("\n");
}

/* code ranges for the plan, from the code map (after data islands left it) */
static void ranges_from_codebits(void)
{
    nvr = 0;
    for (int q = 0; q < nfm; q++) {
        if (!(fm[q].prot & 4))
            continue;
        int f = fm[q].file;
        uint64_t x0 = fm[q].off / 4, xn = x0 + fm[q].size / 4;
        for (uint64_t x = x0; x < xn;) {
            if (!(codebits[f][x >> 3] & (1 << (x & 7)))) {
                x++;
                continue;
            }
            uint64_t y = x;
            while (y < xn && (codebits[f][y >> 3] & (1 << (y & 7))))
                y++;
            if (nvr == capvr) {
                capvr = capvr ? capvr * 2 : 65536;
                vr = realloc(vr, capvr * sizeof(*vr));
            }
            vr[nvr].lo = fm[q].addr + 4 * (x - x0);
            vr[nvr].hi = fm[q].addr + 4 * (y - x0);
            nvr++;
            x = y;
        }
    }
}

/*
 * Every code address something enters other than by falling into it: function starts, landing pads, the targets of
 * direct branches, ADR and ADRP+ADD in code, jump tables, and every rebase of the cache's data (slide info v5). No
 * hole may hold one (plan_is_target): compilers share a tail-call PAC check between a signed path and an unsigned early
 * return that branches straight to its EOR (libsystem_trace's _logDestructor in 24B119).
 */
static int xmaps[4096], nxmaps;
static uint64_t n_ref_code, n_ref_data;

static int cmp_xmap(const void *a, const void *b)
{
    const struct fmap *x = &fm[*(const int *)a], *y = &fm[*(const int *)b];
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void mark_ref(uint64_t va)
{
    int lo = 0, hi = nxmaps;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (fm[xmaps[mid]].addr + fm[xmaps[mid]].size <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == nxmaps || va < fm[xmaps[lo]].addr)
        return;
    const struct fmap *q = &fm[xmaps[lo]];
    uint64_t x = (q->off + va - q->addr) / 4;
    refbits[q->file][x >> 3] |= (uint8_t)(1 << (x & 7));
}

static void mark_jump_table(uint64_t va)
{
    uint32_t w[9];
    for (int k = 0; k < 9; k++)
        w[k] = word_at(va - 4 * (8 - k));
    uint64_t table, bse, n;
    const uint8_t *t;
    if (!a64x_jump_table(w, va, &table, &bse, &n) || !n || !(t = vmptr(table, 4 * n)))
        return;
    for (uint64_t i = 0; i < n; i++) {
        int32_t d;
        memcpy(&d, t + 4 * i, 4);
        mark_ref(bse + (int64_t)d);
    }
}

static void mark_slide_rebases(int f)
{
    uint32_t so = u32(fb[f], 0x138), sc = u32(fb[f], 0x13C);
    for (uint32_t i = 0; i < sc; i++) {
        const uint8_t *e = fb[f] + so + 56 * i;
        uint64_t fileoff = u64(e, 16), info = u64(e, 24), info_size = u64(e, 32);
        if (info_size < 24 || u32(fb[f], info) != 5)
            continue;
        uint32_t page_size = u32(fb[f], info + 4), npages = u32(fb[f], info + 8);
        uint64_t value_add = u64(fb[f], info + 16);
        if (24 + 2ull * npages > info_size)
            continue;
        for (uint32_t p = 0; p < npages; p++) {
            uint16_t start;
            memcpy(&start, fb[f] + info + 24 + 2 * p, 2);
            if (start == 0xFFFF)
                continue;
            uint64_t page = fileoff + (uint64_t)p * page_size, loc = page + start;
            while (loc + 8 <= page + page_size && loc + 8 <= fsz[f]) {
                uint64_t v = u64(fb[f], loc);
                mark_ref(value_add + (v & 0x3FFFFFFFFull));
                n_ref_data++;
                uint64_t delta = ((v >> 52) & 0x7FF) * 8;
                if (!delta)
                    break;
                loc += delta;
            }
        }
    }
}

static void mark_refs(void)
{
    for (int f = 0; f < 512; f++)
        if (fb[f])
            refbits[f] = calloc(fsz[f] / 32 + 1, 1);
    nxmaps = 0;
    for (int q = 0; q < nfm; q++)
        if (fm[q].prot & 4)
            xmaps[nxmaps++] = q;
    qsort(xmaps, (size_t)nxmaps, sizeof(*xmaps), cmp_xmap);
    for (size_t i = 0; i < nfstarts; i++)
        mark_ref(fstarts[i]);
    for (size_t i = 0; i < nlpads; i++)
        mark_ref(lpads[i]);
    for (int i = 0; i < nxmaps; i++) {
        const struct fmap *q = &fm[xmaps[i]];
        const uint32_t *w = (const uint32_t *)(fb[q->file] + q->off);
        uint32_t n = (uint32_t)(q->size / 4);
        for (uint32_t k = 0; k < n; k++) {
            uint64_t x = q->off / 4 + k, to[1];
            if (!BITV(codebits[q->file], x))
                continue;
            if (w[k] == 0xD61F0200u)
                mark_jump_table(q->addr + 4ull * k);
            if (a64x_code_refs(w, n, k, q->addr, to)) {
                mark_ref(to[0]);
                n_ref_code++;
            }
        }
    }
    for (int f = 0; f < 512; f++)
        if (fb[f])
            mark_slide_rebases(f);
    printf("entries: %llu code references, %llu data rebases, %zu function starts, %zu landing pads\n",
           (unsigned long long)n_ref_code, (unsigned long long)n_ref_data, nfstarts, nlpads);
}

struct hole {
    uint64_t va;
    uint8_t size, used;
};
static struct hole *holes;
static size_t nholes, capholes;
static uint64_t hole_page_va, n3, n5;

static void add_hole(void *ctx, uint32_t index, uint32_t nwords)
{
    (void)ctx;
    if (nholes == capholes) {
        capholes = capholes ? capholes * 2 : 1 << 20;
        holes = realloc(holes, capholes * sizeof(*holes));
    }
    holes[nholes++] = (struct hole){hole_page_va + 4 * (uint64_t)index, (uint8_t)nwords, 0};
    if (nwords == 3)
        n3++;
    else
        n5++;
}

static int cmp_hole(const void *x, const void *y)
{
    const struct hole *a = x, *b = y;
    return a->va < b->va ? -1 : a->va > b->va;
}

/* "next unused hole" to the right / left, per hole size class (3 or 5), with path compression */
struct dsu {
    int64_t *idx;
    size_t n;
    int64_t *nx, *pv;
};
static struct dsu d3, d5;

static void dsu_build(struct dsu *d, int size)
{
    d->n = 0;
    d->idx = malloc(nholes * sizeof(int64_t));
    for (size_t i = 0; i < nholes; i++)
        if (holes[i].size == size)
            d->idx[d->n++] = (int64_t)i;
    d->nx = malloc((d->n + 1) * sizeof(int64_t));
    d->pv = malloc((d->n + 1) * sizeof(int64_t));
    for (size_t i = 0; i <= d->n; i++) {
        d->nx[i] = (int64_t)i;
        d->pv[i] = (int64_t)i;
    }
}

static int64_t dfind_r(struct dsu *d, int64_t i)
{
    int64_t r = i;
    while (r < (int64_t)d->n && d->nx[r] != r)
        r = d->nx[r];
    while (i < (int64_t)d->n && d->nx[i] != i) {
        int64_t t = d->nx[i];
        d->nx[i] = r;
        i = t;
    }
    return r;
}

static int64_t dfind_l(struct dsu *d, int64_t i)
{
    int64_t r = i;
    while (r >= 0 && d->pv[r] != r)
        r = d->pv[r];
    while (i >= 0 && d->pv[i] != i) {
        int64_t t = d->pv[i];
        d->pv[i] = r;
        i = t;
    }
    return r;
}

static void dsu_take(struct dsu *d, int64_t i)
{
    d->nx[i] = i + 1;
    d->pv[i] = i - 1;
    holes[d->idx[i]].used = 1;
}

/* nearest unused hole of this class to `va`, within `range`; returns hole index or -1 */
static int64_t nearest(struct dsu *d, uint64_t va, uint64_t range, int64_t *dsu_pos)
{
    size_t lo = 0, hi = d->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (holes[d->idx[mid]].va < va)
            lo = mid + 1;
        else
            hi = mid;
    }
    int64_t r = dfind_r(d, (int64_t)lo), l = dfind_l(d, (int64_t)lo - 1);
    uint64_t dr = r < (int64_t)d->n ? holes[d->idx[r]].va - va : UINT64_MAX;
    uint64_t dl = l >= 0 ? va - holes[d->idx[l]].va : UINT64_MAX;
    int64_t pick = dl <= dr ? l : r;
    uint64_t dist = dl <= dr ? dl : dr;
    if (dist == UINT64_MAX || dist > range)
        return -1;
    *dsu_pos = pick;
    return d->idx[pick];
}

struct patch {
    uint64_t va;
    uint32_t w;
};
static struct patch *patches;
static size_t npatch, cappatch;
static const char *ctx_tag = "?";
static uint64_t ctx_site;

static void add_patch(uint64_t va, uint32_t w)
{
    (void)ctx_tag;
    (void)ctx_site;
    if (npatch == cappatch) {
        cappatch = cappatch ? cappatch * 2 : 1 << 20;
        patches = realloc(patches, cappatch * sizeof(*patches));
    }
    patches[npatch].va = va;
    patches[npatch].w = w;
    npatch++;
}

static uint32_t enc_bl(uint64_t from, uint64_t to)
{
    return 0x94000000u | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3FFFFFF);
}

/*
 * Place a linear stub st[0..n) (laid out for address `dummy`; branches outside it keep their absolute target) into
 * holes. Prefers one hole of size >= n within `range` of `near`; else pieces from one 1 MB bin. Returns the
 * address of its first instruction, or 0. pieces_out: number of holes used.
 */
#define BIN_SHIFT 20
static uint64_t bin_base;
static int64_t nbins, *bin_room, *bin_first, *bin_end;

static bool commit(const uint32_t *st, int n, uint64_t dummy, const uint64_t *addr, const uint64_t *links, int nlinks)
{
    struct a64x_placed placed[A64X_STUB_MAX * 2 + 4];
    int np = a64x_stub_split(st, n, dummy, addr, links, nlinks, placed);
    if (np < 0)
        return false;
    for (int i = 0; i < np; i++)
        add_patch(placed[i].at, placed[i].word);
    return true;
}

static uint64_t stat_single, stat_pieces_sites, stat_pieces_total;

static uint64_t place(const uint32_t *st, int n, uint64_t dummy, uint64_t near, uint64_t range, int *pieces_out)
{
    uint64_t addr[A64X_STUB_MAX];
    /* 1. one hole that holds it all */
    int64_t pos3 = -1, pos5 = -1;
    int64_t h5 = nearest(&d5, near, range, &pos5);
    int64_t h3 = n <= 3 ? nearest(&d3, near, range, &pos3) : -1;
    int64_t h = -1;
    struct dsu *dd = NULL;
    int64_t pos = -1;
    if (h3 >= 0 && (h5 < 0 || (holes[h3].va > near ? holes[h3].va - near : near - holes[h3].va) <=
                                  (holes[h5].va > near ? holes[h5].va - near : near - holes[h5].va))) {
        h = h3;
        dd = &d3;
        pos = pos3;
    } else if (h5 >= 0 && n <= 5) {
        h = h5;
        dd = &d5;
        pos = pos5;
    }
    if (h >= 0) {
        if (holes[h].used) {
            static int warned;
            if (warned++ < 5)
                fprintf(stderr, "place: DSU returned a used hole %llx (size %d, pos %lld)\n",
                        (unsigned long long)holes[h].va, holes[h].size, (long long)pos);
        }
        for (int j = 0; j < n; j++)
            addr[j] = holes[h].va + 4 * (uint64_t)j;
        if (commit(st, n, dummy, addr, NULL, 0)) {
            dsu_take(dd, pos);
            int64_t bb = (int64_t)((holes[h].va - bin_base) >> BIN_SHIFT);
            bin_room[bb] -= holes[h].size - 1;
            *pieces_out = 1;
            stat_single++;
            return addr[0];
        }
    }
    /* 2. pieces from the nearest bin with room (bins whose room was overcounted are zeroed and skipped) */
    int64_t sb = (int64_t)((near - bin_base) >> BIN_SHIFT), chosen = -1;
    int64_t maxd = (int64_t)(range >> BIN_SHIFT);
    int64_t used[A64X_STUB_MAX];
    int nu = 0, cap = 0;
    for (int64_t dlt = 0; dlt <= maxd && chosen < 0; dlt++) {
        for (int sg = 0; sg < 2 && chosen < 0; sg++) {
            int64_t b = sg ? sb - dlt : sb + dlt;
            if ((dlt == 0 && sg) || b < 0 || b >= nbins || bin_room[b] < n + 3)
                continue;
            uint64_t bva = bin_base + ((uint64_t)b << BIN_SHIFT);
            if ((bva > near ? bva + (1u << BIN_SHIFT) - near : near - bva) > range)
                continue;
            nu = 0;
            cap = 0;
            for (int64_t i = bin_first[b]; i < bin_end[b] && cap < n && nu < A64X_STUB_MAX; i++) {
                if (holes[i].used)
                    continue;
                used[nu++] = i;
                cap += holes[i].size - 1;
                if (cap + 1 >= n)
                    cap = n;
            }
            if (cap < n) {
                bin_room[b] = 0;
                continue;
            }
            chosen = b;
        }
    }
    if (chosen < 0)
        return 0;
    uint64_t links[A64X_STUB_MAX];
    int nl = 0, ki = 0;
    for (int i = 0; i < nu && ki < n; i++) {
        int room = holes[used[i]].size, take = n - ki <= room ? n - ki : room - 1;
        for (int j = 0; j < take; j++)
            addr[ki + j] = holes[used[i]].va + 4 * (uint64_t)j;
        ki += take;
        if (ki < n)
            links[nl++] = holes[used[i]].va + 4 * (uint64_t)take;
    }
    if (!commit(st, n, dummy, addr, links, nl))
        return 0;
    for (int i = 0; i < nu; i++) {
        holes[used[i]].used = 1;
        bin_room[chosen] -= holes[used[i]].size - 1;
        /* keep the per-class DSUs consistent */
        struct dsu *dc = holes[used[i]].size == 3 ? &d3 : &d5;
        size_t lo = 0, hi = dc->n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (dc->idx[mid] < used[i])
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < dc->n && dc->idx[lo] == used[i])
            dsu_take(dc, (int64_t)lo);
    }
    while (bin_first[chosen] < bin_end[chosen] && holes[bin_first[chosen]].used)
        bin_first[chosen]++;
    *pieces_out = nu;
    stat_pieces_sites++;
    stat_pieces_total += (uint64_t)nu;
    return addr[0];
}

struct shared {
    uint32_t word;
    uint8_t variant;
    uint64_t va;
    struct shared *next;
};

#define SH_BUCKETS (1u << 16)
static struct shared *sh_tab[SH_BUCKETS];
static uint64_t sh_copies, sh_words;

static uint64_t shared_stub(uint32_t w, uint8_t variant, uint64_t site, int *pieces)
{
    uint32_t key = (w * 2654435761u ^ variant) & (SH_BUCKETS - 1);
    for (struct shared *s = sh_tab[key]; s; s = s->next) {
        if (s->word == w && s->variant == variant) {
            int64_t d = (int64_t)(s->va - site);
            if (d > -(120LL << 20) && d < (120LL << 20)) {
                *pieces = 0;
                return s->va;
            }
        }
    }
    uint32_t st[A64X_STUB_MAX];
    uint64_t dummy = site + 8;
    int n = a64x_stub_v(w, site, dummy, (variant & 1) ? ((1u << 16) | (1u << 17)) : 0, (variant & 2) ? 0xFFFF0000u : 0,
                        st, A64X_STUB_MAX);
    if (n <= 0 || n > A64X_STUB_MAX)
        return 0;
    st[n - 1] = 0xD65F03C0; /* ret */
    uint64_t va = place(st, n, dummy, site, 100u << 20, pieces);
    if (!va)
        return 0;
    struct shared *s = malloc(sizeof(*s));
    *s = (struct shared){w, variant, va, sh_tab[key]};
    sh_tab[key] = s;
    sh_copies++;
    sh_words += (uint64_t)n;
    return va;
}

static int cmp_patch(const void *x, const void *y)
{
    const struct patch *a = x, *b = y;
    return a->va < b->va ? -1 : a->va > b->va;
}

static int cmp_vr(const void *x, const void *y)
{
    const struct vrange *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

static void write_plan(const char *path, const uint8_t *m)
{
    qsort(patches, npatch, sizeof(*patches), cmp_patch);
    qsort(vr, nvr, sizeof(*vr), cmp_vr);
    size_t nm = 0;
    for (size_t i = 0; i < nvr; i++) {
        if (nm && vr[i].lo <= vr[nm - 1].hi) {
            if (vr[i].hi > vr[nm - 1].hi)
                vr[nm - 1].hi = vr[i].hi;
        } else
            vr[nm++] = vr[i];
    }
    nvr = nm;

    struct pp {
        uint64_t va;
        uint32_t first, count;
    } *pages = NULL;

    size_t np = 0, capp = 0;
    for (size_t i = 0; i < npatch; i++) {
        uint64_t pva = patches[i].va & ~4095ull;
        if (!np || pages[np - 1].va != pva) {
            if (np == capp) {
                capp = capp ? capp * 2 : 1 << 16;
                pages = realloc(pages, capp * sizeof(*pages));
            }
            pages[np].va = pva;
            pages[np].first = (uint32_t)i;
            pages[np].count = 0;
            np++;
        }
        pages[np - 1].count++;
    }

    struct {
        char magic[8];
        uint8_t uuid[16];
        uint32_t nranges, npages;
        uint64_t ranges_off, pages_off, words_off, size;
    } h;

    memcpy(h.magic, "A64XPLN1", 8);
    memcpy(h.uuid, m + 0x58, 16);
    h.nranges = (uint32_t)nvr;
    h.npages = (uint32_t)np;
    h.ranges_off = sizeof(h);
    h.pages_off = h.ranges_off + nvr * 16;
    h.words_off = h.pages_off + np * 16;
    h.size = h.words_off + npatch * 8;
    FILE *o = fopen(path, "wb");
    fwrite(&h, sizeof(h), 1, o);
    fwrite(vr, 16, nvr, o);
    for (size_t i = 0; i < np; i++) {
        fwrite(&pages[i].va, 8, 1, o);
        fwrite(&pages[i].first, 4, 1, o);
        fwrite(&pages[i].count, 4, 1, o);
    }
    for (size_t i = 0; i < npatch; i++) {
        uint16_t idx = (uint16_t)((patches[i].va & 4095) / 4), pad = 0;
        fwrite(&idx, 2, 1, o);
        fwrite(&pad, 2, 1, o);
        fwrite(&patches[i].w, 4, 1, o);
    }
    fclose(o);
    printf("plan written: %s: %zu code ranges, %zu pages, %zu words, %.1f MB\n", path, nvr, np, npatch,
           h.size / 1048576.0);
}

static uint32_t patch_at(uint64_t va, bool *found)
{
    size_t lo = 0, hi = npatch;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (patches[mid].va < va)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < npatch && patches[lo].va == va;
    return *found ? patches[lo].w : 0;
}

/* follow a translated site through planned words: B chains end with a B to site+4, BL chains with RET */
static int follow(uint64_t site, uint32_t w)
{
    /* every path through the stub (conditional branches included) must stay in plan words and end at the return */
    bool bl = (w & 0xFC000000u) == 0x94000000u;
    uint64_t work[64], seen[512];
    int nw = 0, ns = 0;
    work[nw++] = site + ((int64_t)(int32_t)((w & 0x3FFFFFFu) << 6) >> 4);
    while (nw) {
        uint64_t pc = work[--nw];
        for (int step = 0; step < 512; step++) {
            bool dup = false;
            for (int k = 0; k < ns; k++)
                if (seen[k] == pc)
                    dup = true;
            if (dup)
                break;
            if (ns == 512)
                return 0;
            seen[ns++] = pc;
            bool f;
            uint32_t x = patch_at(pc, &f);
            if (!f)
                return 0;
            if (bl && x == 0xD65F03C0u)
                break;
            if ((x & 0xFC000000u) == 0x14000000u) {
                uint64_t t = pc + ((int64_t)(int32_t)((x & 0x3FFFFFFu) << 6) >> 4);
                if (!bl && t == site + 4)
                    break;
                pc = t;
                continue;
            }
            bool isb;
            int64_t off = a64x_branch_offset(x, &isb);
            if (isb) { /* conditional: both ways */
                if (nw == 64)
                    return 0;
                work[nw++] = pc + (uint64_t)off;
            }
            if (x == 0xD65F03C0u || (x & 0xFFFFFC1Fu) == 0xD61F0000u)
                return 0; /* stray return / indirect branch */
            pc += 4;
            if (step == 511)
                return 0;
        }
    }
    return 1;
}

static uint64_t chain_sites, chain_hops;

/* nearest bin with room for n words, any distance; returns bin index or -1 */
static int64_t nearest_bin_any(uint64_t near, int n)
{
    int64_t sb = (int64_t)((near - bin_base) >> BIN_SHIFT);
    for (int64_t dlt = 0; dlt < nbins; dlt++) {
        for (int sg = 0; sg < 2; sg++) {
            int64_t b = sg ? sb - dlt : sb + dlt;
            if ((dlt == 0 && sg) || b < 0 || b >= nbins)
                continue;
            if (bin_room[b] >= n + 3)
                return b;
        }
    }
    return -1;
}

/*
 * site -> V1 -> V2 ... -> stub; stub -> Vk.out -> ... -> V1.out -> site+4. Each veneer is a 3- or 5-word hole
 * holding [B forward][B back]. Hops are at most ~115 MB.
 */
static bool place_chain(uint32_t w, uint32_t dead, uint64_t site)
{
    uint32_t st[A64X_STUB_MAX];
    int n0 = a64x_stub(w, site, site + 8, dead, st, A64X_STUB_MAX);
    if (n0 <= 0 || n0 > A64X_STUB_MAX)
        return false;
    int64_t tb = nearest_bin_any(site, n0);
    static int dbg;
    if (tb < 0) {
        if (getenv("CHAIN_DEBUG") && dbg++ < 10)
            fprintf(stderr, "chain %llx: no bin\n", (unsigned long long)site);
        return false;
    }
    uint64_t target = bin_base + ((uint64_t)tb << BIN_SHIFT) + (1u << (BIN_SHIFT - 1));
    uint64_t ven[8], pos_last = site;
    struct dsu *vd[8];
    int64_t vpos[8], vh;
    int k = 0;
    bool fail = false;
    while (!fail && (target > pos_last ? target - pos_last : pos_last - target) > (120u << 20)) {
        if (k == 8) {
            fail = true;
            break;
        }
        uint64_t want = target > pos_last ? pos_last + (120u << 20) : pos_last - (120u << 20);
        int64_t p3, p5;
        int64_t h3 = nearest(&d3, want, 100u << 20, &p3), h5 = nearest(&d5, want, 100u << 20, &p5);
        if (h3 < 0 && h5 < 0) {
            fail = true;
            break;
        }
        bool use3 = h3 >= 0 && (h5 < 0 || (holes[h3].va > want ? holes[h3].va - want : want - holes[h3].va) <=
                                              (holes[h5].va > want ? holes[h5].va - want : want - holes[h5].va));
        vh = use3 ? h3 : h5;
        uint64_t dold = target > pos_last ? target - pos_last : pos_last - target;
        uint64_t dnew = target > holes[vh].va ? target - holes[vh].va : holes[vh].va - target;
        if (!a64x_branch(pos_last, holes[vh].va) || dnew + (8u << 20) > dold) {
            fail = true;
            break;
        }
        vd[k] = use3 ? &d3 : &d5;
        vpos[k] = use3 ? p3 : p5;
        if (holes[vh].used) {
            static int w2;
            if (w2++ < 5)
                fprintf(stderr, "chain: DSU returned a used hole %llx size %d\n", (unsigned long long)holes[vh].va,
                        holes[vh].size);
        }
        ven[k] = holes[vh].va;
        dsu_take(vd[k], vpos[k]); /* reserve now so the next hop does not pick it */
        pos_last = ven[k];
        k++;
    }
    if (fail && getenv("CHAIN_DEBUG") && dbg++ < 10) {
        fprintf(stderr, "chain %llx: target bin %lld (%+lld MB) failed after %d hops at %llx\n",
                (unsigned long long)site, (long long)tb, (long long)((int64_t)(target - site) >> 20), k,
                (unsigned long long)pos_last);
    }
    if (fail) {
        for (int i = 0; i < k; i++) {
            vd[i]->nx[vpos[i]] = vpos[i];
            vd[i]->pv[vpos[i]] = vpos[i];
            holes[vd[i]->idx[vpos[i]]].used = 0;
        }
        return false;
    }
    /* the stub returns to the last veneer's second word (or to site + 4 when there is no veneer) */
    uint64_t ret_to = k ? ven[k - 1] + 4 : site + 4;
    int n = a64x_stub(w, ret_to - 4, pos_last + 8, dead, st, A64X_STUB_MAX);
    int pieces = 0;
    for (int i = 0; i < k; i++)
        if (!holes[vd[i]->idx[vpos[i]]].used || holes[vd[i]->idx[vpos[i]]].va != ven[i]) {
            static int w3;
            if (w3++ < 5)
                fprintf(stderr, "chain: veneer %d at %llx not marked (hole va %llx used %d)\n", i,
                        (unsigned long long)ven[i], (unsigned long long)holes[vd[i]->idx[vpos[i]]].va,
                        holes[vd[i]->idx[vpos[i]]].used);
        }
    uint64_t sva = place(st, n, pos_last + 8, pos_last, 125u << 20, &pieces);
    for (int i = 0; i < k; i++)
        if (sva == ven[i]) {
            static int w4;
            if (w4++ < 5)
                fprintf(stderr, "chain: stub placed on veneer %d at %llx (pieces %d, n %d)\n", i,
                        (unsigned long long)ven[i], pieces, n);
        }
    if (!sva && getenv("CHAIN_DEBUG") && dbg++ < 10)
        fprintf(stderr, "chain %llx: stub not placed near %llx (target %+lld MB, k %d, n %d)\n",
                (unsigned long long)site, (unsigned long long)pos_last, (long long)((int64_t)(target - site) >> 20), k,
                n);
    if (!sva) {
        /* give the veneer holes back (rare) */
        for (int i = 0; i < k; i++) {
            vd[i]->nx[vpos[i]] = vpos[i];
            vd[i]->pv[vpos[i]] = vpos[i];
            holes[vd[i]->idx[vpos[i]]].used = 0;
        }
        return false;
    }
    uint64_t prev = site;
    for (int i = 0; i < k; i++) {
        uint64_t next_in = i + 1 < k ? ven[i + 1] : sva;
        add_patch(ven[i], a64x_branch(ven[i], next_in));
        add_patch(ven[i] + 4, a64x_branch(ven[i] + 4, i ? ven[i - 1] + 4 : site + 4));
        (void)prev;
    }
    add_patch(site, a64x_branch(site, k ? ven[0] : sva));
    chain_sites++;
    chain_hops += (uint64_t)k;
    return true;
}

double *g_dens;

int cmpd(const void *a, const void *b2)
{
    double x = g_dens[*(const size_t *)a], y = g_dens[*(const size_t *)b2];
    return x < y ? 1 : x > y ? -1 : 0;
}

struct site {
    uint64_t va;
    uint32_t w, dead, vdead;
    uint8_t cls, lr_dead, hot;
};

static uint64_t fp_sites, fp_vspill_before, fp_vspill_after, fp_words_before, fp_words_after;

/* V temporaries the stub for w uses when V16-V31 are all free (the shared "V dead" variant) */
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

static bool has_vspill(const uint32_t *st, int n)
{
    for (int k = 0; k < n; k++) {
        if ((st[k] & 0xFFC003E0u) == 0xAD8003E0u || (st[k] & 0xFFE00FE0u) == 0x3C800FE0u)
            return true; /* STP/STR Q, [sp, #-x]! */
    }
    return false;
}

static int wide_budget = 2000;
static FILE *cmp_out;
static uint64_t size_hist[A64X_STUB_MAX + 1];

int a64x_cmd_plan2(int argc, char **argv)
{
    int ch;
    const char *outp = NULL, *dump = NULL, *unplaced_path = NULL;
    int no_shared = 0, no_hops = 0;
    while ((ch = getopt(argc, argv, "o:d:u:SH")) != -1) {
        if (ch == 'u')
            unplaced_path = optarg;
        if (ch == 'o')
            outp = optarg;
        if (ch == 'd')
            dump = optarg;
        if (ch == 'S')
            no_shared = 1;
        if (ch == 'H')
            no_hops = 1;
    }
    const char *mainpath = argv[optind];
    uint64_t msz;
    const uint8_t *m = mapfile(mainpath, &msz);
    uint32_t sub_off = u32(m, 0x188), sub_cnt = u32(m, 0x18C);
    for (uint32_t f = 0; f <= sub_cnt; f++) {
        char path[4096];
        if (f == 0)
            snprintf(path, sizeof(path), "%s", mainpath);
        else
            snprintf(path, sizeof(path), "%s%.32s", mainpath, (const char *)(m + sub_off + 56 * (f - 1) + 24));
        fb[f] = mapfile(path, &fsz[f]);
        codebits[f] = calloc(fsz[f] / 32 + 1, 1);
        hotbits[f] = calloc(fsz[f] / 32 + 1, 1);
        uint32_t moff = u32(fb[f], 0x10), mcnt = u32(fb[f], 0x14);
        for (uint32_t i = 0; i < mcnt; i++) {
            const uint8_t *e = fb[f] + moff + 32 * i;
            fm[nfm++] = (struct fmap){u64(e, 0), u64(e, 8), u64(e, 16), (int)f, u32(e, 28)};
        }
    }
    mark_code(m);
    mark_stub_mappings((int)sub_cnt + 1);
    load_function_starts(m);
    if (getenv("A64X_CMP"))
        cmp_out = fopen(getenv("A64X_CMP"), "w");
    qsort(secs, nsecs, sizeof(*secs), cmp_sec);
    load_landing_pads(m);
    reach_pass();
    if (getenv("A64X_ISLANDS"))
        isl_out = fopen(getenv("A64X_ISLANDS"), "w");
    uint8_t *orig_code[512] = {0};
    if (getenv("A64X_SIMPLE"))
        for (int f = 0; f < 512; f++)
            if (fb[f]) {
                orig_code[f] = malloc(fsz[f] / 32 + 1);
                memcpy(orig_code[f], codebits[f], fsz[f] / 32 + 1);
            }
    drop_data_islands();
    if (getenv("A64X_SIMPLE")) {
        /*
		 * The kernel's cheap rule for images without a plan (a64x_islands_*: runs cut at UDF, non-code and function
		 * starts; >= 1/32 unallocated = data) against the full analysis above.
		 */
        uint64_t miss_rw = 0, miss_words = 0, lost_code = 0, lost_rw = 0, simple_words = 0;
        for (int q = 0; q < nfm; q++) {
            if (!(fm[q].prot & 4))
                continue;
            int f = fm[q].file;
            uint64_t x0 = fm[q].off / 4, n = fm[q].size / 4;
            uint8_t *data = calloc(n / 8 + 1, 1), *fsl = calloc(n / 8 + 1, 1);
            for (uint64_t k = 0; k < n; k++)
                if (BITV(fsbits[f], x0 + k))
                    fsl[k >> 3] |= 1 << (k & 7);
            struct a64x_islands is = {data, fsl, 0, 0, false, false, 0, 0, 0, 0};

            struct ctx_t {
                uint8_t *bits;
                uint64_t base, x0;
            } cx = {orig_code[f], fm[q].addr, x0};

            (void)cx;
            for (uint64_t k = 0; k < n; k += 1024) {
                uint32_t nw = k + 1024 <= n ? 1024 : (uint32_t)(n - k), wbuf[1024];
                for (uint32_t i = 0; i < nw; i++) {
                    uint64_t x = x0 + k + i;
                    /* non-code words as zero: they close runs like the kernel's code map does */
                    wbuf[i] = BITV(orig_code[f], x) ? u32(fb[f], 4 * x) : 0;
                }
                a64x_islands_page(&is, wbuf, nw, k, fm[q].addr + 4 * k, NULL);
            }
            a64x_islands_end(&is, n);
            simple_words += is.words;
            for (uint64_t k = 0; k < n; k++) {
                uint64_t x = x0 + k;
                if (!BITV(orig_code[f], x))
                    continue;
                uint32_t w = u32(fb[f], 4 * x), one;
                enum a64x_class c = a64x_classify(w);
                bool rw = c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP ||
                          a64x_rewrite1(w, &one) || c == A64X_FP16 || c == A64X_FCMA || c == A64X_JSCVT ||
                          c == A64X_RDM;
                bool full_data = !BITV(codebits[f], x), simple_data = data[k >> 3] & (1 << (k & 7));
                if (full_data && !simple_data) {
                    miss_words++;
                    miss_rw += rw;
                    if (rw && isl_out)
                        fprintf(isl_out, "MISS %llx %08x\n", (unsigned long long)(fm[q].addr + 4 * k), w);
                }
                if (simple_data && BITV(reachbits[f], x)) {
                    lost_code++;
                    lost_rw += rw;
                    if (rw && isl_out)
                        fprintf(isl_out, "LOST %llx %08x\n", (unsigned long long)(fm[q].addr + 4 * k), w);
                }
            }
            free(data);
            free(fsl);
        }
        printf("simple island rule: %llu data words; vs full analysis: %llu data words missed (%llu of them would be "
               "rewritten), "
               "%llu reached code words called data (%llu of them need a rewrite)\n",
               (unsigned long long)simple_words, (unsigned long long)miss_words, (unsigned long long)miss_rw,
               (unsigned long long)lost_code, (unsigned long long)lost_rw);
        return 0;
    }
    if (isl_out)
        fclose(isl_out);
    ranges_from_codebits();
    if (getenv("A64X_CLASSDUMP")) {
        /* every reached code word of the FP16/FCMA/JSCVT/RDM classes: "class word va" */
        FILE *o = fopen(getenv("A64X_CLASSDUMP"), "w");
        for (int q = 0; q < nfm; q++) {
            if (!(fm[q].prot & 4))
                continue;
            int f = fm[q].file;
            for (uint64_t k = 0; k < fm[q].size / 4; k++) {
                uint64_t x = fm[q].off / 4 + k;
                if (!BITV(codebits[f], x))
                    continue;
                uint32_t w = u32(fb[f], 4 * x);
                enum a64x_class c = a64x_classify(w);
                if (c == A64X_FP16 || c == A64X_FCMA || c == A64X_JSCVT || c == A64X_RDM)
                    fprintf(o, "%s %08x %llx %d %d\n", a64x_class_name(c), w, (unsigned long long)(fm[q].addr + 4 * k),
                            BITV(reachbits[f], x) ? 1 : 0, BITV(hotbits[f], x) ? 1 : 0);
            }
        }
        fclose(o);
        return 0;
    }
    if (getenv("A64X_FIND")) {
        uint32_t want[8];
        int nw = 0;
        for (char *t = strtok(getenv("A64X_FIND"), ","); t && nw < 8; t = strtok(NULL, ","))
            want[nw++] = (uint32_t)strtoul(t, NULL, 16);
        for (int q = 0; q < nfm; q++) {
            if (!(fm[q].prot & 4))
                continue;
            int f = fm[q].file;
            for (uint64_t k = 0; k < fm[q].size / 4; k++) {
                uint64_t x = fm[q].off / 4 + k;
                if (!BITV(codebits[f], x))
                    continue;
                uint32_t w = u32(fb[f], 4 * x);
                for (int i = 0; i < nw; i++)
                    if (w == want[i])
                        printf("FOUND %08x at %llx reached %d\n", w, (unsigned long long)(fm[q].addr + 4 * k),
                               BITV(reachbits[f], x) ? 1 : 0);
            }
        }
        return 0;
    }
    if (getenv("A64X_REACH")) {
        load_data_in_code(m);
        FILE *ro = fopen(getenv("A64X_REACH"), "w");
        uint64_t code = 0, rch = 0, mod = 0, mod_un = 0, site = 0, site_un = 0, site_dic = 0, mod_dic = 0;
        for (int q = 0; q < nfm; q++) {
            if (!(fm[q].prot & 4))
                continue;
            int f = fm[q].file;
            for (uint64_t k = 0; k < fm[q].size / 4; k++) {
                uint64_t x = fm[q].off / 4 + k;
                if (!(codebits[f][x >> 3] & (1 << (x & 7))))
                    continue;
                code++;
                bool r = reachbits[f][x >> 3] & (1 << (x & 7));
                rch += r;
                uint32_t w = u32(fb[f], 4 * x), one;
                enum a64x_class c = a64x_classify(w);
                bool st = c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP;
                bool md = st || a64x_rewrite1(w, &one) || c != A64X_OK;
                if (!md)
                    continue;
                uint64_t va = fm[q].addr + 4 * k;
                bool dic = ndic && in_dic(va);
                mod++;
                mod_un += !r;
                mod_dic += dic;
                if (st) {
                    site++;
                    site_un += !r;
                    site_dic += dic;
                }
                if (!r)
                    fprintf(ro, "%s %llx %08x %s\n", st ? "SITE" : "WORD", (unsigned long long)va, w,
                            a64x_class_name(c));
            }
        }
        fclose(ro);
        printf("reach: code words %llu reached %llu (%.2f%%), indirect BR stops %llu\n", (unsigned long long)code,
               (unsigned long long)rch, 100.0 * rch / code, (unsigned long long)n_indirect_br);
        printf("reach: modified words %llu unreached %llu in-data-in-code %llu; LSE sites %llu unreached %llu "
               "in-data-in-code %llu\n",
               (unsigned long long)mod, (unsigned long long)mod_un, (unsigned long long)mod_dic,
               (unsigned long long)site, (unsigned long long)site_un, (unsigned long long)site_dic);
        return 0;
    }

    mark_refs();
    /* holes and sites, page by page (the kernel's rule for holes) */
    struct site *sites = NULL;
    size_t nsites = 0, capsites = 0;
    void *walk_buf = malloc(A64X_WALK_BYTES(65536));
    struct a64x_walk walk = a64x_walk_in(walk_buf, 65536);
    for (int q = 0; q < nfm; q++) {
        if (!(fm[q].prot & 4))
            continue;
        const uint8_t *t = fb[fm[q].file] + fm[q].off;
        uint64_t n = fm[q].size / 4, base = fm[q].addr, fo0 = fm[q].off;
        int f = fm[q].file;
        for (uint64_t pg = 0; pg < n; pg += 1024) {
            uint32_t in[1024], out[1024];
            uint32_t nw = pg + 1024 <= n ? 1024 : (uint32_t)(n - pg);
            for (uint32_t x = 0; x < 1024; x++)
                in[x] = x < nw ? u32(t, (pg + x) * 4) : 0;
            memcpy(out, in, sizeof(in));
            hole_page_va = base + 4 * pg;
            a64x_dead_checks(in, out, nw, base + 4 * pg, &plan_code_map, add_hole, NULL);
            for (uint32_t x = 0; x < nw; x++) {
                uint64_t fo = fo0 + 4 * (pg + x);
                if (!(codebits[f][(fo / 4) >> 3] & (1 << ((fo / 4) & 7))))
                    continue;
                uint32_t w = in[x], one;
                enum a64x_class c = a64x_classify(w);
                bool stub_class = c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP ||
                                  c == A64X_JSCVT || c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM ||
                                  (c == A64X_PAC_LDRA && !a64x_rewrite1(w, &one));
                if (!stub_class)
                    continue;
                if (nsites == capsites) {
                    capsites = capsites ? capsites * 2 : 1 << 20;
                    sites = realloc(sites, capsites * sizeof(*sites));
                }
                /* offline: look up to 64 KB ahead (whole functions), not just the page */
                uint64_t k0 = pg + x, avail = n - k0;
                int wn = (int)(avail < 16384 ? avail : 16384);
                const uint32_t *wp = (const uint32_t *)(t + 4 * k0);
                /* the site's own register operands are never scratch; the rest are tested one by one */
                uint32_t cand = (1u << 16) | (1u << 17) | (1u << 30) | (0x7Fu << 9) | 0x1FFu | (0x3FFu << 19);
                uint32_t lr;
                uint64_t fend, fstart = function_of(base + 4 * k0, &fend);
                int q2 = fstart ? map_of(fstart) : -1;
                if (q2 >= 0 && fend > fstart && fend <= fm[q2].addr + fm[q2].size && fend - fstart <= 4u * 65536) {
                    /* the whole function: loops back to before the site and tail calls are understood */
                    const uint32_t *fp = (const uint32_t *)vmptr(fstart, fend - fstart);
                    lr = a64x_dead_regs_flow_fn(fp, (int)((fend - fstart) / 4), (int)((base + 4 * k0 - fstart) / 4),
                                                cand, wide_budget, &walk);
                    if (cmp_out) {
                        static uint64_t nx30;
                        uint32_t lw = a64x_dead_regs_flow(wp, wn, 0, cand, wide_budget, &walk);
                        if (lw & ~lr)
                            fprintf(cmp_out, "LOST %llx %08x win %08x fn %08x\n", (unsigned long long)(base + 4 * k0),
                                    w, lw, lr);
                        if ((lr >> 30) & ~(lw >> 30) & 1) {
                            if (nx30++ % 5000 == 0)
                                fprintf(cmp_out, "X30 %llx %08x fn %llx..%llx\n", (unsigned long long)(base + 4 * k0),
                                        w, (unsigned long long)fstart, (unsigned long long)fend);
                        }
                    }
                } else {
                    lr = a64x_dead_regs_flow(wp, wn, 0, cand, wide_budget, &walk);
                }
                uint32_t vd = 0;
                if (a64x_class_uses_vtemps(c)) {
                    if (q2 >= 0 && fend > fstart && fend <= fm[q2].addr + fm[q2].size && fend - fstart <= 4u * 65536) {
                        const uint32_t *fp = (const uint32_t *)vmptr(fstart, fend - fstart);
                        vd = a64x_dead_vregs_flow(fp, (int)((fend - fstart) / 4), (int)((base + 4 * k0 - fstart) / 4),
                                                  0xFFFFFFFFu, wide_budget, true, &walk);
                    } else {
                        vd = a64x_dead_vregs_flow(wp, wn, 0, 0xFFFFFFFFu, wide_budget, false, &walk);
                    }
                    if (getenv("A64X_NO_VLIVE"))
                        vd = 0;
                    static FILE *vdump;
                    static uint64_t vn;
                    if (!vdump && getenv("A64X_VDUMP"))
                        vdump = fopen(getenv("A64X_VDUMP"), "w");
                    if (vdump && vn++ % 9001 == 0)
                        fprintf(vdump, "%llx %08x vdead %08x\n", (unsigned long long)(base + 4 * k0), w, vd);
                }
                sites[nsites++] = (struct site){base + 4 * (pg + x),
                                                w,
                                                lr & ~(1u << 30),
                                                vd,
                                                (uint8_t)c,
                                                (uint8_t)((lr >> 30) & 1),
                                                (uint8_t)((hotbits[f][(fo / 4) >> 3] >> ((fo / 4) & 7)) & 1)};
            }
        }
    }
    free(walk_buf);
    qsort(holes, nholes, sizeof(*holes), cmp_hole);
    {
        size_t dup = 0, o = 0;
        for (size_t i = 0; i < nholes; i++) {
            if (o && holes[i].va < holes[o - 1].va + 4u * holes[o - 1].size) {
                dup++;
                continue;
            }
            holes[o++] = holes[i];
        }
        if (dup)
            printf("dropped %zu overlapping hole records\n", dup);
        nholes = o;
    }
    dsu_build(&d3, 3);
    dsu_build(&d5, 5);
    bin_base = holes[0].va & ~((1ull << BIN_SHIFT) - 1);
    nbins = (int64_t)((holes[nholes - 1].va - bin_base) >> BIN_SHIFT) + 1;
    bin_room = calloc((size_t)nbins, sizeof(int64_t));
    bin_first = calloc((size_t)nbins, sizeof(int64_t));
    bin_end = calloc((size_t)nbins, sizeof(int64_t));
    for (int64_t b = 0; b < nbins; b++)
        bin_first[b] = -1;
    for (size_t i = 0; i < nholes; i++) {
        int64_t b = (int64_t)((holes[i].va - bin_base) >> BIN_SHIFT);
        if (bin_first[b] < 0)
            bin_first[b] = (int64_t)i;
        bin_end[b] = (int64_t)i + 1;
        bin_room[b] += holes[i].size - 1;
    }
    for (int64_t b = 0; b < nbins; b++)
        if (bin_first[b] < 0)
            bin_first[b] = 0;
    uint64_t hw = 0;
    for (size_t i = 0; i < nholes; i++)
        hw += holes[i].size;
    printf("holes: %" PRIu64 " x3 + %" PRIu64 " x5 = %" PRIu64 " words; sites %zu\n", n3, n5, hw, nsites);

    /* placement: hot first, then the rest */
    uint64_t by_shared = 0, by_single = 0, by_pieces = 0, by_hop = 0, failed = 0, hot_sites = 0, hot_ok = 0,
             lr_dead = 0;
    uint64_t spill_sites = 0, extra_branches = 0, failed_cls[A64X_NCLASSES] = {0};
    /*
	 * Order: phase 0 = shared BL stubs (x30 dead), hot libraries first; phase 1 = per-site stubs, hot first, then
	 * the densest neighbourhoods first (so a dense image like WebCore gets the holes around it before its
	 * neighbours, which can reach holes farther away).
	 */
    uint8_t *done = calloc(nsites, 1);
    size_t *ord = malloc(nsites * sizeof(size_t));
    double *dens = calloc(nsites, sizeof(double));
    {
        int64_t *cnt = calloc((size_t)nbins + 1, sizeof(int64_t));
        for (size_t i = 0; i < nsites; i++)
            if (!sites[i].lr_dead && sites[i].va >= bin_base) {
                int64_t b = (int64_t)((sites[i].va - bin_base) >> BIN_SHIFT);
                if (b < nbins)
                    cnt[b + 1]++;
            }
        for (int64_t b = 1; b <= nbins; b++)
            cnt[b] += cnt[b - 1];
        for (size_t i = 0; i < nsites; i++) {
            int64_t b = sites[i].va >= bin_base ? (int64_t)((sites[i].va - bin_base) >> BIN_SHIFT) : 0;
            int64_t lo = b - 64 < 0 ? 0 : b - 64, hi = b + 65 > nbins ? nbins : b + 65;
            dens[i] = (double)(cnt[hi] - cnt[lo]);
        }
        free(cnt);
    }
    for (int phase = 0; phase < 2; phase++) {
        size_t no = 0;
        for (int hot = 1; hot >= 0; hot--) {
            size_t start = no;
            for (size_t i = 0; i < nsites; i++)
                if (!done[i] && sites[i].hot == hot && (phase || sites[i].lr_dead))
                    ord[no++] = i;
            if (phase == 1 && !hot) {
                /* densest first: simple insertion into buckets by density */
                size_t m2 = no - start;
                size_t *tmp = malloc(m2 * sizeof(size_t));
                memcpy(tmp, ord + start, m2 * sizeof(size_t));
                int cmpd(const void *a, const void *b2);
                extern double *g_dens;
                g_dens = dens;
                qsort(tmp, m2, sizeof(size_t), cmpd);
                memcpy(ord + start, tmp, m2 * sizeof(size_t));
                free(tmp);
            }
        }
        for (size_t oi = 0; oi < no; oi++) {
            size_t i = ord[oi];
            struct site *s = &sites[i];
            if (phase == 0) {
                if (s->hot)
                    hot_sites++;
                lr_dead += s->lr_dead;
            }
            if (phase == 1 && s->hot && !s->lr_dead)
                hot_sites++;
            int pieces = 0;
            bool ok = false;
            ctx_site = s->va;
            ctx_tag = phase ? "per-site" : "shared";
            bool two_free = (s->dead & ((1u << 16) | (1u << 17))) == ((1u << 16) | (1u << 17));
            /* 1. shared BL stub */
            if (phase == 0 && !no_shared && s->lr_dead && s->cls != A64X_PAC_LDRA) {
                uint32_t vt = a64x_class_uses_vtemps(s->cls) ? vtemps_of(s->w) : 0;
                bool vfree = vt && (s->vdead & vt) == vt;
                uint64_t va = shared_stub(s->w, (two_free ? 1 : 0) | (vfree ? 2 : 0), s->va, &pieces);
                if (va && a64x_class_uses_vtemps(s->cls)) {
                    uint32_t st0[A64X_STUB_MAX], st1[A64X_STUB_MAX];
                    int n0 = a64x_stub_v(s->w, s->va, s->va + 8, two_free ? (1u << 16) | (1u << 17) : 0, 0, st0,
                                         A64X_STUB_MAX);
                    int n1 = a64x_stub_v(s->w, s->va, s->va + 8, two_free ? (1u << 16) | (1u << 17) : 0,
                                         vfree ? 0xFFFF0000u : 0, st1, A64X_STUB_MAX);
                    fp_sites++;
                    fp_words_before += (uint64_t)n0;
                    fp_words_after += (uint64_t)n1;
                    fp_vspill_before += has_vspill(st0, n0);
                    fp_vspill_after += has_vspill(st1, n1);
                }
                if (va) {
                    add_patch(s->va, enc_bl(s->va, va));
                    ok = true;
                    by_shared++;
                    if (!two_free)
                        spill_sites++;
                    extra_branches += 2 + (pieces > 1 ? (uint64_t)(pieces - 1) : 0);
                }
            }
            if (phase == 0 && !ok) {
                continue; /* left for phase 1 */
            }
            /* 2./3. per-site stub */
            if (!ok) {
                uint32_t st[A64X_STUB_MAX];
                uint64_t dummy = s->va + 8;
                int n = a64x_stub_v(s->w, s->va, dummy, s->dead, s->vdead, st, A64X_STUB_MAX);
                if (n > 0 && n <= A64X_STUB_MAX)
                    size_hist[n]++;
                if (n > 0 && a64x_class_uses_vtemps(s->cls)) {
                    uint32_t st0[A64X_STUB_MAX];
                    int n0 = a64x_stub_v(s->w, s->va, dummy, s->dead, 0, st0, A64X_STUB_MAX);
                    fp_sites++;
                    fp_words_before += (uint64_t)n0;
                    fp_words_after += (uint64_t)n;
                    fp_vspill_before += has_vspill(st0, n0);
                    fp_vspill_after += has_vspill(st, n);
                }
                if (n > 0 && n <= A64X_STUB_MAX) {
                    uint64_t va = place(st, n, dummy, s->va, 120u << 20, &pieces);
                    if (va) {
                        add_patch(s->va, a64x_branch(s->va, va));
                        ok = true;
                        if (pieces == 1)
                            by_single++;
                        else
                            by_pieces++;
                        if ((st[0] & 0xFFC003FFu) == 0xD10003FFu)
                            spill_sites++;
                        extra_branches += 2 + (uint64_t)(pieces - 1);
                    }
                }
            }
            ctx_tag = "chain";
            if (!ok && !no_hops && place_chain(s->w, s->dead, s->va)) {
                ok = true;
                by_hop++;
                extra_branches += 2 + 2 * (chain_hops ? 1 : 0);
            }
            done[i] = 1;
            if (ok && s->hot)
                hot_ok++;
            if (!ok) {
                failed++;
                failed_cls[s->cls]++;
                if (unplaced_path) {
                    static FILE *uf;
                    if (!uf)
                        uf = fopen(unplaced_path, "w");
                    fprintf(uf, "%llx %08x %u\n", (unsigned long long)s->va, s->w, s->lr_dead);
                }
            }
        }
    }
    uint64_t placed = by_shared + by_single + by_pieces + by_hop;
    if (fp_sites)
        printf("FP stubs (V temporaries): %llu sites; Q spills at %llu sites without V liveness, %llu with it; mean "
               "stub %.1f -> %.1f words\n",
               (unsigned long long)fp_sites, (unsigned long long)fp_vspill_before, (unsigned long long)fp_vspill_after,
               (double)fp_words_before / fp_sites, (double)fp_words_after / fp_sites);
    printf("sites %zu: placed %" PRIu64 " (%.2f%%): shared BL %" PRIu64 ", one hole %" PRIu64 ", pieces %" PRIu64
           " (mean %.2f holes), veneer %" PRIu64 "; trap %" PRIu64 "\n",
           nsites, placed, 100.0 * placed / nsites, by_shared, by_single, by_pieces,
           stat_pieces_sites ? (double)stat_pieces_total / stat_pieces_sites : 0.0, by_hop, failed);
    printf("hot libraries: %" PRIu64 " / %" PRIu64 " placed; x30 dead at %" PRIu64 " sites; shared copies %" PRIu64
           " (%" PRIu64 " words)\n",
           hot_ok, hot_sites, lr_dead, sh_copies, sh_words);
    printf("veneer chains: %" PRIu64 " sites, %.2f hops each\n", chain_sites,
           chain_sites ? (double)chain_hops / chain_sites : 0.0);
    printf("cost: %.2f taken branches per placed site on average (inline LL/SC = 0); spilling at %" PRIu64 " sites\n",
           placed ? (double)extra_branches / placed : 0.0, spill_sites);
    for (int c = 1; c < A64X_NCLASSES; c++)
        if (failed_cls[c])
            printf("  trap: %-10s %" PRIu64 "\n", a64x_class_name(c), failed_cls[c]);
    printf("per-site stub sizes:");
    for (int k = 1; k <= A64X_STUB_MAX; k++)
        if (size_hist[k])
            printf(" %d:%" PRIu64, k, size_hist[k]);
    printf("\n");

    if (outp)
        write_plan(outp, m);
    else
        qsort(patches, npatch, sizeof(*patches), cmp_patch);
    {
        uint64_t dups = 0;
        for (size_t i = 1; i < npatch; i++)
            if (patches[i].va == patches[i - 1].va) {
                if (dups < 4)
                    fprintf(stderr, "duplicate plan word at %llx: %08x / %08x\n", (unsigned long long)patches[i].va,
                            patches[i - 1].w, patches[i].w);
                dups++;
            }
        printf("duplicate plan addresses: %" PRIu64 "\n", dups);
    }

    /* verification: simulate the kernel transform; follow every translated site */
    uint64_t ok = 0, bad = 0, changed = 0, pages = 0, vcls[A64X_NCLASSES] = {0};
    static uint8_t seen[1u << 29];
    FILE *d = dump ? fopen(dump, "w") : NULL;
    for (int q = 0; q < nfm; q++) {
        if (!(fm[q].prot & 4))
            continue;
        const uint8_t *t = fb[fm[q].file] + fm[q].off;
        uint64_t n = fm[q].size / 4, base = fm[q].addr;
        for (uint64_t pg = 0; pg < n; pg += 1024) {
            uint32_t in[1024], out[1024];
            uint32_t nw = pg + 1024 <= n ? 1024 : (uint32_t)(n - pg);
            for (uint32_t x = 0; x < 1024; x++)
                in[x] = x < nw ? u32(t, (pg + x) * 4) : 0;
            uint64_t va = base + 4 * pg;
            a64x_translate_page(in, out, nw, va, &plan_code_map, NULL, 0, NULL);
            a64x_dead_checks(in, out, nw, va, &plan_code_map, NULL, NULL);
            for (uint32_t x = 0; x < nw; x++) {
                bool f;
                uint32_t w = patch_at(va + 4 * x, &f);
                if (f)
                    out[x] = w;
            }
            pages++;
            for (uint32_t x = 0; x < nw; x++) {
                if (!plan_is_code(NULL, va + 4 * x))
                    continue;
                if (out[x] != in[x])
                    changed++;
                if (d && !(seen[out[x] >> 3] & (1 << (out[x] & 7)))) {
                    seen[out[x] >> 3] |= 1 << (out[x] & 7);
                    fprintf(d, "%08x\n", out[x]);
                }
                enum a64x_class c = a64x_classify(in[x]);
                if ((c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP ||
                     c == A64X_JSCVT || c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM) &&
                    out[x] != in[x]) {
                    vcls[c]++;
                    if (follow(va + 4 * x, out[x]))
                        ok++;
                    else {
                        bad++;
                        if (bad < 3) {
                            fprintf(stderr, "broken site %llx word %08x out %08x\n", (unsigned long long)(va + 4 * x),
                                    in[x], out[x]);
                            uint64_t pc = va + 4 * x + ((int64_t)(int32_t)((out[x] & 0x3FFFFFFu) << 6) >> 4);
                            for (int stp = 0; stp < 24; stp++) {
                                bool f2;
                                uint32_t ww = patch_at(pc, &f2);
                                fprintf(stderr, "   %llx %s %08x\n", (unsigned long long)pc, f2 ? "plan" : "ORIG",
                                        f2 ? ww : word_at(pc));
                                if (!f2)
                                    break;
                                if ((ww & 0xFC000000u) == 0x14000000u)
                                    pc += ((int64_t)(int32_t)((ww & 0x3FFFFFFu) << 6) >> 4);
                                else
                                    pc += 4;
                            }
                        }
                    }
                }
            }
        }
    }
    if (d)
        fclose(d);
    printf("verify: %" PRIu64 " pages, %" PRIu64
           " code words changed; stub sites with every path reaching their return %" PRIu64 ", broken %" PRIu64 " (",
           pages, changed, ok, bad);
    for (int c = 1; c < A64X_NCLASSES; c++)
        if (vcls[c])
            printf(" %s %" PRIu64, a64x_class_name(c), vcls[c]);
    printf(" )\n");
    (void)word_at;
    return bad ? 1 : 0;
}
