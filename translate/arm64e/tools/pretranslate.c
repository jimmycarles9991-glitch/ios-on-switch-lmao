#include "commands.h"

#include "../macho/pretranslate.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SLICES 16

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

/* `arm64e pretranslate [-r REPORT] IN OUT`: every arm64 slice translated in place, verified, re-signed */
int a64x_cmd_pretranslate(int argc, char **argv)
{
    const char *report = NULL;
    int ch;
    while ((ch = getopt(argc, argv, "r:")) != -1) {
        if (ch != 'r')
            return 2;
        report = optarg;
    }
    if (argc - optind != 2) {
        fprintf(stderr, "usage: arm64e pretranslate [-r REPORT.json] IN OUT\n");
        return 2;
    }
    const char *in = argv[optind], *outp = argv[optind + 1];
    int fd = open(in, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) {
        perror(in);
        return 1;
    }
    size_t size = (size_t)st.st_size;
    const uint8_t *file = size ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
    close(fd);
    uint8_t *out = malloc(size ? size : 1);
    if (file == MAP_FAILED || !out) {
        fprintf(stderr, "%s: cannot map\n", in);
        return 1;
    }
    if (size)
        memcpy(out, file, size);
    struct a64x_slice slices[MAX_SLICES];
    int n = size ? a64x_macho_slices(file, size, slices, MAX_SLICES) : -1;
    FILE *rf = report ? fopen(report, "w") : NULL;
    if (report && !rf) {
        perror(report);
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
            a64x_pretranslate(&m, out + slices[i].offset, &r, err, sizeof(err))) {
            fprintf(stderr, "%s: slice %d: %s\n", in, i, err);
            rc = 1;
            break;
        }
        if (r.status == A64X_PT_TRANSLATED) {
            verified = a64x_pt_verify(&m, out + slices[i].offset, &v, err, sizeof(err));
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
    return rc;
}
