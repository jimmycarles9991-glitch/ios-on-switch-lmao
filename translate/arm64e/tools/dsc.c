#include "commands.h"

#include "../a64x/a64x.h"
#include "../macho/dsc.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t *map_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) {
        perror(path);
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    void *p = st.st_size ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "%s: cannot map\n", path);
        return NULL;
    }
    *size = (size_t)st.st_size;
    return p;
}

static int hexbytes(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int cmp_pair(const void *a, const void *b)
{
    return memcmp(a, b, A64X_CDHASH_SIZE);
}

/* "OLD NEW" hex cdhashes, one translated slice per line */
static struct a64x_cdhash_pair *read_pairs(const char *path, size_t *n)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return NULL;
    }
    size_t cap = 1024;
    struct a64x_cdhash_pair *p = malloc(cap * sizeof(*p));
    char line[256], a[64], b[64];
    *n = 0;
    while (p && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %63s", a, b) != 2 || strlen(a) != 40 || strlen(b) != 40)
            continue;
        if (*n == cap)
            p = realloc(p, (cap *= 2) * sizeof(*p));
        if (p && !hexbytes(a, p[*n].old, 20) && !hexbytes(b, p[*n].now, 20))
            (*n)++;
    }
    fclose(f);
    if (p)
        qsort(p, *n, sizeof(*p), cmp_pair);
    return p;
}

static void print_cdhash(const uint8_t h[A64X_CDHASH_SIZE])
{
    for (int i = 0; i < A64X_CDHASH_SIZE; i++)
        printf("%02x", h[i]);
}

static int open_plan(const char *path, struct a64x_plan *plan)
{
    size_t size;
    char err[256];
    const uint8_t *buf = map_file(path, &size);
    if (!buf)
        return -1;
    if (a64x_plan_open(buf, size, plan, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", path, err);
        return -1;
    }
    return 0;
}

int a64x_cmd_dsc_apply(int argc, char **argv)
{
    const char *pairs_path = NULL;
    int ch;
    while ((ch = getopt(argc, argv, "c:")) != -1) {
        if (ch != 'c')
            return 2;
        pairs_path = optarg;
    }
    if (argc - optind != 3) {
        fprintf(stderr, "usage: arm64e dsc-apply [-c CDHASH_PAIRS] PLAN CACHE_FILE OUT\n");
        return 2;
    }
    struct a64x_plan plan;
    struct a64x_dsc_file f;
    size_t size, npairs = 0;
    char err[256];
    if (open_plan(argv[optind], &plan))
        return 1;
    const uint8_t *buf = map_file(argv[optind + 1], &size);
    struct a64x_cdhash_pair *pairs = pairs_path ? read_pairs(pairs_path, &npairs) : NULL;
    if (!buf || (pairs_path && !pairs))
        return 1;
    if (a64x_dsc_parse(buf, size, &f, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", argv[optind + 1], err);
        return 1;
    }
    uint8_t *out = malloc(size), cdhash[A64X_CDHASH_SIZE];
    struct a64x_dsc_stats st;
    if (!out || a64x_dsc_apply(&plan, &f, pairs, npairs, out, cdhash, &st, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", argv[optind + 1], out ? err : "out of memory");
        return 1;
    }
    FILE *o = fopen(argv[optind + 2], "wb");
    if (!o || fwrite(out, 1, size, o) != size || fclose(o)) {
        perror(argv[optind + 2]);
        return 1;
    }
    print_cdhash(cdhash);
    printf(" pages %" PRIu64 " changed %" PRIu64 " plan %" PRIu64 " dead_checks %" PRIu64 " cdhashes %" PRIu64
           " fips_macs %" PRIu64 "\n",
           st.pages, st.words_changed, st.plan_words, st.dead_checks, st.cdhashes_replaced, st.fips_macs);
    free(out);
    free(pairs);
    return 0;
}

#define MAX_FILES 256

/* main cache file + its subcaches, as named in the main header (dyld_subcache_entry: 56 bytes, suffix at 24) */
static int cache_files(const char *main_path, char names[][1024], int max)
{
    size_t size;
    const uint8_t *m = map_file(main_path, &size);
    if (!m || size < 0x190)
        return -1;
    uint32_t off, count;
    memcpy(&off, m + 0x188, 4);
    memcpy(&count, m + 0x18C, 4);
    if ((int)count + 1 > max || off + 56ull * count > size)
        return -1;
    snprintf(names[0], 1024, "%s", main_path);
    for (uint32_t i = 0; i < count; i++)
        snprintf(names[i + 1], 1024, "%s%.32s", main_path, (const char *)m + off + 56 * i + 24);
    munmap((void *)m, size);
    return (int)count + 1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int a64x_cmd_dsc_verify(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: arm64e dsc-verify PLAN ORIGINAL_MAIN_CACHE TRANSLATED_DIR|-   (- = translate here)\n");
        return 2;
    }
    static char names[MAX_FILES][1024];
    static struct a64x_dsc_file orig[MAX_FILES], out[MAX_FILES];
    struct a64x_plan plan;
    char err[256];
    int n = cache_files(argv[2], names, MAX_FILES);
    if (open_plan(argv[1], &plan) || n < 0)
        return 1;
    for (int i = 0; i < n; i++) {
        size_t size;
        const uint8_t *buf = map_file(names[i], &size);
        if (!buf || a64x_dsc_parse(buf, size, &orig[i], err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", names[i], buf ? err : "unreadable");
            return 1;
        }
        const uint8_t *now;
        if (!strcmp(argv[3], "-")) {
            uint8_t *t = malloc(size), h[A64X_CDHASH_SIZE];
            struct a64x_dsc_stats st;
            if (!t || a64x_dsc_apply(&plan, &orig[i], NULL, 0, t, h, &st, err, sizeof(err))) {
                fprintf(stderr, "%s: %s\n", names[i], t ? err : "out of memory");
                return 1;
            }
            now = t;
            print_cdhash(h);
            printf(" %s\n", base_name(names[i]));
        } else {
            char path[2048];
            size_t nsize;
            snprintf(path, sizeof(path), "%s/%s", argv[3], base_name(names[i]));
            now = map_file(path, &nsize);
            if (!now || nsize != size) {
                fprintf(stderr, "%s: missing or of another size\n", path);
                return 1;
            }
        }
        if (a64x_dsc_parse(now, size, &out[i], err, sizeof(err)))
            return 1;
    }
    struct a64x_dsc_verify v;
    int rc = a64x_dsc_verify(&plan, orig, out, n, &v, err, sizeof(err));
    printf("verify: %d files, %" PRIu64 " code words: %" PRIu64 " one-word rewrites, %" PRIu64 " dead checks, %" PRIu64
           " stub words (%" PRIu64 " entered by a branch), sites %" PRIu64 " ok %" PRIu64
           " broken; A12-baseline words left %" PRIu64 "; FIPS MACs %" PRIu64 "\n",
           n, v.code_words, v.rewritten, v.dead_checks, v.stub_words, v.entered_stub_words, v.sites_ok, v.sites_bad,
           v.left_baseline, v.fips_macs);
    for (int c = 1; c < 64 && c < A64X_NCLASSES; c++) {
        if (v.sites[c] || v.left[c])
            printf("  %-12s sites %" PRIu64 " left %" PRIu64 "\n", a64x_class_name((enum a64x_class)c), v.sites[c],
                   v.left[c]);
    }
    if (rc)
        fprintf(stderr, "dsc-verify: %s\n", err);
    return rc ? 1 : 0;
}
