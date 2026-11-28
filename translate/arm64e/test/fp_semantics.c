/*
 * The FP stubs (FJCVTZS, FCMLA/FCADD, FP16, SQRDMLAH/SQRDMLSH) against Apple silicon's own instructions.
 *
 * For each instruction word: run the original instruction and its a64x stub on the same random machine state (X0-X30
 * except X18, NZCV, Q0-Q31) and compare the whole state afterwards. Stubs are generated three times: with no dead
 * registers (everything they touch must be spilled and restored), with X16/X17 dead (those two are then not
 * compared), and with random V registers dead. FPSR is not compared. A word this host does not execute (SIGILL) is
 * an encoding a64x classified but the ISA leaves unallocated: counted, not compared.
 *
 *   fp_semantics_main(argc, argv): ITERS WORD...        these words
 *                                  ITERS -              every word on stdin
 *                                  ITERS --gen N CLASS  N random words of one a64x class, one per encoding form
 */
#include "../a64x/a64x.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct state {
    uint64_t x[31];
    uint64_t nzcv;
    uint64_t q[32][2];
} __attribute__((aligned(16)));

static uint32_t *jit;
#define JIT_WORDS 4096

typedef void (*fn_t)(struct state *);

static int emit_frame(uint32_t *c, const uint32_t *body, int nbody)
{
    int n = 0;
    c[n++] = 0xA9BF7BFDu; /* stp x29, x30, [sp, #-16]! */
    c[n++] = 0xD10283FFu; /* sub sp, sp, #160 */
    for (int i = 0; i < 5; i++)
        c[n++] = 0xA9000000u | ((uint32_t)(2 * i) << 15) | ((20u + 2 * i) << 10) | (31u << 5) |
                 (19u + 2 * i); /* stp x19+2i.. */
    for (int i = 0; i < 4; i++)
        c[n++] = 0x6D000000u | ((uint32_t)(10 + 2 * i) << 15) | ((9u + 2 * i) << 10) | (31u << 5) |
                 (8u + 2 * i); /* stp d8+2i.. */
    c[n++] = 0xF9004BE0u;      /* str x0, [sp, #144] */
    for (uint32_t i = 0; i < 32; i++)
        c[n++] = 0x3DC00000u | ((256u / 16 + i) << 10) | i; /* ldr qi, [x0, #256+16i] */
    c[n++] = 0xF9407C01u;                                   /* ldr x1, [x0, #248] */
    c[n++] = 0xD51B4201u;                                   /* msr nzcv, x1 */
    for (uint32_t i = 1; i < 31; i++)
        if (i != 18)
            c[n++] = 0xF9400000u | (i << 10) | i; /* ldr xi, [x0, #8i] */
    c[n++] = 0xF9400000u;                         /* ldr x0, [x0] */
    memcpy(c + n, body, (size_t)nbody * 4);
    n += nbody;
    c[n++] = 0xA9BF07E0u; /* stp x0, x1, [sp, #-16]! */
    c[n++] = 0xF94053E0u; /* ldr x0, [sp, #160] (state) */
    for (uint32_t i = 2; i < 31; i++)
        if (i != 18)
            c[n++] = 0xF9000000u | (i << 10) | i; /* str xi, [x0, #8i] */
    c[n++] = 0xD53B4201u;                         /* mrs x1, nzcv */
    c[n++] = 0xF9007C01u;                         /* str x1, [x0, #248] */
    c[n++] = 0xA8C10FE2u;                         /* ldp x2, x3, [sp], #16 */
    c[n++] = 0xF9000002u;                         /* str x2, [x0] */
    c[n++] = 0xF9000403u;                         /* str x3, [x0, #8] */
    for (uint32_t i = 0; i < 32; i++)
        c[n++] = 0x3D800000u | ((256u / 16 + i) << 10) | i; /* str qi */
    for (int i = 0; i < 5; i++)
        c[n++] = 0xA9400000u | ((uint32_t)(2 * i) << 15) | ((20u + 2 * i) << 10) | (31u << 5) | (19u + 2 * i);
    for (int i = 0; i < 4; i++)
        c[n++] = 0x6D400000u | ((uint32_t)(10 + 2 * i) << 15) | ((9u + 2 * i) << 10) | (31u << 5) | (8u + 2 * i);
    c[n++] = 0x910283FFu; /* add sp, sp, #160 */
    c[n++] = 0xA8C17BFDu; /* ldp x29, x30, [sp], #16 */
    c[n++] = 0xD65F03C0u; /* ret */
    return n;
}

/* the stub sits inline: its final `b site+4` must land right after it, so site = stub_end - 4 */
static uint32_t cur_vdead;

static fn_t build(uint32_t *at, uint32_t w, bool stub, uint32_t dead, int *nwords)
{
    uint32_t body[256];
    int nb;
    if (!stub) {
        body[0] = w;
        nb = 1;
    } else {
        /* the body starts after the frame prologue: compute where */
        uint32_t tmp[512];
        int pro = emit_frame(tmp, NULL, 0);
        (void)pro;
        int head = 2 + 5 + 4 + 1 + 32 + 2 + 29 + 1; /* prologue words before body */
        uint64_t stub_pc = (uint64_t)(uintptr_t)(at + head);
        int n = a64x_stub_v(w, 0, stub_pc, dead, cur_vdead, false, body, 256);
        if (n <= 0 || n > 256)
            return NULL;
        /* regenerate with the right site: the trailing branch must go to stub_pc + 4n */
        n = a64x_stub_v(w, stub_pc + 4 * (uint64_t)n - 4, stub_pc, dead, cur_vdead, false, body, 256);
        nb = n;
    }
    pthread_jit_write_protect_np(0);
    int n = emit_frame(at, body, nb);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(at, (size_t)n * 4);
    *nwords = nb;
    return (fn_t)(uintptr_t)at;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const uint16_t h_special[] = {0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7D00, 0x7C01,
                                     0x0001, 0x8001, 0x03FF, 0x0400, 0x7BFF, 0xFBFF, 0x3C00, 0xBC00,
                                     0x3800, 0x3C01, 0x4000, 0x5640, 0x1400, 0x0200, 0x7BFE, 0x3555};
static const uint32_t s_special[] = {0,           0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00000u,
                                     1,           0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x3F800000u, 0xBF800000u,
                                     0x3F000001u, 0x4B000000u, 0xCF000000u, 0x4EFFFFFFu, 0x34000000u};
static const uint64_t d_special[] = {0,
                                     0x8000000000000000ull,
                                     0x7FF0000000000000ull,
                                     0xFFF0000000000000ull,
                                     0x7FF8000000000000ull,
                                     0x7FF4000000000000ull,
                                     1,
                                     0x800FFFFFFFFFFFFFull,
                                     0x0010000000000000ull,
                                     0x7FEFFFFFFFFFFFFFull,
                                     0x3FF0000000000000ull,
                                     0xBFF0000000000000ull,
                                     0x41DFFFFFFFC00000ull /* 2^31-1 */,
                                     0xC1E0000000000000ull /* -2^31 */,
                                     0x41E0000000000000ull /* 2^31 */,
                                     0xC1E0000000200000ull /* -2^31-1 */,
                                     0x43E0000000000000ull /* 2^63 */,
                                     0xC3E0000000000000ull,
                                     0x43F0000000000000ull /* 2^64 */,
                                     0x4530000000000001ull /* 2^84+.. */,
                                     0x452FFFFFFFFFFFFFull,
                                     0x4340000000000001ull /* 2^53+2 */,
                                     0x3FE0000000000000ull /* .5 */,
                                     0xBFEFFFFFFFFFFFFFull,
                                     0x41EFFFFFFFE00000ull /* 2^32-1 */,
                                     0x41F0000000000000ull /* 2^32 */,
                                     0xC1F0000000000001ull,
                                     0x433FFFFFFFFFFFFFull,
                                     0x41D0000000000001ull,
                                     0x4440000000000000ull /* 2^69 */,
                                     0x44BFFFFFFFFFFFFFull};

/* lane-typed random fill: kind 0 = half lanes, 1 = single, 2 = double, 3 = raw */
static void fill_q(uint64_t q[2], int kind)
{
    uint16_t *h = (uint16_t *)q;
    uint32_t *s = (uint32_t *)q;
    uint64_t *d = q;
    switch (kind) {
    case 0:
        for (int i = 0; i < 8; i++) {
            uint64_t r = rnd();
            h[i] = (r & 3) == 0   ? h_special[(r >> 8) % (sizeof(h_special) / 2)]
                   : (r & 3) == 1 ? (uint16_t)((r >> 16) & 0x47FF) | (uint16_t)(r >> 63 << 15)
                                  : (uint16_t)(r >> 20);
        }
        break;
    case 1:
        for (int i = 0; i < 4; i++) {
            uint64_t r = rnd();
            s[i] = (r & 3) == 0   ? s_special[(r >> 8) % (sizeof(s_special) / 4)]
                   : (r & 3) == 1 ? (uint32_t)((r >> 16) & 0x40FFFFFFu) | (uint32_t)(r >> 63 << 31)
                                  : (uint32_t)(r >> 24);
        }
        break;
    case 2:
        for (int i = 0; i < 2; i++) {
            uint64_t r = rnd();
            if ((r & 7) < 3)
                d[i] = d_special[(r >> 8) % (sizeof(d_special) / 8)];
            else if ((r & 7) == 3) {
                double v = (double)(int64_t)(rnd() >> (rnd() % 64)) * ((r >> 40) & 1 ? -1 : 1);
                memcpy(&d[i], &v, 8);
            } else if ((r & 7) == 4) {
                double v = ((double)(int32_t)rnd()) + ((r >> 50) & 1 ? 0.5 : 0);
                memcpy(&d[i], &v, 8);
            } else if ((r & 7) == 5)
                d[i] = (rnd() & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 + (rnd() % 100)) << 52);
            else
                d[i] = rnd();
        }
        break;
    case 4: { /* integer lanes with saturation edges (RDM): 16-bit and 32-bit extremes */
        static const uint16_t e16[] = {0x8000, 0x7FFF, 0xFFFF, 0x0000, 0x0001, 0x4000, 0xC000, 0x8001};
        for (int i = 0; i < 8; i++) {
            uint64_t r = rnd();
            h[i] = (r & 1) ? e16[(r >> 8) & 7] : (uint16_t)(r >> 20);
        }
        for (int i = 0; i < 4; i++) {
            uint64_t r = rnd();
            if ((r & 3) == 0)
                s[i] = (r >> 8) & 1 ? 0x80000000u : 0x7FFFFFFFu;
        }
        break;
    }
    default:
        q[0] = rnd();
        q[1] = rnd();
    }
}

static bool no_snan;

static void quiet_all(uint64_t q[2], int kind)
{
    uint16_t *h = (uint16_t *)q;
    uint32_t *s = (uint32_t *)q;
    if (kind == 0)
        for (int i = 0; i < 8; i++) {
            if ((h[i] & 0x7C00) == 0x7C00 && (h[i] & 0x3FF))
                h[i] |= 0x200;
        }
    if (kind == 1)
        for (int i = 0; i < 4; i++) {
            if ((s[i] & 0x7F800000u) == 0x7F800000u && (s[i] & 0x7FFFFF))
                s[i] |= 0x400000;
        }
    if (kind == 2)
        for (int i = 0; i < 2; i++) {
            if ((q[i] & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (q[i] & 0xFFFFFFFFFFFFFull))
                q[i] |= 0x8000000000000ull;
        }
}

static void random_state(struct state *s, int kind)
{
    for (int i = 0; i < 31; i++)
        s->x[i] = rnd();
    s->x[18] = 0;
    s->nzcv = (rnd() & 0xF) << 28;
    for (int i = 0; i < 32; i++) {
        fill_q(s->q[i], kind);
        if (no_snan)
            quiet_all(s->q[i], kind);
    }
}

static bool is_snan_lane(uint64_t v, int kind)
{
    if (kind == 0)
        return (v & 0x7C00) == 0x7C00 && (v & 0x3FF) && !(v & 0x200);
    if (kind == 1)
        return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFF) && !(v & 0x400000);
    return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull) &&
           !(v & 0x8000000000000ull);
}

static bool has_snan(const struct state *s, int kind)
{
    for (int i = 0; i < 32; i++)
        for (int l = 0; l < (kind == 0 ? 8 : kind == 1 ? 4 : 2); l++) {
            uint64_t v = kind == 0   ? ((const uint16_t *)s->q[i])[l]
                         : kind == 1 ? ((const uint32_t *)s->q[i])[l]
                                     : s->q[i][l];
            if (is_snan_lane(v, kind))
                return true;
        }
    return false;
}

/* compare; returns a description of the first difference or NULL */
static uint32_t ignore_v;

static const char *diff(const struct state *a, const struct state *b, uint32_t ignore_x, char *buf)
{
    for (int i = 0; i < 31; i++) {
        if (i == 18 || (ignore_x & (1u << i)))
            continue;
        if (a->x[i] != b->x[i]) {
            snprintf(buf, 160, "x%d %016llx vs %016llx", i, (unsigned long long)a->x[i], (unsigned long long)b->x[i]);
            return buf;
        }
    }
    if (a->nzcv != b->nzcv) {
        snprintf(buf, 160, "nzcv %llx vs %llx", (unsigned long long)a->nzcv >> 28, (unsigned long long)b->nzcv >> 28);
        return buf;
    }
    for (int i = 0; i < 32; i++)
        if (!(ignore_v & (1u << i)) && (a->q[i][0] != b->q[i][0] || a->q[i][1] != b->q[i][1])) {
            snprintf(buf, 160, "q%d %016llx%016llx vs %016llx%016llx", i, (unsigned long long)a->q[i][1],
                     (unsigned long long)a->q[i][0], (unsigned long long)b->q[i][1], (unsigned long long)b->q[i][0]);
            return buf;
        }
    return NULL;
}

/* which lane kind to fill for a word: guessed from its class and size bits */
static int kind_of(uint32_t w)
{
    enum a64x_class c = a64x_classify(w);
    if (c == A64X_JSCVT)
        return 2;
    if (c == A64X_FP16)
        return 0;
    if (c == A64X_RDM)
        return 4;
    if (c == A64X_FCMA) {
        uint32_t size = (w >> 22) & 3;
        if ((w & 0xFF000000u) == 0x2F000000u || (w & 0xFF000000u) == 0x6F000000u)
            return size == 1 ? 0 : 1; /* by element */
        return size == 1 ? 0 : size == 2 ? 1 : 2;
    }
    return 3;
}

static uint64_t total_runs, total_fail, total_snan_skip, words_ok, words_fail, words_nostub, words_host_invalid;

static sigjmp_buf probe_jb;

static void on_sigill(int sig)
{
    (void)sig;
    siglongjmp(probe_jb, 1);
}

/* Does this host execute the original word? */
static bool host_executes(fn_t f, int kind)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, &old);
    bool ok = false;
    if (!sigsetjmp(probe_jb, 1)) {
        struct state s;
        random_state(&s, kind);
        f(&s);
        ok = true;
    }
    sigaction(SIGILL, &old, NULL);
    return ok;
}

static void test_word(uint32_t w, int iters, bool verbose)
{
    int kind = kind_of(w);
    uint32_t *orig_at = jit, *stub_at = jit + 1024, *stub2_at = jit + 2048, *stub3_at = jit + 3072;
    int nw;
    fn_t f0 = build(orig_at, w, false, 0, &nw);
    int ns1 = 0, ns2 = 0, ns3 = 0;
    fn_t f1 = build(stub_at, w, true, 0, &ns1);
    fn_t f2 = build(stub2_at, w, true, (1u << 16) | (1u << 17), &ns2);
    /* third build: some V registers dead (the instruction's own operands excluded by the generator) */
    uint32_t vd = (uint32_t)rnd() & (uint32_t)rnd();
    vd |= 0xFF000000u & (uint32_t)rnd();
    cur_vdead = vd;
    fn_t f3 = build(stub3_at, w, true, 0, &ns3);
    cur_vdead = 0;
    if (!host_executes(f0, kind)) {
        words_host_invalid++;
        if (verbose)
            printf("%08x %-10s not executed by this host (unallocated)\n", w, a64x_class_name(a64x_classify(w)));
        return;
    }
    if (!f1 || !f2 || !f3) {
        words_nostub++;
        if (verbose)
            printf("%08x %-10s no stub\n", w, a64x_class_name(a64x_classify(w)));
        return;
    }
    int fails = 0, snan = 0;
    char buf[160], first[200] = "";
    for (int it = 0; it < iters; it++) {
        struct state s0, a, b, c, d3;
        random_state(&s0, kind);
        a = s0;
        b = s0;
        c = s0;
        d3 = s0;
        f0(&a);
        f1(&b);
        f2(&c);
        f3(&d3);
        total_runs++;
        const char *d1 = diff(&a, &b, 0, buf);
        if (!d1)
            d1 = diff(&a, &c, (1u << 16) | (1u << 17), buf);
        if (!d1) {
            /* dead V registers may hold anything afterwards, except the instruction's own destination */
            uint32_t rd = w & 31, keep = 1u << rd;
            if (a64x_classify(w) == A64X_JSCVT)
                keep = 0;
            ignore_v = vd & ~keep;
            d1 = diff(&a, &d3, 0, buf);
            ignore_v = 0;
        }
        if (d1) {
            if (has_snan(&s0, kind)) {
                snan++;
                total_snan_skip++;
                continue;
            }
            if (!fails)
                snprintf(first, sizeof(first), "%s", d1);
            fails++;
        }
    }
    total_fail += (uint64_t)fails;
    if (fails)
        words_fail++;
    else
        words_ok++;
    if (verbose || fails)
        printf("%08x %-10s stub %2d/%2d words: %s%s (%d sNaN-input diffs not counted)\n", w,
               a64x_class_name(a64x_classify(w)), ns1, ns2, fails ? "FAIL " : "ok", first, snan);
}

/*
 * Random words of one class, at most one per encoding form (register fields cleared), from the top bytes where the
 * FP and SIMD classes live. Deterministic for a seed.
 */
static int gen_words(uint32_t *out, int max, int samples, enum a64x_class want)
{
    static const uint8_t tops[] = {0x0E, 0x2E, 0x4E, 0x6E, 0x5E, 0x7E, 0x1E, 0x9E, 0x0F, 0x2F, 0x4F, 0x6F, 0x5F, 0x7F};
    static uint8_t seen[1u << 23];
    memset(seen, 0, sizeof(seen));
    uint32_t st[A64X_STUB_MAX];
    int n = 0;
    for (int i = 0; i < samples && n < max; i++) {
        uint64_t r = rnd();
        uint32_t w = (uint32_t)tops[(r >> 40) % sizeof(tops)] << 24 | ((uint32_t)r & 0xFFFFFFu);
        if (a64x_classify(w) != want)
            continue;
        uint32_t h = ((w & ~0x001F03FFu) * 2654435761u) >> 6;
        if (seen[h >> 3] & (1u << (h & 7)))
            continue;
        if (a64x_stub(w, 0x100000, 0x200000, 0, st, A64X_STUB_MAX) <= 0)
            continue;
        seen[h >> 3] |= (uint8_t)(1u << (h & 7));
        out[n++] = w;
    }
    return n;
}

/* words that passed, failed, had no stub, were not executed by this host */
void fp_semantics_counts(uint64_t out[4])
{
    out[0] = words_ok;
    out[1] = words_fail;
    out[2] = words_nostub;
    out[3] = words_host_invalid;
}

int fp_semantics_main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 20000;
    jit = mmap(NULL, JIT_WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    no_snan = getenv("NO_SNAN") != NULL;
    bool from_stdin = argc > 2 && !strcmp(argv[2], "-");
    if (from_stdin) {
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            uint32_t w = (uint32_t)strtoul(line, NULL, 16);
            test_word(w, iters, false);
        }
    } else if (argc > 4 && !strcmp(argv[2], "--gen")) {
        static uint32_t words[4096];
        int n = gen_words(words, 4096, atoi(argv[3]), (enum a64x_class)atoi(argv[4]));
        for (int i = 0; i < n; i++)
            test_word(words[i], iters, false);
    } else if (argc > 2) {
        for (int i = 2; i < argc; i++)
            test_word((uint32_t)strtoul(argv[i], NULL, 16), iters, true);
    }
    printf("words ok %llu, failing %llu, without stub %llu, not executed by this host %llu; runs %llu, mismatches %llu "
           "(+%llu with signaling-NaN inputs)\n",
           (unsigned long long)words_ok, (unsigned long long)words_fail, (unsigned long long)words_nostub,
           (unsigned long long)words_host_invalid, (unsigned long long)total_runs, (unsigned long long)total_fail,
           (unsigned long long)total_snan_skip);
    return words_fail ? 1 : 0;
}
