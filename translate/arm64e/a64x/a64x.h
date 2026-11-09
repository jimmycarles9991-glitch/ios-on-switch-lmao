/*
 * a64x: classify and rewrite AArch64 instructions that a Cortex-A57 (ARMv8.0 + CRC32 + AES/SHA1/SHA2/PMULL)
 * cannot execute but arm64e code built for Apple A12 (ARMv8.3) contains.
 *
 * Freestanding C (no libc beyond what the caller provides): the same file builds into XNU and into the host
 * tools and tests. The PAC rewrite table follows Bergamota's Renegade (github.com/ry-diffusion/Bergamota,
 * tools/renegade-rs/src/pacstrip.rs: the mapping only; no code of it is used here), extended.
 */
#ifndef A64X_H
#define A64X_H

#ifdef KERNEL
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

/* Instruction classes. 0 = fine on ARMv8.0 (incl. HINT-space PAC, which is a NOP there). */
enum a64x_class {
    A64X_OK = 0,
    /* --- A12 baseline (arm64e may use these unconditionally): must be handled --- */
    A64X_PAC_BR,     /* BRAA/BRAB/BRAAZ/BRABZ                         -> BR Xn          */
    A64X_PAC_BLR,    /* BLRAA/BLRAB/BLRAAZ/BLRABZ                     -> BLR Xn         */
    A64X_PAC_RET,    /* RETAA/RETAB                                   -> RET            */
    A64X_PAC_ERET,   /* ERETAA/ERETAB (EL1 only)                                        */
    A64X_PAC_DP,     /* PACIA..AUTDZB register forms                  -> NOP            */
    A64X_PAC_XPAC,   /* XPACI/XPACD                                   -> NOP            */
    A64X_PAC_PACGA,  /* PACGA                                         -> EOR/MOV        */
    A64X_PAC_LDRA,   /* LDRAA/LDRAB                                   -> LDR/LDUR or stub */
    A64X_LSE_CAS,    /* CAS{A,L,AL}{B,H,}                             -> stub           */
    A64X_LSE_CASP,   /* CASP{A,L,AL}                                  -> stub           */
    A64X_LSE_LDOP,   /* LD{ADD,CLR,EOR,SET,SMAX,SMIN,UMAX,UMIN}*, ST* -> stub           */
    A64X_LSE_SWP,    /* SWP*                                          -> stub           */
    A64X_RCPC_LDAPR, /* LDAPR{B,H,}                                   -> LDAR           */
    A64X_LOR,        /* LDLAR/STLLR                                   -> LDAR/STLR      */
    A64X_RDM,        /* SQRDMLAH/SQRDMLSH                                               */
    A64X_FP16,       /* half-precision arithmetic (FEAT_FP16)                           */
    A64X_JSCVT,      /* FJCVTZS                                                         */
    A64X_FCMA,       /* FCMLA/FCADD                                                     */
    A64X_DPB,        /* DC CVAP                                       -> DC CVAC        */
    /* --- newer than A12 (A13+): only valid behind a feature check --- */
    A64X_DOTPROD,       /* SDOT/UDOT                                                       */
    A64X_FHM,           /* FMLAL/FMLSL                                                     */
    A64X_SHA3_SM,       /* SHA512/SHA3/SM3/SM4 (0xCE space)                                */
    A64X_RCPC2,         /* LDAPUR/STLUR                                                    */
    A64X_FLAGM,         /* CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG                           */
    A64X_PSTATE,        /* MSR (immediate) to PSTATE fields beyond v8.0 (DIT, SSBS, ...)   */
    A64X_SB,            /* SB                                                              */
    A64X_FRINTTS,       /* FRINT32/64                                                      */
    A64X_I8MM_BF16,     /* SMMLA/UMMLA/USDOT/BFDOT/BFMMLA/BFCVT...                         */
    A64X_THREE_EXTRA,   /* other "three same extra" (unclassified)                         */
    A64X_SYSREG_NEW,    /* MRS/MSR of a register newer than v8.0 (DIT, SSBS, RNDR, ...)    */
    A64X_SYSREG_IMPDEF, /* MRS/MSR of an implementation-defined register (S3_x_C15_...)    */
    A64X_SYS_NEW,       /* SYS ops newer than v8.0 other than DC CVAP                      */
    A64X_APPLE_UDF,     /* Apple AMX/GXF opcodes (0x00201xxx)                              */
    A64X_LSE_OTHER,     /* other encodings in the atomic-memory group (LS64, ...)          */
    A64X_SVE_SME,       /* SVE / SME (A17 Pro/M4+)                                         */
    A64X_CSSC,          /* FEAT_CSSC (min/max immediate, CNT, CTZ, ABS)                    */
    A64X_MTE,           /* FEAT_MTE (IRG, GMI, ADDG, STG, ...)                             */
    A64X_NCLASSES
};

/* Informational (never rewritten): HINT-space pointer authentication, a NOP on ARMv8.0. */
bool a64x_is_pac_hint(uint32_t w);

enum a64x_class a64x_classify(uint32_t w);
enum a64x_class a64x_classify_slow(uint32_t w); /* without the top-byte fast path */
const char *a64x_class_name(enum a64x_class c);
/* true for the A12-baseline classes (those an arm64e binary may execute without a feature check). */
bool a64x_class_is_baseline(enum a64x_class c);

/* One-word ARMv8.0 replacement (PAC identity, LDAPR->LDAR, LDLAR/STLLR, DC CVAP, encodable LDRA). */
bool a64x_rewrite1(uint32_t w, uint32_t *out);
/* GPRs read/written by w (false if not decoded); and the X registers dead at a site given the words after it. */
bool a64x_regs(uint32_t w, uint32_t *rd_mask, uint32_t *wr_mask);
uint32_t a64x_dead_regs(const uint32_t *next, int n);
/* path-sensitive: window[0..n) is the page, site is the index of the instruction being replaced */
uint32_t a64x_dead_regs_window(const uint32_t *window, int n, int site);
uint32_t a64x_dead_regs_window_lr(const uint32_t *window, int n, int site);
uint32_t a64x_dead_regs_budget(const uint32_t *window, int n, int site, int budget, int depth);

/*
 * Scratch of the per-register walks below over windows of up to `words` words: a visited bit per word and the
 * branch targets still to walk. The caller allocates it once (per image scan, page-in or host run) and reuses it for
 * every site: it does not belong on a 16 KB kernel stack that exec or a page fault has already half used. A window
 * longer than `words` finds nothing dead.
 */
#define A64X_WALK_STACK        512
#define A64X_WALK_BYTES(words) (A64X_WALK_STACK * sizeof(int) + ((words) + 7u) / 8u)

struct a64x_walk {
    int *stack;       /* A64X_WALK_STACK entries */
    uint8_t *visited; /* (words + 7) / 8 bytes */
    uint32_t words;
};
/* the walk over buf: A64X_WALK_BYTES(words) bytes, aligned for int */
struct a64x_walk a64x_walk_in(void *buf, uint32_t words);

/* per-register reachability (handles loops); candidates: registers to test */
uint32_t a64x_dead_regs_flow(const uint32_t *window, int n, int site, uint32_t candidates, int budget,
                             const struct a64x_walk *walk);
/* same over a whole function (fn[0] = its first word): branches out of it are tail calls */
uint32_t a64x_dead_regs_flow_fn(const uint32_t *fn, int n, int site, uint32_t candidates, int budget,
                                const struct a64x_walk *walk);
/* SIMD&FP registers dead after the site (a64x_vlive.h); whole_function: branches out of fn are tail calls */
uint32_t a64x_dead_vregs_flow(const uint32_t *fn, int n, int site, uint32_t candidates, int budget, bool whole_function,
                              const struct a64x_walk *walk);
/* V registers one instruction may read / writes in full (conservative; false = not decoded, reads all) */
bool a64x_vregs_public(uint32_t w, uint32_t *vr, uint32_t *vw);

/* classes whose stubs use V temporaries */
static inline bool a64x_class_uses_vtemps(enum a64x_class c)
{
    return c == A64X_FP16 || c == A64X_FCMA || c == A64X_RDM;
}

/*
 * Out-of-line ARMv8.0 code for w (LSE, far LDRA): written to out[0..] for execution at stub_pc, ending in a branch to
 * site_pc + 4. `dead` = registers free at the site (a64x_dead_regs), others are spilled below the red zone.
 * Returns the number of words (may exceed max: then nothing past max was written) or -1 if w has no stub.
 */
int a64x_stub(uint32_t w, uint64_t site_pc, uint64_t stub_pc, uint32_t dead, uint32_t *out, int max);
/* same, with the SIMD&FP registers dead at the site (a64x_dead_vregs_flow): FP stubs use them instead of spilling */
int a64x_stub_v(uint32_t w, uint64_t site_pc, uint64_t stub_pc, uint32_t dead, uint32_t vdead, uint32_t *out, int max);
#define A64X_STUB_MAX 64
/* B from -> to, or 0 if out of range. */
uint32_t a64x_branch(uint64_t from, uint64_t to);

struct a64x_code_map {
    void *ctx;
    bool (*is_code)(void *ctx, uint64_t va); /* is the word at va inside an instruction section? */
    uint32_t flags;                          /* A64X_MAP_* */
    /*
     * Is the word at va entered other than by falling into it (a branch, a jump table, a landing pad, a pointer)?
     * Only stub placement asks (a64x_dead_checks' holes); NULL where nothing is placed.
     */
    bool (*is_target)(void *ctx, uint64_t va);
};

/*
 * Plain arm64 image (not arm64e): it holds no pointer-authentication instructions to rewrite, so PAC classes are
 * left alone (a stray one is data, or reaches the trap path); everything else the A57 lacks is still translated.
 */
#define A64X_MAP_NO_PAC 1u
/*
 * Kernel code (kexts translated at prelink time): also rewrite what only EL1 code uses and the A57 lacks:
 * MSR PAN/UAO/DIT/SSBS/TCO, #imm -> NOP; MRS Xt, <those> -> MOV Xt, #0; MSR <those>, Xt -> NOP.
 */
#define A64X_MAP_KERNEL 2u
bool a64x_rewrite1_kernel(uint32_t w, uint32_t *out);

/* the pointer-authentication classes (rewritten as the identity in arm64e images) */
static inline bool a64x_class_is_pac(enum a64x_class c)
{
    return c >= A64X_PAC_BR && c <= A64X_PAC_LDRA;
}

struct a64x_stub_region {
    uint64_t va;                  /* where the region is mapped (stub branches are PC-relative) */
    uint32_t *words;              /* kernel/host view of it; NULL = only compute the layout */
    uint32_t cap_words;           /* stub words may be placed at indices < cap_words */
    uint32_t first_word;          /* words[0] holds stub word index first_word (a window onto the region) */
    const struct a64x_walk *walk; /* for the FP stubs' liveness: at least the page's word count */
};

struct a64x_stats {
    uint64_t rewrite1[A64X_NCLASSES], stubs[A64X_NCLASSES], left[A64X_NCLASSES];
    uint64_t stub_words, stubs_spill;
};

/*
 * Translate one page (nwords words at va) from in to out. Stubs go to sr at `cursor` (in words); returns the cursor
 * after this page. Deterministic: the same page and cursor give the same output.
 */
uint32_t a64x_translate_page(const uint32_t *in, uint32_t *out, uint32_t nwords, uint64_t va,
                             const struct a64x_code_map *code, struct a64x_stub_region *sr, uint32_t cursor,
                             struct a64x_stats *st);

/*
 * Dead PAC failure checks -> `b ok` (see a64x_image.c). Returns how many; hole() gets each freed run (word index in
 * the page, length) that nothing enters (code->is_target). The kernel and the cache planner use this same function on
 * the same 4 KB pages.
 */
uint32_t a64x_dead_checks(const uint32_t *in, uint32_t *out, uint32_t n, uint64_t va, const struct a64x_code_map *code,
                          void (*hole)(void *ctx, uint32_t index, uint32_t nwords), void *hctx);

/*
 * The code addresses the word w[i] (at va + 4 * i) refers to: a direct branch's target (B, BL, B.cond, CBZ/CBNZ,
 * TBZ/TBNZ), an ADR's, or an ADRP's page plus the ADD of its register among the next four words. Returns how many
 * went to out (0 or 1). a64x_refs.c, host tools only.
 */
uint32_t a64x_code_refs(const uint32_t *w, uint32_t n, uint32_t i, uint64_t va, uint64_t out[1]);
/*
 * The arm64e hardened jump table whose BR x16 is w[8] (w[0..7]: the eight words before it, br_va - 32 up):
 *   [mov x17, #N] cmp x16, #N | csel x16, x16, xzr, ls | adrp x17, T | add x17, x17, T | ldrsw x16, [x17, x16, lsl #2]
 *   | adr x17, B | add x16, x17, x16 | br x16
 * Its targets are B + (int32)T[i], i = 0..N. false if w is no such dispatch; *count is 0 when its bound is neither form.
 */
bool a64x_jump_table(const uint32_t w[9], uint64_t br_va, uint64_t *table, uint64_t *base, uint64_t *count);

struct a64x_islands {
    uint8_t *data;         /* out: 1 bit per word of the image's executable segment, set = data (zeroed by caller) */
    const uint8_t *fstart; /* in: 1 bit per word, set = a function start (LC_FUNCTION_STARTS), or NULL */
    uint64_t piece, pnun;  /* open piece: first word, unallocated words in it */
    bool open, sticky;
    uint64_t runs, words; /* stats: data runs and words found */
    uint64_t tail;        /* the open piece's first word after its last call (BL, BLR) */
    uint32_t last;        /* the open piece's last word */
};

bool a64x_unallocated(uint32_t w);
/*
 * Whether code can stop at w and have padding (a zero or UDF word) or the end of the section next: w ends its block
 * unconditionally (B, BR, RET, BRK) or calls (BL, BLR) a function that does not return. Code never runs into padding,
 * so a run of words that does is data, whatever its words decode as.
 */
bool a64x_ends_code(uint32_t w);
/* feed words [first, first + n) of the segment (va of the first one, code map as for translation), in order */
void a64x_islands_page(struct a64x_islands *is, const uint32_t *w, uint32_t n, uint64_t first, uint64_t va,
                       const struct a64x_code_map *code);
void a64x_islands_end(struct a64x_islands *is, uint64_t end);

#endif /* A64X_H */
