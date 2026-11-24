#include "commands.h"

#include "../macho/pretranslate.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SLICES 16
#define MAX_EDITS  64

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, size_t n, uint8_t **out, size_t *out_len)
{
    if (n & 1)
        return -1;
    *out_len = n / 2;
    if (!*out_len) {
        *out = NULL;
        return 0;
    }
    uint8_t *buf = malloc(*out_len);
    if (!buf)
        return -1;
    for (size_t i = 0; i < *out_len; i++) {
        int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(buf);
            return -1;
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = buf;
    return 0;
}

static int parse_edit(const char *spec, struct a64x_pt_edit *e)
{
    const char *c1 = strchr(spec, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    const char *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
    if (!c1 || !c2 || !c3 || strchr(c3 + 1, ':') || c1 == spec || c2 == c1 + 1 || c3 == c2 + 1)
        return -1;
    if (!isdigit((unsigned char)spec[0]) || !isdigit((unsigned char)c2[1]))
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long off = strtoull(spec, &end, 0);
    if (errno == ERANGE || end != c1)
        return -1;
    errno = 0;
    unsigned long long at = strtoull(c2 + 1, &end, 0);
    if (errno == ERANGE || end != c3 || at > SIZE_MAX)
        return -1;
    uint8_t *stock = NULL, *patched = NULL;
    size_t stock_len = 0, patched_len = 0;
    if (parse_hex(c1 + 1, (size_t)(c2 - (c1 + 1)), &stock, &stock_len) || !stock_len ||
        parse_hex(c3 + 1, strlen(c3 + 1), &patched, &patched_len)) {
        free(stock);
        free(patched);
        return -1;
    }
    e->fileoff = off;
    e->stock = stock;
    e->stock_len = stock_len;
    e->at = (size_t)at;
    e->patched = patched;
    e->patched_len = patched_len;
    return 0;
}

static void free_edits(struct a64x_pt_edit *edits, size_t nedits)
{
    for (size_t i = 0; i < nedits; i++) {
        free((void *)edits[i].stock);
        free((void *)edits[i].patched);
    }
}

static const char *status_name(enum a64x_pt_status s)
{
    return s == A64X_PT_TRANSLATED ? "translated" : s == A64X_PT_FALLBACK ? "fallback" : "unchanged";
}

static void hex(FILE *f, const uint8_t *h)
{
    for (int i = 0; i < A64X_CDHASH_SIZE; i++)
        fprintf(f, "%02x", h[i]);
}

static void counts(FILE *f, const char *name, const uint64_t *v)
{
    fprintf(f, ", \"%s\": {", name);
    const char *sep = "";
    for (int c = 1; c < A64X_NCLASSES; c++) {
        if (!v[c])
            continue;
        fprintf(f, "%s\"%s\": %" PRIu64, sep, a64x_class_name((enum a64x_class)c), v[c]);
        sep = ", ";
    }
    fprintf(f, "}");
}

static void report_slice(FILE *f, const struct a64x_slice *s, const struct a64x_pt_result *r, long verified)
{
    fprintf(f, "{\"cputype\": %u, \"cpusubtype\": %u, \"status\": \"%s\", \"why\": \"%s\", \"cdhash_in\": \"",
            s->cputype, s->cpusubtype, status_name(r->status), r->why);
    hex(f, r->cdhash_in);
    fprintf(f, "\", \"cdhash_out\": \"");
    if (r->status == A64X_PT_TRANSLATED)
        hex(f, r->cdhash_out);
    fprintf(f,
            "\", \"sites\": %" PRIu64 ", \"shared_sites\": %" PRIu64 ", \"shared_copies\": %" PRIu64
            ", \"stub_words\": %" PRIu64 ", \"spill_sites\": %" PRIu64 ", \"dead_checks\": %" PRIu64
            ", \"hole_words\": %" PRIu64 ", \"tail_words\": %" PRIu64 ", \"island_words\": %" PRIu64
            ", \"changed\": %" PRIu64 ", \"verified_sites\": %ld, \"fips_mac\": %s",
            r->sites, r->shared_sites, r->shared_copies, r->stub_words, r->spill_sites, r->dead_checks, r->hole_words,
            r->tail_words, r->island_words, r->changed, verified, r->fips_mac ? "true" : "false");
    counts(f, "rewrite1", r->stats.rewrite1);
    counts(f, "stubs", r->stats.stubs);
    counts(f, "left", r->stats.left);
    fprintf(f, "}");
}

/* `arm64e pretranslate [-r REPORT] [-e FILEOFF:STOCK:AT:PATCHED]... IN OUT`: every arm64 slice translated in place, verified, re-signed */
int a64x_cmd_pretranslate(int argc, char **argv)
{
    const char *report = NULL;
    struct a64x_pt_edit edits[MAX_EDITS];
    size_t nedits = 0;
    int ch;
    while ((ch = getopt(argc, argv, "r:e:")) != -1) {
        if (ch == 'r') {
            report = optarg;
        } else if (ch == 'e') {
            if (nedits >= MAX_EDITS || parse_edit(optarg, &edits[nedits])) {
                fprintf(stderr, "invalid edit spec: %s\n", optarg);
                free_edits(edits, nedits);
                return 2;
            }
            nedits++;
        } else {
            free_edits(edits, nedits);
            return 2;
        }
    }
    if (argc - optind != 2) {
        fprintf(stderr, "usage: arm64e pretranslate [-r REPORT.json] [-e FILEOFF:STOCK:AT:PATCHED]... IN OUT\n");
        free_edits(edits, nedits);
        return 2;
    }
    const char *in = argv[optind], *outp = argv[optind + 1];
    int fd = open(in, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) {
        perror(in);
        free_edits(edits, nedits);
        return 1;
    }
    size_t size = (size_t)st.st_size;
    const uint8_t *file = size ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
    close(fd);
    uint8_t *out = malloc(size ? size : 1);
    if (file == MAP_FAILED || !out) {
        fprintf(stderr, "%s: cannot map\n", in);
        free(out);
        free_edits(edits, nedits);
        return 1;
    }
    if (size)
        memcpy(out, file, size);
    struct a64x_slice slices[MAX_SLICES];
    int n = size ? a64x_macho_slices(file, size, slices, MAX_SLICES) : -1;
    if (nedits && n != 1) {
        fprintf(stderr, "%s: edits require a single-slice Mach-O (got %d)\n", in, n);
        free(out);
        free_edits(edits, nedits);
        return 1;
    }
    FILE *rf = report ? fopen(report, "w") : NULL;
    if (report && !rf) {
        perror(report);
        free(out);
        free_edits(edits, nedits);
        return 1;
    }
    int rc = 0, translated = 0;
    if (rf)
        fprintf(rf, "{\"slices\": [");
    for (int i = 0; i < n && !rc; i++) {
        char err[256];
        struct a64x_macho m;
        struct a64x_pt_result r;
        struct a64x_pt_check v;
        long verified = 0;
        if (a64x_macho_parse(file + slices[i].offset, slices[i].size, &m, err, sizeof(err)) ||
            a64x_pretranslate_edits(&m, out + slices[i].offset, edits, nedits, &r, err, sizeof(err))) {
            fprintf(stderr, "%s: slice %d: %s\n", in, i, err);
            rc = 1;
            break;
        }
        if (r.status == A64X_PT_TRANSLATED) {
            verified = a64x_pt_verify_edits(&m, out + slices[i].offset, edits, nedits, &v, err, sizeof(err));
            if (verified < 0) {
                fprintf(stderr, "%s: slice %d: verification failed: %s\n", in, i, err);
                rc = 1;
            }
            translated++;
        }
        if (rf) {
            fprintf(rf, "%s", i ? ", " : "");
            report_slice(rf, &slices[i], &r, verified);
        }
    }
    if (rf) {
        fprintf(rf, "], \"status\": \"%s\"}\n", translated ? "translated" : n < 0 ? "not-macho" : "unchanged");
        fclose(rf);
    }
    FILE *o = rc ? NULL : fopen(outp, "wb");
    if (!rc && (!o || fwrite(out, 1, size, o) != size || fclose(o))) {
        perror(outp);
        rc = 1;
    }
    free(out);
    free_edits(edits, nedits);
    return rc;
}
