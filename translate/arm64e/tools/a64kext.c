/*
 * a64kext: translate the kexts of a prelinked kernel (kernel mode, PAC as the identity, LSE/FP stubs, corecrypto's
 * MSR DIT/SSBS/PAN), each kext's stubs in its own block of one stub region after the kexts' text.
 *
 *   a64kext BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS
 *
 * BLOB: the kexts' coalesced __TEXT_EXEC bytes as laid out at BLOB_VA (other bytes are not touched).
 * JOBS: per kext "K <id>", then "R <va> <size>" per instruction section, "F <va>" per function start, "E".
 * Prints one line per kext: "S <id> <words> <changed> <stub_first_word> <stub_end_word> <class>=<rw>/<stub>/<left> ..."
 */
#include "../a64x/a64x.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct job {
    char id[160];
    uint64_t r[64][2];
    int nr;
    uint64_t *f;
    size_t nf, capf;
};
static const struct job *cur;
static uint8_t *island;
static uint64_t island_lo, island_words;

static bool is_code(void *ctx, uint64_t va)
{
    (void)ctx;
    if (island && va >= island_lo && (va - island_lo) / 4 < island_words) {
        uint64_t w = (va - island_lo) / 4;
        if (island[w / 8] & (1u << (w & 7)))
            return false;
    }
    for (int i = 0; i < cur->nr; i++)
        if (va >= cur->r[i][0] && va < cur->r[i][0] + cur->r[i][1])
            return true;
    return false;
}

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 8);
    if (fread(b, 1, *n, f) != *n) {
        perror(p);
        exit(1);
    }
    fclose(f);
    return b;
}

int a64x_cmd_kext(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: a64kext BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS\n");
        return 2;
    }
    size_t n;
    uint8_t *blob = slurp(argv[1], &n);
    uint64_t base = strtoull(argv[2], NULL, 0), stubs_va = strtoull(argv[4], NULL, 0);
    uint8_t *orig = malloc(n);
    memcpy(orig, blob, n);
    FILE *jf = fopen(argv[3], "r");
    if (!jf) {
        perror(argv[3]);
        return 1;
    }
    uint32_t cap = 1u << 22, *stubs = calloc(cap, 4), cursor = 0;
    void *walk_buf = malloc(A64X_WALK_BYTES(1024));
    struct a64x_walk walk = a64x_walk_in(walk_buf, 1024);
    char line[512];
    struct job j;
    memset(&j, 0, sizeof(j));
    while (fgets(line, sizeof(line), jf)) {
        if (line[0] == 'K') {
            memset(&j, 0, sizeof(j));
            sscanf(line + 2, "%159s", j.id);
            continue;
        }
        if (line[0] == 'R') {
            sscanf(line + 2, "%" SCNx64 " %" SCNx64, &j.r[j.nr][0], &j.r[j.nr][1]);
            j.nr++;
            continue;
        }
        if (line[0] == 'F') {
            if (j.nf == j.capf) {
                j.capf = j.capf ? 2 * j.capf : 1024;
                j.f = realloc(j.f, j.capf * 8);
            }
            sscanf(line + 2, "%" SCNx64, &j.f[j.nf++]);
            continue;
        }
        if (line[0] != 'E')
            continue;
        cur = &j;
        uint64_t lo = UINT64_MAX, hi = 0;
        for (int i = 0; i < j.nr; i++) {
            if (j.r[i][0] < lo)
                lo = j.r[i][0];
            if (j.r[i][0] + j.r[i][1] > hi)
                hi = j.r[i][0] + j.r[i][1];
        }
        lo &= ~4095ull;
        hi = (hi + 4095) & ~4095ull;
        struct a64x_code_map cm = {NULL, is_code, A64X_MAP_KERNEL, NULL};
        /* data islands, as the kernel finds them for an image without a plan, seeded with the function starts */
        island_lo = lo;
        island_words = (hi - lo) / 4;
        uint8_t *fsb = calloc(island_words / 8 + 1, 1);
        island = NULL;
        uint8_t *isl = calloc(island_words / 8 + 1, 1);
        for (size_t k = 0; k < j.nf; k++)
            if (j.f[k] >= lo && j.f[k] < hi) {
                uint64_t w = (j.f[k] - lo) / 4;
                fsb[w / 8] |= (uint8_t)(1u << (w & 7));
            }
        struct a64x_islands is = {isl, fsb, 0, 0, false, false, 0, 0, 0, 0};
        for (uint64_t va = lo; va < hi; va += 4096) {
            uint32_t in[1024] = {0};
            if (va >= base && va - base < n)
                memcpy(in, orig + (va - base), (va - base) + 4096 <= n ? 4096 : n - (va - base));
            a64x_islands_page(&is, in, 1024, (va - lo) / 4, va, &cm);
        }
        a64x_islands_end(&is, island_words);
        island = isl;
        struct a64x_stats st;
        memset(&st, 0, sizeof(st));
        uint32_t first = cursor;
        uint64_t words = 0, changed = 0;
        for (uint64_t va = lo; va < hi; va += 4096) {
            uint32_t in[1024] = {0}, out[1024];
            if (!(va >= base && va - base < n))
                continue;
            size_t take = (va - base) + 4096 <= n ? 4096 : n - (va - base);
            memcpy(in, orig + (va - base), take);
            struct a64x_stub_region sr = {stubs_va, stubs, cap, 0, &walk};
            cursor = a64x_translate_page(in, out, 1024, va, &cm, &sr, cursor, &st);
            for (uint32_t w = 0; w < take / 4; w++) {
                /* only this kext's instruction sections (its data islands come back unchanged) */
                bool mine = false;
                for (int i = 0; i < j.nr; i++)
                    if (va + 4 * w >= j.r[i][0] && va + 4 * w < j.r[i][0] + j.r[i][1])
                        mine = true;
                if (!mine)
                    continue;
                words++;
                if (in[w] != out[w]) {
                    changed++;
                    memcpy(blob + (va - base) + 4 * w, &out[w], 4);
                }
            }
        }
        /* data islands inside this kext's instruction sections (the prelinker must not patch them either) */
        for (uint64_t w = 0; w < island_words; w++) {
            if (!(isl[w / 8] & (1u << (w & 7))))
                continue;
            uint64_t e = w;
            while (e + 1 < island_words && (isl[(e + 1) / 8] & (1u << ((e + 1) & 7))))
                e++;
            printf("I %" PRIx64 " %" PRIu64 "\n", lo + 4 * w, e - w + 1);
            w = e;
        }
        printf("S %s %" PRIu64 " %" PRIu64 " %u %u islands=%" PRIu64, j.id, words, changed, first, cursor, is.words);
        for (int c = 1; c < A64X_NCLASSES; c++)
            if (st.rewrite1[c] || st.stubs[c] || st.left[c])
                printf(" %s=%" PRIu64 "/%" PRIu64 "/%" PRIu64, a64x_class_name(c), st.rewrite1[c], st.stubs[c],
                       st.left[c]);
        printf("\n");
        free(fsb);
        free(isl);
        island = NULL;
        free(j.f);
        j.f = NULL;
    }
    free(walk_buf);
    FILE *o = fopen(argv[5], "wb");
    fwrite(blob, 1, n, o);
    fclose(o);
    o = fopen(argv[6], "wb");
    fwrite(stubs, 4, cursor, o);
    fclose(o);
    return 0;
}
