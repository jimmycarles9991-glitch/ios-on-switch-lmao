/*
 * a64xlate: translate standalone arm64e Mach-O images the way the kernel does at load time (page by page, stubs in
 * a region right after the image), and check the result.
 *
 *   a64xlate [-p pagesize] [-d distinct_out.txt] FILE...
 *
 * Checks: every site branch reaches its stub, every stub's last word returns to site+4, no stub is out of range.
 * -d writes every distinct word of the translated code and of the stub regions (for tools/oracle.py: each must decode
 * on cortex-a57, or be a word a64x left on purpose, or be data).
 */
#include "../a64x/a64x.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fts.h>

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

struct range {
    uint64_t a, b;
};
static struct range code[4096];
static int ncode;

static bool is_code(void *ctx, uint64_t va)
{
    (void)ctx;
    for (int i = 0; i < ncode; i++)
        if (va >= code[i].a && va < code[i].b)
            return true;
    return false;
}

static struct a64x_stats total;
static uint32_t pagesize = 4096;
static FILE *fdist;
static uint64_t g_bad, g_files, g_words;

/* distinct-word set for -d */
static uint32_t *dset;
static size_t dcap = 1 << 22, dlen;

static void dadd(uint32_t w)
{
    if (!fdist)
        return;
    if (dlen * 2 >= dcap) {
        size_t nc = dcap * 2;
        uint32_t *nt = calloc(nc, 4);
        for (size_t i = 0; i < dcap; i++)
            if (dset[i]) {
                size_t h = (dset[i] * 2654435761u) & (nc - 1);
                while (nt[h])
                    h = (h + 1) & (nc - 1);
                nt[h] = dset[i];
            }
        free(dset);
        dset = nt;
        dcap = nc;
    }
    if (!w)
        return;
    size_t h = (w * 2654435761u) & (dcap - 1);
    while (dset[h] && dset[h] != w)
        h = (h + 1) & (dcap - 1);
    if (!dset[h]) {
        dset[h] = w;
        dlen++;
    }
}

static void slice(const char *name, const uint8_t *mh, uint64_t len)
{
    if (u32(mh, 0) != 0xFEEDFACF)
        return;
    uint32_t ncmds = u32(mh, 16);
    ncode = 0;
    uint64_t vmend = 0;

    struct {
        uint64_t vm, vmsize, off, fsize;
        uint32_t prot;
    } segs[64];

    int nseg = 0;
    const uint8_t *lc = mh + 32;
    for (uint32_t c = 0; c < ncmds; c++) {
        if (u32(lc, 0) == 0x19) {
            uint64_t vm = u64(lc, 24), vs = u64(lc, 32), fo = u64(lc, 40), fs = u64(lc, 48);
            uint32_t prot = u32(lc, 60);
            if (!memcmp(lc + 8, "__PAGEZERO", 10)) {
                lc += u32(lc, 4);
                continue;
            }
            if (vm + vs > vmend)
                vmend = vm + vs;
            if (nseg < 64) {
                segs[nseg].vm = vm;
                segs[nseg].vmsize = vs;
                segs[nseg].off = fo;
                segs[nseg].fsize = fs;
                segs[nseg].prot = prot;
                nseg++;
            }
            uint32_t ns = u32(lc, 64);
            for (uint32_t s = 0; s < ns; s++) {
                const uint8_t *sp = lc + 72 + 80 * s;
                if ((u32(sp, 64) & 0x80000400u) && u64(sp, 40) && ncode < 4096) {
                    code[ncode].a = u64(sp, 32);
                    code[ncode].b = u64(sp, 32) + u64(sp, 40);
                    ncode++;
                }
            }
        }
        lc += u32(lc, 4);
    }
    struct a64x_code_map cm = {NULL, is_code, 0, NULL};
    uint64_t stub_va = (vmend + 0x3FFF) & ~0x3FFFull;
    /* dylibs extracted from a dyld cache keep cache addresses (LINKEDIT GBs away): put the region after the text */
    uint64_t xend = 0;
    for (int s = 0; s < nseg; s++)
        if ((segs[s].prot & 4) && segs[s].vm + segs[s].vmsize > xend)
            xend = segs[s].vm + segs[s].vmsize;
    if (stub_va - xend > (64ull << 20))
        stub_va = (xend + 0x3FFF) & ~0x3FFFull;
    struct a64x_stats st = {0};
    /* pass 1: layout; pass 2: generate */
    uint32_t cursor = 0;
    uint32_t *in = malloc(pagesize), *out = malloc(pagesize);
    void *walk_buf = malloc(A64X_WALK_BYTES(pagesize / 4));
    struct a64x_walk walk = a64x_walk_in(walk_buf, pagesize / 4);
    struct a64x_stub_region sr = {stub_va, NULL, 1u << 30, 0, &walk};
    for (int pass = 0; pass < 2; pass++) {
        cursor = 0;
        memset(&st, 0, sizeof(st));
        if (pass == 1) {
            sr.words = calloc(sr.cap_words, 4);
        }
        for (int s = 0; s < nseg; s++) {
            if (!(segs[s].prot & 4))
                continue;
            for (uint64_t pg = segs[s].vm & ~(uint64_t)(pagesize - 1); pg < segs[s].vm + segs[s].fsize;
                 pg += pagesize) {
                memset(in, 0, pagesize);
                uint64_t fo = segs[s].off + (pg - segs[s].vm);
                uint64_t n = pagesize;
                if (fo + n > len)
                    n = len > fo ? len - fo : 0;
                memcpy(in, mh + fo, n);
                cursor = a64x_translate_page(in, out, pagesize / 4, pg, &cm, &sr, cursor, pass ? &st : NULL);
                if (pass == 1) {
                    for (uint32_t i = 0; i < pagesize / 4; i++) {
                        uint64_t va = pg + 4ull * i;
                        if (!is_code(NULL, va))
                            continue;
                        g_words++;
                        dadd(out[i]);
                        if (out[i] != in[i] && (out[i] & 0xFC000000u) == 0x14000000u) {
                            int64_t d = (int64_t)(int32_t)((out[i] & 0x3FFFFFFu) << 6) >> 4;
                            uint64_t t = va + d;
                            if (t < stub_va || t >= stub_va + 4ull * cursor) {
                                g_bad++;
                                printf("BAD site %llx -> %llx\\n", va, t);
                                continue;
                            }
                            /* walk the stub to its final B: it must return to va + 4 */
                            uint64_t q = t;
                            int k = 0;
                            bool okret = false;
                            for (; k < A64X_STUB_MAX; k++, q += 4) {
                                uint32_t sw = sr.words[(q - stub_va) / 4];
                                if ((sw & 0xFC000000u) == 0x14000000u) {
                                    int64_t d2 = (int64_t)(int32_t)((sw & 0x3FFFFFFu) << 6) >> 4;
                                    if (q + d2 == va + 4) {
                                        okret = true;
                                        break;
                                    }
                                }
                            }
                            if (!okret) {
                                g_bad++;
                                printf("BAD stub for site %llx\n", va);
                            }
                        }
                    }
                }
            }
        }
        if (pass == 0) {
            sr.cap_words = cursor;
        }
    }
    for (uint32_t i = 0; i < cursor; i++)
        dadd(sr.words[i]);
    free(sr.words);
    free(walk_buf);
    free(in);
    free(out);
    uint64_t r1 = 0, sb = 0, lf = 0;
    for (int c = 0; c < A64X_NCLASSES; c++) {
        r1 += st.rewrite1[c];
        sb += st.stubs[c];
        lf += st.left[c];
        total.rewrite1[c] += st.rewrite1[c];
        total.stubs[c] += st.stubs[c];
        total.left[c] += st.left[c];
    }
    total.stub_words += st.stub_words;
    total.stubs_spill += st.stubs_spill;
    printf("%-48s 1:1=%-8" PRIu64 " stubs=%-6" PRIu64 " (spill %" PRIu64 ") stub region %" PRIu64 " KB, left=%" PRIu64,
           strrchr(name, '/') ? strrchr(name, '/') + 1 : name, r1, sb, st.stubs_spill, st.stub_words * 4 / 1024, lf);
    for (int c = 1; c < A64X_NCLASSES; c++)
        if (st.left[c])
            printf(" %s=%" PRIu64, a64x_class_name(c), st.left[c]);
    printf("\n");
    g_files++;
}

static void file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return;
    }
    struct stat sst;
    fstat(fd, &sst);
    if (sst.st_size < 4096) {
        close(fd);
        return;
    }
    const uint8_t *b = mmap(NULL, sst.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    uint32_t magic = u32(b, 0);
    if (magic == 0xBEBAFECA) {
        uint32_t n = __builtin_bswap32(u32(b, 4));
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = b + 8 + 20 * i;
            uint32_t ct = __builtin_bswap32(u32(a, 0)), sub = __builtin_bswap32(u32(a, 4));
            uint32_t off = __builtin_bswap32(u32(a, 8)), len = __builtin_bswap32(u32(a, 12));
            if (ct == 0x0100000C && (sub & 0xFF) == 2)
                slice(path, b + off, len);
        }
    } else if (magic == 0xFEEDFACF && u32(b, 4) == 0x0100000C && (u32(b, 8) & 0xFF) == 2) {
        slice(path, b, (uint64_t)sst.st_size);
    }
    munmap((void *)b, sst.st_size);
}

int a64x_cmd_xlate(int argc, char **argv)
{
    int ch;
    while ((ch = getopt(argc, argv, "p:d:")) != -1) {
        if (ch == 'p')
            pagesize = (uint32_t)atoi(optarg);
        if (ch == 'd') {
            fdist = fopen(optarg, "w");
            dset = calloc(dcap, 4);
        }
    }
    /* files or directories (walked recursively; regular files only) */
    for (int i = optind; i < argc; i++) {
        char *paths[2] = {argv[i], NULL};
        FTS *ft = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
        FTSENT *e;
        while (ft && (e = fts_read(ft))) {
            if (e->fts_info == FTS_F)
                file(e->fts_path);
        }
        if (ft)
            fts_close(ft);
    }
    uint64_t r1 = 0, sb = 0, lf = 0;
    for (int c = 0; c < A64X_NCLASSES; c++) {
        r1 += total.rewrite1[c];
        sb += total.stubs[c];
        lf += total.left[c];
    }
    printf("TOTAL files=%" PRIu64 " code words=%" PRIu64 " 1:1=%" PRIu64 " stubs=%" PRIu64 " (spilling %" PRIu64
           ", mean %.1f words) left=%" PRIu64 " bad=%" PRIu64 "\n",
           g_files, g_words, r1, sb, total.stubs_spill, sb ? (double)total.stub_words / sb : 0.0, lf, g_bad);
    for (int c = 1; c < A64X_NCLASSES; c++)
        if (total.rewrite1[c] || total.stubs[c] || total.left[c])
            printf("  %-16s 1:1 %-9" PRIu64 " stub %-7" PRIu64 " left %" PRIu64 "\n", a64x_class_name(c),
                   total.rewrite1[c], total.stubs[c], total.left[c]);
    if (fdist) {
        for (size_t i = 0; i < dcap; i++)
            if (dset[i])
                fprintf(fdist, "%08x\n", dset[i]);
        fclose(fdist);
    }
    return g_bad ? 1 : 0;
}
