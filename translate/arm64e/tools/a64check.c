/*
 * a64check: what the a64x loader does with a Mach-O, decided exactly as bsd/kern/a64x_xnu.c decides it.
 *
 *   arm64e                         -> full translation (PAC identity, LSE, FP16, ...)
 *   arm64 (ALL/V8), no marker      -> scanned; only what the A57 lacks is translated (no PAC rewrites);
 *                                     nothing found -> mapped as is (byte-identical)
 *   section __TEXT,__a64x_target   -> mapped as is, never scanned (built for this core)
 *
 * It runs the same a64x_translate_page() over the executable segment (4 KB pages, the kernel's stub layout) and
 * reports the words that change, per class. -a: translate even a marked image (to verify its promise).
 *
 * usage: a64check [-a] FILE        (thin or fat; every arm64/arm64e slice)
 */
#include "../a64x/a64x.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CPU_TYPE_ARM64_ 0x0100000Cu

struct range {
    uint64_t lo, hi;
};
static struct range ranges[256];
static int nranges;

static uint8_t *island; /* data islands of the current image (a64x_islands_*), by word of its segment */
static uint64_t island_base, island_words_total;

static bool is_code(void *ctx, uint64_t off)
{
    (void)ctx;
    if (island && off >= island_base && (island[(off - island_base) / 32] & (1u << (((off - island_base) / 4) & 7))))
        return false;
    for (int i = 0; i < nranges; i++)
        if (off >= ranges[i].lo && off < ranges[i].hi)
            return true;
    return false;
}

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

static bool quiet;
static const char *cur_name = "", *list_match;
static uint64_t fs_tot[A64X_NCLASSES][3], fs_stub_words, fs_changed;

static int check_image(const uint8_t *base, uint64_t mh_off, uint64_t slice_off, bool all, bool kernel)
{
    const uint8_t *mh = base + mh_off;
    uint32_t cputype = rd32(mh + 4), subtype = rd32(mh + 8) & 0x00FFFFFFu, ncmds = rd32(mh + 16);
    if (rd32(mh) != 0xFEEDFACFu || cputype != CPU_TYPE_ARM64_) {
        printf("  slice at 0x%llx: not arm64 (cputype 0x%x): native\n", (unsigned long long)slice_off, cputype);
        return 0;
    }
    const char *kind = subtype == 2 ? "arm64e" : (subtype == 0 || subtype == 1) ? "arm64" : "other";
    bool marked = false;
    uint64_t exec_off = 0, exec_size = 0;
    int nexec = 0;
    nranges = 0;
    const uint8_t *lc = mh + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc), size = rd32(lc + 4);
        if (cmd == 0x19) {
            uint32_t nsects = rd32(lc + 64), initprot = rd32(lc + 60);
            const uint8_t *sc = lc + 72;
            for (uint32_t s = 0; s < nsects; s++, sc += 80) {
                if (!strncmp((const char *)sc + 16, "__TEXT", 16) && !strncmp((const char *)sc, "__a64x_target", 16)) {
                    marked = true;
                    printf("  marker __TEXT,__a64x_target: \"%.*s\"\n",
                           (int)strnlen((const char *)(base + slice_off + rd32(sc + 48)), rd64(sc + 40)),
                           (const char *)(base + slice_off + rd32(sc + 48)));
                }
            }
            if (initprot & 4) {
                nexec++;
                exec_off = slice_off + rd64(lc + 40);
                exec_size = rd64(lc + 48);
                sc = lc + 72;
                for (uint32_t s = 0; s < nsects; s++, sc += 80) {
                    if ((rd32(sc + 64) & 0x80000400u) && rd64(sc + 40) && nranges < 256) {
                        ranges[nranges].lo = slice_off + rd32(sc + 48);
                        ranges[nranges].hi = ranges[nranges].lo + rd64(sc + 40);
                        nranges++;
                    }
                }
            }
        }
        lc += size;
    }
    if (!quiet)
        printf("  slice at 0x%llx: %s (subtype %u), %s, executable segment 0x%llx bytes, %d instruction sections\n",
               (unsigned long long)mh_off, kind, subtype, marked ? "MARKED for this core" : "not marked",
               (unsigned long long)exec_size, nranges);
    if (subtype > 2) {
        puts("  loader: native (unknown arm64 subtype)");
        return 0;
    }
    if (marked && !all) {
        puts("  loader: mapped as is (marked); not scanned. Use -a to verify the promise.");
        return 0;
    }
    if (nexec != 1) {
        puts("  loader: cannot lay out (not one executable segment)");
        return 1;
    }
    struct a64x_code_map cm = {NULL, is_code, kernel ? A64X_MAP_KERNEL : subtype == 2 ? 0 : A64X_MAP_NO_PAC, NULL};
    /* data islands exactly as the kernel finds them for an image without a plan (a64x_find_islands) */
    uint64_t nw_seg = (exec_size + 4095) / 4096 * 1024;
    uint8_t *fsb = calloc(nw_seg / 8 + 1, 1);
    island = calloc(nw_seg / 8 + 1, 1);
    island_base = exec_off;
    {
        const uint8_t *lc2 = mh + 32;
        uint64_t text_vm = 0, exec_vm = 0;
        uint32_t fs_off = 0, fs_size = 0;
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc2);
            if (cmd == 0x19 && !strncmp((const char *)lc2 + 8, "__TEXT", 16))
                text_vm = rd64(lc2 + 24);
            if (cmd == 0x19 && (rd32(lc2 + 60) & 4))
                exec_vm = rd64(lc2 + 24);
            if (cmd == 0x26) {
                fs_off = rd32(lc2 + 8);
                fs_size = rd32(lc2 + 12);
            }
            lc2 += rd32(lc2 + 4);
        }
        const uint8_t *d = base + slice_off + fs_off;
        uint64_t a = text_vm;
        for (uint32_t q = 0; fs_size && q < fs_size;) {
            uint64_t v = 0;
            int sh = 0;
            while (q < fs_size) {
                uint8_t b = d[q++];
                v |= (uint64_t)(b & 0x7F) << sh;
                sh += 7;
                if (!(b & 0x80))
                    break;
            }
            if (!v)
                break;
            a += v;
            if (a >= exec_vm && (a - exec_vm) / 4 < nw_seg)
                fsb[(a - exec_vm) / 32] |= (uint8_t)(1u << (((a - exec_vm) / 4) & 7));
        }
        uint8_t *keep = island;
        island = NULL;
        struct a64x_islands is = {keep, fsb, 0, 0, false, false, 0, 0, 0, 0};
        for (uint64_t off = exec_off; off < exec_off + exec_size; off += 4096) {
            uint32_t in[1024] = {0};
            uint64_t n = exec_off + exec_size - off < 4096 ? exec_off + exec_size - off : 4096;
            memcpy(in, base + off, n);
            a64x_islands_page(&is, in, 1024, (off - exec_off) / 4, off, &cm);
        }
        a64x_islands_end(&is, nw_seg);
        island = keep;
        island_words_total += is.words;
    }
    free(fsb);
    uint32_t cap = 1u << 24, *stubs = malloc(cap * 4), cursor = 0;
    void *walk_buf = malloc(A64X_WALK_BYTES(1024));
    struct a64x_walk walk = a64x_walk_in(walk_buf, 1024);
    uint64_t changed = 0, words = 0;
    struct a64x_stats st;
    memset(&st, 0, sizeof(st));
    for (uint64_t off = exec_off; off < exec_off + exec_size; off += 4096) {
        uint32_t in[1024] = {0}, out[1024];
        uint64_t n = exec_off + exec_size - off < 4096 ? exec_off + exec_size - off : 4096;
        memcpy(in, base + off, n);
        struct a64x_stub_region sr = {exec_off + exec_size + 0x4000, stubs, cap, 0, &walk};
        cursor = a64x_translate_page(in, out, 1024, off, &cm, &sr, cursor, &st);
        for (int i = 0; i < 1024; i++)
            changed += in[i] != out[i];
        if (list_match && strstr(cur_name, list_match)) {
            for (uint64_t i = 0; i < n / 4; i++) {
                enum a64x_class c = a64x_classify(in[i]);
                if (c != A64X_OK && in[i] == out[i] && is_code(NULL, off + 4 * i))
                    printf("    LEFT %llx %08x %s\n", (unsigned long long)(off + 4 * i), in[i], a64x_class_name(c));
            }
        }
        words += n / 4;
    }
    free(walk_buf);
    fs_stub_words += cursor;
    fs_changed += changed;
    for (int c = 1; c < A64X_NCLASSES; c++) {
        fs_tot[c][0] += st.rewrite1[c];
        fs_tot[c][1] += st.stubs[c];
        fs_tot[c][2] += st.left[c];
    }
    if (quiet) {
        if (island) {
            free(island);
            island = NULL;
        }
        uint64_t left = 0, lse = 0;
        for (int c = 1; c < A64X_NCLASSES; c++)
            left += st.left[c];
        lse = st.stubs[A64X_LSE_CAS] + st.stubs[A64X_LSE_CASP] + st.stubs[A64X_LSE_LDOP] + st.stubs[A64X_LSE_SWP];
        printf(" %-7s %8llu words, %7llu changed, %5llu LSE stubs, %6u stub words, %llu left", kind,
               (unsigned long long)words, (unsigned long long)changed, (unsigned long long)lse, cursor,
               (unsigned long long)left);
        for (int c = 1; c < A64X_NCLASSES; c++)
            if (st.left[c])
                printf(" %s:%llu", a64x_class_name(c), (unsigned long long)st.left[c]);
        printf("\n");
        free(stubs);
        return 0;
    }
    if (island) {
        free(island);
        island = NULL;
    }
    printf("  translated as %s: %llu of %llu words change, %u stub words\n",
           subtype == 2 ? "arm64e (full)" : "arm64 (no PAC rewrites)", (unsigned long long)changed,
           (unsigned long long)words, cursor);
    for (int c = 1; c < A64X_NCLASSES; c++) {
        if (st.rewrite1[c] || st.stubs[c] || st.left[c])
            printf("    %-14s rewritten %llu, stubs %llu, left %llu\n", a64x_class_name(c),
                   (unsigned long long)st.rewrite1[c], (unsigned long long)st.stubs[c], (unsigned long long)st.left[c]);
    }
    if (marked)
        printf("  loader: mapped as is (marked); the promise %s\n",
               changed ? "is BROKEN (see above)" : "holds: nothing to translate");
    else if (subtype != 2 && !changed)
        puts("  loader: nothing the A57 lacks: mapped as is, byte-identical, no pager");
    else
        puts("  loader: executable segment through the translating pager");
    free(stubs);
    return 0;
}

int a64x_cmd_check(int argc, char **argv)
{
    bool all = argc > 2 && !strcmp(argv[1], "-a");
    list_match = getenv("A64CHECK_LIST");
    const char *path = argv[argc - 1];
    int fd = open(path, O_RDONLY);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb)) {
        perror(path);
        return 1;
    }
    const uint8_t *b = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    printf("%s\n", path);
    uint32_t magic = rd32(b);
    if (magic == 0xBEBAFECAu) {
        uint32_t n = __builtin_bswap32(rd32(b + 4));
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = b + 8 + 20 * i;
            uint64_t off = __builtin_bswap32(rd32(a + 8));
            check_image(b, off, off, all, false);
        }
        return 0;
    }
    if (rd32(b + 12) == 0xC) {
        /*
		 * MH_FILESET kernelcache: every entry translated in kernel mode (PAC identity included: arm64e kexts on our
		 * arm64 kernel), stubs laid out after each entry's text as the prelinker would place them.
		 */
        uint32_t ncmds = rd32(b + 16);
        const uint8_t *lc = b + 32;
        quiet = true;
        for (uint32_t i = 0; i < ncmds; i++) {
            if (rd32(lc) == 0x80000035u) {
                uint64_t off = rd64(lc + 16);
                const char *id = (const char *)(lc + rd32(lc + 24));
                char name[61];
                snprintf(name, sizeof(name), "%s", id);
                /* name goes where the kind is printed */
                const uint8_t *emh = b + off;
                (void)emh;
                printf("%-62s", name);
                cur_name = id;
                check_image(b, off, 0, true, true);
            }
            lc += rd32(lc + 4);
        }
        printf("fileset total: %llu words changed, %llu stub words; %llu words of __text left alone as data islands\n",
               (unsigned long long)fs_changed, (unsigned long long)fs_stub_words,
               (unsigned long long)island_words_total);
        for (int c = 1; c < A64X_NCLASSES; c++)
            if (fs_tot[c][0] || fs_tot[c][1] || fs_tot[c][2])
                printf("  %-14s rewritten %llu, stubs %llu, left %llu\n", a64x_class_name(c),
                       (unsigned long long)fs_tot[c][0], (unsigned long long)fs_tot[c][1],
                       (unsigned long long)fs_tot[c][2]);
        return 0;
    }
    return check_image(b, 0, 0, all, false);
}
