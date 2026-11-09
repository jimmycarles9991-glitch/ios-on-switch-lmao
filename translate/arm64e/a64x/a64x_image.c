/*
 * a64x_image.c: translate code in fixed-size chunks (one VM page), so that a page-in of any page can be redone
 * from that page alone plus one number (the stub cursor at the start of the page).
 *
 * Per page: every word that has a one-word ARMv8.0 form is replaced; every word that needs a stub becomes
 * `B stub`, the stub is allocated at the page's cursor in the image's stub region (LSE, far LDRA, FJCVTZS, FCMA,
 * FP16, RDM); anything else (features newer than A12, forms without a stub) is left as is and reaches the kernel's
 * undefined-instruction path. With A64X_MAP_NO_PAC (plain arm64 images) the PAC classes are not touched.
 * Liveness only looks ahead inside the page, so a re-translation sees exactly what the first one saw.
 */
#include "a64x.h"

static bool needs_stub(enum a64x_class c)
{
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_PAC_LDRA ||
           c == A64X_JSCVT || c == A64X_FCMA || c == A64X_FP16 || c == A64X_RDM || c == A64X_SB;
}

uint32_t a64x_translate_page(const uint32_t *in, uint32_t *out, uint32_t nwords, uint64_t va,
                             const struct a64x_code_map *code, struct a64x_stub_region *sr, uint32_t cursor,
                             struct a64x_stats *st)
{
    uint32_t stub_buf[A64X_STUB_MAX];
    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t w = in[i], one;
        out[i] = w;
        if (code && !code->is_code(code->ctx, va + 4ull * i)) {
            continue;
        }
        enum a64x_class c = a64x_classify(w);
        if (code && (code->flags & A64X_MAP_KERNEL) && a64x_rewrite1_kernel(w, &one)) {
            out[i] = one;
            if (st) {
                st->rewrite1[c]++;
            }
            continue;
        }
        if (c == A64X_OK || (code && (code->flags & A64X_MAP_NO_PAC) && a64x_class_is_pac(c))) {
            continue;
        }
        if (c == A64X_PAC_LDRA ? a64x_rewrite1(w, &one) : (!needs_stub(c) && a64x_rewrite1(w, &one))) {
            out[i] = one;
            if (st) {
                st->rewrite1[c]++;
            }
            continue;
        }
        if (needs_stub(c) && sr) {
            uint64_t site = va + 4ull * i;
            uint64_t stub_va = sr->va + 4ull * cursor;
            uint32_t dead = a64x_dead_regs_window(in, (int)nwords, (int)i);
            /* V temporaries: those dead within the page (path walk as for X registers) need no save */
            uint32_t vdead = a64x_class_uses_vtemps(c)
                                 ? a64x_dead_vregs_flow(in, (int)nwords, (int)i, 0xFFFFFFFFu, 2000, false, sr->walk)
                                 : 0;
            int n = a64x_stub_v(w, site, stub_va, dead, vdead, stub_buf, A64X_STUB_MAX);
            uint32_t b = n > 0 ? a64x_branch(site, stub_va) : 0;
            if (n > 0 && n <= A64X_STUB_MAX && b && cursor + (uint32_t)n <= sr->cap_words) {
                if (sr->words) {
                    for (int k = 0; k < n; k++) {
                        sr->words[cursor + (uint32_t)k - sr->first_word] = stub_buf[k];
                    }
                }
                cursor += (uint32_t)n;
                out[i] = b;
                if (st) {
                    st->stubs[c]++;
                    st->stub_words += (uint64_t)n;
                    if ((stub_buf[0] & 0xFFC003FFu) == 0xD10003FFu) {
                        st->stubs_spill++; /* starts with SUB SP, SP, #frame */
                    }
                }
                continue;
            }
        }
        if (st) {
            st->left[c]++;
        }
    }
    return cursor;
}

/*
 * PAC failure checks that pass by construction (nothing is ever signed) -> `b ok`, freeing the words after it:
 *   [aut{i,b}sp][eor x16,x30,x30,lsl #1][tbz x16,#62,ok][brk #0xc47x] ok:                 3 free words
 *   [aut* x16,x17][mov x17,x16][xpac* x17][cmp x16,x17][b.eq ok][brk #0xc47x] ok:          5 free words
 * x16, x17 and NZCV are clobbered by these sequences by definition, so skipping them changes nothing observable.
 * Only sequences entirely inside [0, n) and inside code are taken (the kernel sees one page at a time; the planner
 * applies the same rule). `hole` (optional) is told every freed run that nothing enters: compilers share one check
 * between a signed path and an unsigned early return that branches straight to its EOR, and those words stay live.
 */
static bool is_check_brk(uint32_t w)
{
    return (w & 0xFFE0001Fu) == 0xD4200000u && ((w >> 5) & 0xFFFCu) == 0xC470u;
}

uint32_t a64x_dead_checks(const uint32_t *in, uint32_t *out, uint32_t n, uint64_t va, const struct a64x_code_map *code,
                          void (*hole)(void *ctx, uint32_t index, uint32_t nwords), void *hctx)
{
    uint32_t found = 0;
    for (uint32_t k = 3; k < n; k++) {
        if (!is_check_brk(in[k])) {
            continue;
        }
        uint32_t first = 0, len = 0;
        if ((in[k - 3] == 0xD50323FFu || in[k - 3] == 0xD50323BFu) && in[k - 2] == 0xCA1E07D0u &&
            in[k - 1] == 0xB6F00050u) {
            first = k - 3;
            len = 4;
        } else if (k >= 5 && a64x_classify(in[k - 5]) == A64X_PAC_DP && (in[k - 5] & 0x1F) == 16 &&
                   in[k - 4] == 0xAA1003F1u && a64x_classify(in[k - 3]) == A64X_PAC_XPAC && (in[k - 3] & 0x1F) == 17 &&
                   in[k - 2] == 0xEB11021Fu && in[k - 1] == 0x54000040u) {
            first = k - 5;
            len = 6;
        } else {
            continue;
        }
        bool all_code = true;
        for (uint32_t q = first; q <= k && code; q++) {
            if (!code->is_code(code->ctx, va + 4ull * q)) {
                all_code = false;
            }
        }
        if (!all_code) {
            continue;
        }
        out[first] = 0x14000000u | len; /* b ok (the word after the brk) */
        bool entered = false;
        for (uint32_t q = first + 1; hole && q <= k && code && code->is_target; q++) {
            entered |= code->is_target(code->ctx, va + 4ull * q);
        }
        if (hole && !entered) {
            hole(hctx, first + 1, len - 1);
        }
        found++;
    }
    return found;
}

/*
 * Data islands in an image translated page by page (no plan, no whole-image reachability): the words of its
 * instruction sections are cut into runs at UDF/zero words, at non-code words and at function starts
 * (LC_FUNCTION_STARTS); a run holding at least one unallocated encoding per 32 words is data (assembler tables:
 * K256, sincos coefficients, Hann windows are 8-50% unallocated; compiled code has none), and so is what a piece runs
 * into padding with, after its last word that ends code (a64x_ends_code): a table of 64-bit values below 2^32 is
 * single words between zero halves, each read alone (lzfse's mask table: 0x1fffffff decodes as an FP16 FNMSUB), and
 * a jump table can sit right behind the code that dispatches through it. LC_DATA_IN_CODE ranges are
 * marked by the caller. Streamed: pages in order, the open run carries over page boundaries.
 */
bool a64x_unallocated(uint32_t w)
{
    uint32_t op0 = (w >> 25) & 0xF;
    return op0 == 1 || op0 == 3 || (op0 == 0 && !(w >> 31) && (w >> 16) && (w & 0xFFFFF000u) != 0x00201000u);
}

/*
 * Within a run, pieces end after an unconditional terminator (RET*, B, BR*, BRK): a function followed by its constant
 * pool with no symbol in between is one run, and only the pool must go. A data piece makes the rest of its run data
 * (a table split by words that happen to look like branches).
 */
static void piece_close(struct a64x_islands *is, uint64_t end, bool padded)
{
    if (is->open && end > is->piece) {
        /*
		 * (Requiring two unallocated words per piece would keep 4 more obfuscated-code rewrites in the iPadOS cache but
		 * miss one data word in each cache: silent corruption is the worse failure, so one word is enough.)
		 */
        bool data = is->sticky || (is->pnun && is->pnun * 32 >= end - is->piece);
        uint64_t from = is->piece;
        if (!data && padded && !a64x_ends_code(is->last)) {
            data = true;
            from = is->tail; /* the code up to its last call stays */
        }
        if (data) {
            for (uint64_t k = from; k < end; k++) {
                is->data[k >> 3] |= (uint8_t)(1u << (k & 7));
            }
            if (!is->sticky)
                is->runs++;
            is->sticky = true;
            is->words += end - from;
        }
    }
    is->piece = end;
    is->tail = end;
    is->pnun = 0;
}

static void islands_close(struct a64x_islands *is, uint64_t end, bool padded)
{
    piece_close(is, end, padded);
    is->open = false;
    is->sticky = false;
}

static bool terminator(uint32_t w)
{
    uint32_t one;
    if (a64x_rewrite1(w, &one)) {
        w = one;
    }
    return (w & 0xFC000000u) == 0x14000000u || (w & 0xFFFFFC1Fu) == 0xD61F0000u || w == 0xD65F03C0u ||
           (w & 0xFFE0001Fu) == 0xD4200000u;
}

bool a64x_ends_code(uint32_t w)
{
    uint32_t one;
    if (a64x_rewrite1(w, &one)) {
        w = one;
    }
    return terminator(w) || (w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u;
}

void a64x_islands_page(struct a64x_islands *is, const uint32_t *w, uint32_t n, uint64_t first, uint64_t va,
                       const struct a64x_code_map *code)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t k = first + i;
        bool fs = is->fstart && (is->fstart[k >> 3] & (1u << (k & 7)));
        if ((code && !code->is_code(code->ctx, va + 4ull * i)) || !(w[i] >> 16)) {
            islands_close(is, k, true);
            continue;
        }
        if (fs || !is->open) {
            islands_close(is, k, false);
            is->open = true;
            is->piece = is->tail = k;
        }
        is->pnun += a64x_unallocated(w[i]);
        is->last = w[i];
        if (a64x_ends_code(w[i])) {
            is->tail = k + 1;
        }
        if (terminator(w[i])) {
            piece_close(is, k + 1, false);
        }
    }
}

void a64x_islands_end(struct a64x_islands *is, uint64_t end)
{
    islands_close(is, end, true);
}
