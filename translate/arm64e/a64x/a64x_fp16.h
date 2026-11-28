/*
 * a64x_fp16.h (included by a64x_rewrite.c): FEAT_FP16 half-precision arithmetic on ARMv8.0.
 *
 * Every half operation is widened (FCVT/FCVTL, exact), done in single precision, and narrowed (FCVT/FCVTN, one
 * rounding). For +, -, *, /, sqrt, min/max, compares, conversions and roundings this is exact: single has 24 bits and
 * 24 >= 2*11 + 2, so the double rounding is innocuous. Fused operations (FMADD family, FMLA/FMLS, FRECPS/FRSQRTS) are
 * done in double precision and narrowed through FCVTXN (round to odd) so the result is the correctly rounded fused
 * half result. Sign/bit operations (FABS, FNEG, FMOV, FCSEL) work on the raw bits so NaN payloads pass untouched.
 *
 * Known difference: a signaling NaN input is quieted by the widening before the operation sees it, so when one
 * operand is a quiet NaN and a later operand a signaling NaN the propagated payload can differ. FPSR cumulative flags
 * are not reproduced exactly.
 */

static inline uint32_t f_fcvtl(bool hi, uint32_t d, uint32_t n)
{
    return 0x0E217800u | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .4s <- .4h/.8h */

static inline uint32_t f_fcvtl_d(bool hi, uint32_t d, uint32_t n)
{
    return 0x0E617800u | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .2d <- .2s/.4s */

static inline uint32_t f_fcvtn(bool hi, uint32_t d, uint32_t n)
{
    return 0x0E216800u | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .4h/.8h <- .4s */

static inline uint32_t f_fcvtxn(bool hi, uint32_t d, uint32_t n)
{
    return 0x2E616800u | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .2s/.4s <- .2d, odd */

static inline uint32_t f_xtn(bool hi, uint32_t d, uint32_t n)
{
    return 0x0E612800u | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .4h <- .4s */

static inline uint32_t f_sqxtn(bool hi, bool u, uint32_t d, uint32_t n)
{
    return (u ? 0x2E614800u : 0x0E614800u) | ((uint32_t)hi << 30) | (n << 5) | d;
}

static inline uint32_t f_sxtl(bool hi, bool u, uint32_t d, uint32_t n)
{
    return (u ? 0x2F10A400u : 0x0F10A400u) | ((uint32_t)hi << 30) | (n << 5) | d;
} /* .4s <- .4h */

static inline uint32_t f_cvt_s_h(uint32_t d, uint32_t n)
{
    return 0x1EE24000u | (n << 5) | d;
} /* FCVT Sd, Hn */

static inline uint32_t f_cvt_d_h(uint32_t d, uint32_t n)
{
    return 0x1EE2C000u | (n << 5) | d;
} /* FCVT Dd, Hn */

static inline uint32_t f_cvt_h_s(uint32_t d, uint32_t n)
{
    return 0x1E23C000u | (n << 5) | d;
} /* FCVT Hd, Sn */

static inline uint32_t f_cvt_h_d(uint32_t d, uint32_t n)
{
    return 0x1E63C000u | (n << 5) | d;
} /* FCVT Hd, Dn */

static inline uint32_t f_fmov_ss(uint32_t d, uint32_t n)
{
    return 0x1E204000u | (n << 5) | d;
} /* FMOV Sd, Sn (clears above) */

static inline uint32_t f_dup_h(uint32_t d, uint32_t n, uint32_t idx)
{
    return 0x4E000400u | (((idx << 2) | 2u) << 16) | (n << 5) | d;
} /* DUP Vd.8h, Vn.h[i] */

static inline uint32_t f_dup_hs(uint32_t d, uint32_t n, uint32_t idx)
{
    return 0x5E000400u | (((idx << 2) | 2u) << 16) | (n << 5) | d;
} /* DUP Hd, Vn.h[i] */

/* replace Rd/Rn/Rm/Ra fields */
static inline uint32_t f_regs(uint32_t w, uint32_t d, uint32_t n, uint32_t m)
{
    return (w & ~0x001F03FFu) | (m << 16) | (n << 5) | d;
}

static inline uint32_t f_regs1(uint32_t w, uint32_t d, uint32_t n)
{
    return (w & ~0x3FFu) | (n << 5) | d;
} /* unary: bits 20:16 are opcode */

static void f_isolate_h(struct emit *e, uint32_t d, uint32_t n)
{
    put(e, f_dup_hs(d, n, 0));
}

/* the single-precision form of a half-precision Advanced SIMD three-same / two-reg-misc word */
static inline uint32_t f_three_to_s(uint32_t w)
{
    return (w & ~((3u << 21) | (3u << 14))) | (1u << 21) | (3u << 14);
}

static inline uint32_t f_misc_to_s(uint32_t w)
{
    return (w & ~(0x3Fu << 17)) | (0x10u << 17);
}

struct f16 {
    struct emit e;
    uint32_t v[10]; /* vector temporaries */
    int nv;
    uint32_t vs[10]; /* the ones that must be saved (not dead at the site) */
    int nvs;
    uint32_t g[2]; /* GPR temporaries */
    uint32_t gspill;
};

static int f16_begin(struct f16 *f, uint32_t *out, int max, uint64_t stub_pc, uint32_t vavoid, int nv, uint32_t dead,
                     uint32_t vdead, uint32_t gavoid, int ng)
{
    f->e = (struct emit){out, 0, max, stub_pc};
    f->nv = nv;
    f->nvs = 0;
    f->gspill = 0;
    if (ng && pick_scratch(dead, gavoid | XMASK(18) | XMASK(29) | XMASK(30), ng, f->g, &f->gspill) < 0) {
        return -1;
    }
    if (nv && pick_vtemps_dead(vdead, vavoid, nv, f->v, f->vs, &f->nvs) < 0) {
        return -1;
    }
    emit_spill(&f->e, f->gspill, true);
    nv = f->nvs;
    if (nv) {
        /* below the GPR frame if there is one (that one already skipped the red zone) */
        uint32_t frame = (f->gspill ? 0 : REDZONE) + 16u * (uint32_t)((nv + 1) & ~1);
        const uint32_t *v = f->vs;
        put(&f->e, nv >= 2 ? 0xAD800000u | ((((uint32_t)(-(int32_t)frame / 16)) & 0x7F) << 15) | (v[1] << 10) |
                                 (31u << 5) | v[0]
                           : 0x3C800C00u | ((((uint32_t)(-(int32_t)frame)) & 0x1FF) << 12) | (31u << 5) | v[0]);
        for (int i = 2; i < nv; i += 2) {
            put(&f->e, i + 1 < nv ? 0xAD000000u | ((uint32_t)i << 15) | (v[i + 1] << 10) | (31u << 5) | v[i]
                                  : 0x3D800000u | ((uint32_t)i << 10) | (31u << 5) | v[i]);
        }
    }
    return 0;
}

static int f16_end(struct f16 *f, bool shared, uint64_t ret_pc)
{
    int nv = f->nvs;
    const uint32_t *v = f->vs;
    if (nv) {
        uint32_t frame = (f->gspill ? 0 : REDZONE) + 16u * (uint32_t)((nv + 1) & ~1);
        for (int i = 2; i < nv; i += 2) {
            put(&f->e, i + 1 < nv ? 0xAD400000u | ((uint32_t)i << 15) | (v[i + 1] << 10) | (31u << 5) | v[i]
                                  : 0x3DC00000u | ((uint32_t)i << 10) | (31u << 5) | v[i]);
        }
        put(&f->e, nv >= 2 ? 0xACC00000u | (((frame / 16) & 0x7F) << 15) | (v[1] << 10) | (31u << 5) | v[0]
                           : 0x3CC00400u | ((frame & 0x1FF) << 12) | (31u << 5) | v[0]);
    }
    emit_spill(&f->e, f->gspill, false);
    emit_ret(&f->e, shared, ret_pc);
    return f->e.n;
}

enum f16_narrow {
    NARROW_FCVTN,
    NARROW_XTN,
    NARROW_SQXTN,
    NARROW_UQXTN
};

static void f16_narrow(struct emit *e, enum f16_narrow k, bool hi, uint32_t d, uint32_t n)
{
    switch (k) {
    case NARROW_FCVTN:
        put(e, f_fcvtn(hi, d, n));
        break;
    case NARROW_XTN:
        put(e, f_xtn(hi, d, n));
        break;
    case NARROW_SQXTN:
        put(e, f_sqxtn(hi, false, d, n));
        break;
    case NARROW_UQXTN:
        put(e, f_sqxtn(hi, true, d, n));
        break;
    }
}

/* lane-wise binary op: op4s is the single-precision .4s word (registers replaced) */
static void f16_vec2(struct f16 *f, bool q, uint32_t op4s, enum f16_narrow k, uint32_t d, uint32_t n, uint32_t m)
{
    uint32_t a = f->v[0], b = f->v[1], c = f->v[2];
    put(&f->e, f_fcvtl(false, a, n));
    put(&f->e, f_fcvtl(false, b, m));
    put(&f->e, f_regs(op4s, a, a, b));
    if (q) {
        put(&f->e, f_fcvtl(true, c, n));
        put(&f->e, f_fcvtl(true, b, m));
        put(&f->e, f_regs(op4s, c, c, b));
    }
    f16_narrow(&f->e, k, false, d, a);
    if (q) {
        f16_narrow(&f->e, k, true, d, c);
    }
}

/* pairwise binary op (FADDP...): result = [pairs of n, pairs of m] */
static void f16_pair(struct f16 *f, bool q, uint32_t op4s, uint32_t d, uint32_t n, uint32_t m)
{
    uint32_t a = f->v[0], b = f->v[1], c = f->v[2];
    if (!q) {
        put(&f->e, f_fcvtl(false, a, n));
        put(&f->e, f_fcvtl(false, b, m));
        put(&f->e, f_regs(op4s, a, a, b));
        put(&f->e, f_fcvtn(false, d, a));
        return;
    }
    put(&f->e, f_fcvtl(false, a, n));
    put(&f->e, f_fcvtl(true, b, n));
    put(&f->e, f_regs(op4s, a, a, b)); /* pairs of n */
    put(&f->e, f_fcvtl(false, b, m));
    put(&f->e, f_fcvtl(true, c, m));
    put(&f->e, f_regs(op4s, b, b, c)); /* pairs of m */
    put(&f->e, f_fcvtn(false, d, a));
    put(&f->e, f_fcvtn(true, d, b));
}

/* lane-wise unary op; widen: 0 = FCVTL, 1 = SXTL, 2 = UXTL (integer lanes) */
static void f16_vec1(struct f16 *f, bool q, uint32_t op4s, int widen, enum f16_narrow k, uint32_t d, uint32_t n)
{
    uint32_t a = f->v[0], c = f->v[1];
    put(&f->e, widen ? f_sxtl(false, widen == 2, a, n) : f_fcvtl(false, a, n));
    put(&f->e, f_regs1(op4s, a, a));
    if (q) {
        put(&f->e, widen ? f_sxtl(true, widen == 2, c, n) : f_fcvtl(true, c, n));
        put(&f->e, f_regs1(op4s, c, c));
    }
    f16_narrow(&f->e, k, false, d, a);
    if (q) {
        f16_narrow(&f->e, k, true, d, c);
    }
}

/*
 * fused ops in double: op2d is the .2d word. acc: Vd is also an input (FMLA/FMLS). elem_idx >= 0: Vm[elem_idx]
 * duplicated and widened once into Y. Outputs stay in T/R until the final FCVTN{2} so aliasing d with n or m is safe.
 */
static void f16_fused(struct f16 *f, bool q, uint32_t op2d, bool acc, int elem_idx, uint32_t d, uint32_t n, uint32_t m)
{
    int vk = 0;
    uint32_t X = f->v[vk++], Y = f->v[vk++], T = f->v[vk++];
    uint32_t Z = acc ? f->v[vk++] : X;
    uint32_t R = q ? f->v[vk++] : T;
    if (elem_idx >= 0) {
        put(&f->e, f_dup_h(Y, m, (uint32_t)elem_idx));
        put(&f->e, f_fcvtl(false, Y, Y));
        put(&f->e, f_fcvtl_d(false, Y, Y));
    }
    for (int half = q ? 1 : 0; half >= 0; half--) {
        bool hi = half == 1;
        uint32_t res = hi ? R : T;
        for (int pair = 0; pair < 2; pair++) {
            put(&f->e, f_fcvtl(hi, X, n));
            put(&f->e, f_fcvtl_d(pair, X, X));
            if (elem_idx < 0) {
                put(&f->e, f_fcvtl(hi, Y, m));
                put(&f->e, f_fcvtl_d(pair, Y, Y));
            }
            if (acc) {
                put(&f->e, f_fcvtl(hi, Z, d));
                put(&f->e, f_fcvtl_d(pair, Z, Z));
            }
            put(&f->e, f_regs(op2d, Z, X, Y));
            put(&f->e, f_fcvtxn(pair, res, Z));
        }
    }
    put(&f->e, f_fcvtn(false, d, T));
    if (q) {
        put(&f->e, f_fcvtn(true, d, R));
    }
}

/* across lanes (FMAXV...): reduce each half in single, combine with the scalar op, narrow */
static void f16_across(struct f16 *f, bool q, uint32_t opv4s, uint32_t ops, uint32_t d, uint32_t n)
{
    uint32_t a = f->v[0], c = f->v[1];
    put(&f->e, f_fcvtl(false, a, n));
    if (!q) {
        put(&f->e, f_regs1(opv4s, a, a));
    } else {
        put(&f->e, f_fcvtl(true, c, n));
        put(&f->e, f_regs1(opv4s, a, a));
        put(&f->e, f_regs1(opv4s, c, c));
        put(&f->e, f_regs(ops, a, a, c));
    }
    put(&f->e, f_cvt_h_s(d, a));
}

static int stub_fp16(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t vdead, bool shared,
                     uint32_t *out, int max)
{
    uint32_t d = BITS(w, 4, 0), n = BITS(w, 9, 5), m = BITS(w, 20, 16), a = BITS(w, 14, 10);
    uint32_t vav = (1u << d) | (1u << n) | (1u << m);
    struct f16 f;
    bool q = BIT(w, 30);

    /* Scalar floating-point (ftype = 11) */
    if ((w & 0x7FE00000u) == 0x1EE00000u || (w & 0x7FE00000u) == 0x1EC00000u || (w & 0xFFC00000u) == 0x1FC00000u) {
        if ((w & 0xFFC00000u) == 0x1FC00000u) {
            /* FMADD/FMSUB/FNMADD/FNMSUB Hd, Hn, Hm, Ha: in double */
            if (f16_begin(&f, out, max, stub_pc, vav | (1u << a), 3, dead, vdead, 0, 0) < 0)
                return -1;
            uint32_t x = f.v[0], y = f.v[1], z = f.v[2];
            put(&f.e, f_cvt_d_h(x, n));
            put(&f.e, f_cvt_d_h(y, m));
            put(&f.e, f_cvt_d_h(z, a));
            put(&f.e, (w & 0xFF208000u & ~0x00C00000u) | 0x00400000u | (z << 10) | (y << 16) | (x << 5) | x);
            put(&f.e, f_cvt_h_d(d, x));
            return f16_end(&f, shared, ret_pc);
        }
        if (!BIT(w, 21) || BITS(w, 15, 10) == 0) {
            goto conversion;
        }
        if (BIT(w, 31)) {
            return -1;
        }
        if ((w & 0xFFE07C00u) == 0x1EE04000u) {
            /* 1-source: opcode in 20:15 */
            uint32_t opc = BITS(w, 20, 15);
            if (opc == 0) { /* FMOV Hd, Hn */
                if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                f_isolate_h(&f.e, d, n);
                return f16_end(&f, shared, ret_pc);
            }
            if (opc == 1) { /* FABS Hd, Hn */
                if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, f_dup_hs(d, n, 0));
                put(&f.e, 0x2F04B400u | d); /* bic vd.4h, #0x80, lsl #8 */
                return f16_end(&f, shared, ret_pc);
            }
            if (opc == 2) { /* FNEG Hd, Hn: raw bits */
                if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 1) < 0)
                    return -1;
                uint32_t g = f.g[0];
                put(&f.e, 0x1E260000u | (n << 5) | g); /* fmov wg, sn */
                put(&f.e, 0x52110000u | (g << 5) | g); /* eor #0x8000 */
                put(&f.e, 0x12003C00u | (g << 5) | g); /* and #0xffff */
                put(&f.e, 0x1E270000u | (g << 5) | d); /* fmov sd, wg */
                return f16_end(&f, shared, ret_pc);
            }
            if (opc == 3 || (opc >= 8 && opc <= 15 && opc != 13)) { /* FSQRT, FRINT* */
                if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, f_cvt_s_h(d, n));
                put(&f.e, (w & ~0x00C003FFu) | (d << 5) | d);
                put(&f.e, f_cvt_h_s(d, d));
                return f16_end(&f, shared, ret_pc);
            }
            return -1;
        }
        if ((w & 0xFFE03C00u) == 0x1EE02000u) {
            /* FCMP/FCMPE Hn, Hm|#0 */
            bool zero = BIT(w, 3);
            if (f16_begin(&f, out, max, stub_pc, vav, zero ? 1 : 2, dead, vdead, 0, 0) < 0)
                return -1;
            put(&f.e, f_cvt_s_h(f.v[0], n));
            if (!zero)
                put(&f.e, f_cvt_s_h(f.v[1], m));
            put(&f.e, (w & ~0x00C003E0u & ~0x001F0000u) | (f.v[0] << 5) | (zero ? 0 : f.v[1] << 16));
            return f16_end(&f, shared, ret_pc);
        }
        if ((w & 0xFFE00C00u) == 0x1EE00400u) {
            /* FCCMP/FCCMPE */
            if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
                return -1;
            put(&f.e, f_cvt_s_h(f.v[0], n));
            put(&f.e, f_cvt_s_h(f.v[1], m));
            put(&f.e, (w & ~0x00C003E0u & ~0x001F0000u) | (f.v[0] << 5) | (f.v[1] << 16));
            return f16_end(&f, shared, ret_pc);
        }
        if ((w & 0xFFE00C00u) == 0x1EE00C00u) {
            /* FCSEL Hd, Hn, Hm, cond: select the raw single, then keep only its low 16 bits */
            if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                return -1;
            put(&f.e, w & ~0x00C00000u);
            f_isolate_h(&f.e, d, d);
            return f16_end(&f, shared, ret_pc);
        }
        if ((w & 0xFFE01FE0u) == 0x1EE01000u) {
            /* FMOV Hd, #imm8 */
            uint32_t imm8 = BITS(w, 20, 13);
            uint32_t b6 = BIT(imm8, 6);
            uint32_t bits = (BIT(imm8, 7) << 15) | ((b6 ^ 1) << 14) | ((b6 ? 3u : 0u) << 12) |
                            (BITS(imm8, 5, 4) << 10) | (BITS(imm8, 3, 0) << 6);
            uint32_t lo = bits & 0xFF, hi = bits >> 8;
            if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                return -1;
            if (lo) {
                put(&f.e, 0x0F000400u | (BITS(lo, 7, 5) << 16) | (BITS(lo, 4, 0) << 5) | d); /* movi vd.2s, #lo */
                if (hi)
                    put(&f.e,
                        0x0F003400u | (BITS(hi, 7, 5) << 16) | (BITS(hi, 4, 0) << 5) | d); /* orr vd.2s, #hi, lsl #8 */
            } else {
                put(&f.e,
                    0x0F002400u | (BITS(hi, 7, 5) << 16) | (BITS(hi, 4, 0) << 5) | d); /* movi vd.2s, #hi, lsl #8 */
            }
            put(&f.e, f_fmov_ss(d, d));
            return f16_end(&f, shared, ret_pc);
        }
        if ((w & 0xFFE00C00u) == 0x1EE00800u) {
            /* 2-source: FMUL FDIV FADD FSUB FMAX FMIN FMAXNM FMINNM FNMUL */
            uint32_t opc = BITS(w, 15, 12);
            if (opc > 8)
                return -1;
            if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
                return -1;
            put(&f.e, f_cvt_s_h(f.v[0], n));
            put(&f.e, f_cvt_s_h(f.v[1], m));
            put(&f.e, f_regs(w & ~0x00C00000u, f.v[0], f.v[0], f.v[1]));
            put(&f.e, f_cvt_h_s(d, f.v[0]));
            return f16_end(&f, shared, ret_pc);
        }
        return -1;
    conversion:
        {
            /* int <-> half (bit 21 set, 15:10 zero) and fixed point (bit 21 clear) */
            bool fixed = !BIT(w, 21);
            uint32_t rmode = BITS(w, 20, 19), opc = BITS(w, 18, 16);
            bool sf = BIT(w, 31);
            if (!fixed && rmode == 0 && opc == 6) { /* FMOV Wd/Xd, Hn */
                if (f16_begin(&f, out, max, stub_pc, 0, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, 0x0E023C00u | (n << 5) | d); /* umov wd, vn.h[0] */
                return f16_end(&f, shared, ret_pc);
            }
            if (!fixed && rmode == 0 && opc == 7) { /* FMOV Hd, Wn/Xn */
                if (f16_begin(&f, out, max, stub_pc, 0, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, 0x1E270000u | (n << 5) | d); /* fmov sd, wn */
                f_isolate_h(&f.e, d, d);
                return f16_end(&f, shared, ret_pc);
            }
            if ((!fixed && rmode == 0 && (opc == 2 || opc == 3)) || (fixed && rmode == 0 && (opc == 2 || opc == 3))) {
                /* SCVTF/UCVTF Hd, Wn/Xn{, #fbits}: in double (exact for 32-bit, harmless for 64-bit), one rounding */
                if (f16_begin(&f, out, max, stub_pc, 0, 0, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, (w & ~0x00C00000u) | 0x00400000u); /* same, ftype = double, into Dd */
                put(&f.e, f_cvt_h_d(d, d));
                return f16_end(&f, shared, ret_pc);
            }
            if ((!fixed && (opc == 0 || opc == 1 || opc == 4 || opc == 5)) || (fixed && rmode == 3 && opc <= 1)) {
                /* FCVT{N,P,M,Z,A}{S,U} Wd/Xd, Hn{, #fbits}: exact widening, convert from single */
                if (f16_begin(&f, out, max, stub_pc, 1u << n, 1, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, f_cvt_s_h(f.v[0], n));
                put(&f.e, (w & ~0x00C003E0u) | (f.v[0] << 5));
                return f16_end(&f, shared, ret_pc);
            }
            (void)sf;
            return -1;
        }
    }

    /* Advanced SIMD */
    /* three same FP16, vector (0 Q U 01110 a 10 Rm 00 opc 1) and scalar (01 U 11110 a 10 Rm 00 opc 1) */
    if ((w & 0x9F60C400u) == 0x0E400400u || (w & 0xDF60C400u) == 0x5E400400u) {
        bool scalar = BIT(w, 28);
        uint32_t u = BIT(w, 29), sa = BIT(w, 23), opc = BITS(w, 13, 11);
        uint32_t key = (u << 4) | (sa << 3) | opc;
        uint32_t s4 = f_three_to_s(w) | (1u << 30);                            /* .4s (scalar: S form) */
        bool fused = key == 0x01 || key == 0x09 || key == 0x07 || key == 0x0F; /* FMLA FMLS FRECPS FRSQRTS */
        bool cmp = key == 0x04 || key == 0x14 || key == 0x15 || key == 0x1C || key == 0x1D;
        bool pair = !scalar && (key == 0x10 || key == 0x12 || key == 0x16 || key == 0x18 || key == 0x1E);
        if (scalar) {
            if (fused && (key == 0x01 || key == 0x09))
                return -1;
            if (fused) {
                /* FRECPS/FRSQRTS Hd, Hn, Hm in double */
                if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
                    return -1;
                put(&f.e, f_cvt_d_h(f.v[0], n));
                put(&f.e, f_cvt_d_h(f.v[1], m));
                put(&f.e, f_regs(f_three_to_s(w) | (1u << 22), f.v[0], f.v[0], f.v[1]));
                put(&f.e, f_cvt_h_d(d, f.v[0]));
                return f16_end(&f, shared, ret_pc);
            }
            if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
                return -1;
            put(&f.e, f_cvt_s_h(f.v[0], n));
            put(&f.e, f_cvt_s_h(f.v[1], m));
            put(&f.e, f_regs(f_three_to_s(w), f.v[0], f.v[0], f.v[1]));
            if (cmp)
                put(&f.e, f_xtn(false, d, f.v[0]));
            else
                put(&f.e, f_cvt_h_s(d, f.v[0]));
            return f16_end(&f, shared, ret_pc);
        }
        if (fused) {
            bool acc = key == 0x01 || key == 0x09;
            int nv = 3 + (acc ? 1 : 0) + (q ? 1 : 0);
            if (f16_begin(&f, out, max, stub_pc, vav, nv, dead, vdead, 0, 0) < 0)
                return -1;
            f16_fused(&f, q, f_three_to_s(w) | (1u << 30) | (1u << 22), acc, -1, d, n, m);
            return f16_end(&f, shared, ret_pc);
        }
        if (pair) {
            if (f16_begin(&f, out, max, stub_pc, vav, 3, dead, vdead, 0, 0) < 0)
                return -1;
            f16_pair(&f, q, s4, d, n, m);
            return f16_end(&f, shared, ret_pc);
        }
        if (f16_begin(&f, out, max, stub_pc, vav, q ? 3 : 2, dead, vdead, 0, 0) < 0)
            return -1;
        f16_vec2(&f, q, s4, cmp ? NARROW_XTN : NARROW_FCVTN, d, n, m);
        return f16_end(&f, shared, ret_pc);
    }
    /* two-register misc FP16, vector (0 Q U 01110 a 1111 00 opc 10) and scalar (01 U 11110 a 1111 00 opc 10) */
    if ((w & 0x9F7E0C00u) == 0x0E780800u || (w & 0xDF7E0C00u) == 0x5E780800u) {
        bool scalar = BIT(w, 28);
        uint32_t u = BIT(w, 29), sa = BIT(w, 23), opc = BITS(w, 16, 12);
        uint32_t s4 = f_misc_to_s(w) | (scalar ? 0 : (1u << 30));
        bool cmpz = sa && opc >= 0x0C && opc <= 0x0E;           /* FCMGT/FCMEQ/FCMLT/FCMGE/FCMLE #0 */
        bool toint = opc == 0x1A || opc == 0x1B || opc == 0x1C; /* FCVT{N,M,A / P,Z}{S,U} */
        bool fromint = !sa && opc == 0x1D;                      /* SCVTF/UCVTF */
        bool absneg = sa && opc == 0x0F;                        /* FABS / FNEG */
        if (absneg && !scalar) {
            /* raw bits: FABS = BIC #0x8000 per lane; FNEG = EOR with a 0x8000 pattern */
            if (f16_begin(&f, out, max, stub_pc, vav, u ? 1 : 0, dead, vdead, 0, 0) < 0)
                return -1;
            if (!u) {
                if (d != n)
                    put(&f.e, (q ? 0x4EA01C00u : 0x0EA01C00u) | (n << 16) | (n << 5) | d);
                put(&f.e, (q ? 0x6F00B400u : 0x2F00B400u) | (4u << 16) | d); /* bic vd.8h, #0x80, lsl #8 */
            } else {
                put(&f.e, 0x4F04A400u | f.v[0]); /* movi vt.8h, #0x80, lsl #8 */
                put(&f.e, (q ? 0x6E201C00u : 0x2E201C00u) | (f.v[0] << 16) | (n << 5) | d); /* eor */
            }
            return f16_end(&f, shared, ret_pc);
        }
        if (scalar) {
            if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
                return -1;
            if (fromint) {
                put(&f.e, f_sxtl(false, u, d, n));
                put(&f.e, f_regs1(f_misc_to_s(w), d, d));
                put(&f.e, f_cvt_h_s(d, d));
            } else {
                put(&f.e, f_cvt_s_h(d, n));
                put(&f.e, f_regs1(f_misc_to_s(w), d, d));
                if (cmpz)
                    put(&f.e, f_xtn(false, d, d));
                else if (toint)
                    put(&f.e, (u ? 0x7E614800u : 0x5E614800u) | (d << 5) | d); /* uqxtn/sqxtn hd, sd */
                else
                    put(&f.e, f_cvt_h_s(d, d));
            }
            return f16_end(&f, shared, ret_pc);
        }
        if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
            return -1;
        f16_vec1(&f, q, s4, fromint ? (u ? 2 : 1) : 0,
                 cmpz    ? NARROW_XTN
                 : toint ? (u ? NARROW_UQXTN : NARROW_SQXTN)
                         : NARROW_FCVTN,
                 d, n);
        return f16_end(&f, shared, ret_pc);
    }
    /* across lanes FP16: 0 Q 0 01110 a 0 11000 opc 10 */
    if ((w & 0xBF7E0C00u) == 0x0E300800u) {
        uint32_t opc = BITS(w, 16, 12), sa = BIT(w, 23);
        if (opc != 0x0C && opc != 0x0F)
            return -1;
        uint32_t v4s = w | (1u << 29) | (1u << 30);
        /* scalar 2-source counterpart: FMAXNM/FMINNM (opc 0C) or FMAX/FMIN (0F) */
        uint32_t ops = 0x1E200800u | ((opc == 0x0C ? 6u : 4u) + sa) << 12;
        if (f16_begin(&f, out, max, stub_pc, vav, 2, dead, vdead, 0, 0) < 0)
            return -1;
        f16_across(&f, q, v4s, ops, d, n);
        return f16_end(&f, shared, ret_pc);
    }
    /* scalar pairwise FP16: 01 0 11110 a 0 11000 opc 10 (on .2h) */
    if ((w & 0xFF7E0C00u) == 0x5E300800u) {
        if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
            return -1;
        put(&f.e, f_fcvtl(false, d, n));
        put(&f.e, f_regs1(w | (1u << 29), d, d));
        put(&f.e, f_cvt_h_s(d, d));
        return f16_end(&f, shared, ret_pc);
    }
    /* by element FP16, vector (0 Q U 01111 00 L M Rm opc H 0) and scalar (01 U 11111 00 L M Rm opc H 0) */
    if ((w & 0x9FC00400u) == 0x0F000000u || (w & 0xDFC00400u) == 0x5F000000u) {
        bool scalar = BIT(w, 28);
        uint32_t u = BIT(w, 29), opc = BITS(w, 15, 12);
        uint32_t idx = (BIT(w, 11) << 2) | (BIT(w, 21) << 1) | BIT(w, 20), rm = BITS(w, 19, 16);
        bool fmla = !u && opc == 1, fmls = !u && opc == 5, fmul = opc == 9;
        if (!fmla && !fmls && !fmul)
            return -1;
        vav = (1u << d) | (1u << n) | (1u << rm);
        if (scalar) {
            if (f16_begin(&f, out, max, stub_pc, vav, 3, dead, vdead, 0, 0) < 0)
                return -1;
            uint32_t x = f.v[0], y = f.v[1], z = f.v[2];
            put(&f.e, f_dup_hs(y, rm, idx));
            if (fmul) {
                put(&f.e, f_cvt_s_h(x, n));
                put(&f.e, f_cvt_s_h(y, y));
                put(&f.e, u ? 0x5E20DC00u | (y << 16) | (x << 5) | x
                            : 0x1E200800u | (y << 16) | (x << 5) | x); /* fmulx / fmul s */
                put(&f.e, f_cvt_h_s(d, x));
            } else {
                put(&f.e, f_cvt_d_h(x, n));
                put(&f.e, f_cvt_d_h(y, y));
                put(&f.e, f_cvt_d_h(z, d));
                put(&f.e,
                    (fmla ? 0x1F400000u : 0x1F408000u) | (y << 16) | (z << 10) | (x << 5) | x); /* fmadd/fmsub d */
                put(&f.e, f_cvt_h_d(d, x));
            }
            return f16_end(&f, shared, ret_pc);
        }
        if (fmul) {
            if (f16_begin(&f, out, max, stub_pc, vav, 4, dead, vdead, 0, 0) < 0)
                return -1;
            uint32_t t = f.v[3];
            put(&f.e, f_dup_h(t, rm, idx));
            /* by element: U=0 is FMUL, U=1 FMULX; their .4s vector forms are 0x6E20DC00 (FMUL) and 0x4E20DC00 (FMULX) */
            f16_vec2(&f, q, u ? 0x4E20DC00u : 0x6E20DC00u, NARROW_FCVTN, d, n, t);
            return f16_end(&f, shared, ret_pc);
        }
        if (f16_begin(&f, out, max, stub_pc, vav, q ? 5 : 4, dead, vdead, 0, 0) < 0)
            return -1;
        f16_fused(&f, q, fmla ? 0x4E60CC00u : 0x4EE0CC00u, true, (int)idx, d, n, rm);
        return f16_end(&f, shared, ret_pc);
    }
    /* FMOV Vd.4h/8h, #imm (0 Q 0 0111100000 abc 1111 1 1 defgh) */
    if ((w & 0x9FF8FC00u) == 0x0F00FC00u) {
        uint32_t imm8 = (BITS(w, 18, 16) << 5) | BITS(w, 9, 5);
        uint32_t b6 = BIT(imm8, 6);
        uint32_t bits = (BIT(imm8, 7) << 15) | ((b6 ^ 1) << 14) | ((b6 ? 3u : 0u) << 12) | (BITS(imm8, 5, 4) << 10) |
                        (BITS(imm8, 3, 0) << 6);
        uint32_t lo = bits & 0xFF, hi = bits >> 8;
        if (f16_begin(&f, out, max, stub_pc, vav, 0, dead, vdead, 0, 0) < 0)
            return -1;
        uint32_t qq = (uint32_t)q << 30;
        if (lo) {
            put(&f.e, 0x0F008400u | qq | (BITS(lo, 7, 5) << 16) | (BITS(lo, 4, 0) << 5) | d); /* movi vd.8h, #lo */
            if (hi)
                put(&f.e,
                    0x0F00B400u | qq | (BITS(hi, 7, 5) << 16) | (BITS(hi, 4, 0) << 5) | d); /* orr vd.8h, #hi, lsl #8 */
        } else {
            put(&f.e, 0x0F00A400u | qq | (BITS(hi, 7, 5) << 16) | (BITS(hi, 4, 0) << 5) | d); /* movi vd.8h, #hi, lsl #8 */
        }
        return f16_end(&f, shared, ret_pc);
    }
    return -1;
}

/*
 * SQRDMLAH / SQRDMLSH (FEAT_RDM): Vd = sat((Vd << e) +/- 2*Vn*Vm + 2^(e-1)) >> e). Computed as
 * (Vd << (e-1)) +/- Vn*Vm in 2e-bit lanes, which cannot overflow (|Vd << (e-1)| <= 2^(2e-2), |Vn*Vm| <= 2^(2e-2)),
 * then one SQRSHRN #(e-1): rounding, shift and saturation exactly as the original (QC included).
 */
static int stub_rdm(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t vdead, bool shared,
                    uint32_t *out, int max)
{
    bool scalar = BIT(w, 28), byelem = BIT(w, 24), q = BIT(w, 30), sub;
    uint32_t size = BITS(w, 23, 22), d = BITS(w, 4, 0), n = BITS(w, 9, 5), m = BITS(w, 20, 16);
    int idx = -1;
    if (size != 1 && size != 2)
        return -1;
    if (byelem) {
        sub = BIT(w, 13);
        if (size == 1) {
            idx = (int)((BIT(w, 11) << 2) | (BIT(w, 21) << 1) | BIT(w, 20));
            m = BITS(w, 19, 16);
        } else {
            idx = (int)((BIT(w, 11) << 1) | BIT(w, 21));
        }
    } else {
        sub = BIT(w, 11);
    }
    uint32_t e = size == 1 ? 16 : 32, wide = size + 1;
    struct f16 f;
    int nv = 2 + (idx >= 0 ? 1 : 0) + ((q && !scalar) ? 1 : 0);
    if (f16_begin(&f, out, max, stub_pc, (1u << d) | (1u << n) | (1u << m), nv, dead, vdead, 0, 0) < 0)
        return -1;
    int vk = 0;
    uint32_t P = f.v[vk++], A = f.v[vk++];
    uint32_t T = idx >= 0 ? f.v[vk++] : 0;
    uint32_t R = (q && !scalar) ? f.v[vk++] : 0;
    uint32_t mm = m;
    if (idx >= 0) {
        uint32_t imm5 = size == 1 ? (((uint32_t)idx << 2) | 2u) : (((uint32_t)idx << 3) | 4u);
        put(&f.e, 0x4E000400u | (imm5 << 16) | (m << 5) | T); /* dup vt, vm.<T>[idx] */
        mm = T;
    }
    uint32_t shl = 0x0F00A400u | ((e + e - 1) << 16);                       /* sshll #(e-1): immh:immb = e + (e-1) */
    uint32_t shrn = (scalar ? 0x5F009C00u : 0x0F009C00u) | ((e + 1) << 16); /* sqrshrn #(e-1): immh:immb = 2e - (e-1) */
    uint32_t addsub = (sub ? 0x6E208400u : 0x4E208400u) | (wide << 22);
    for (int half = (q && !scalar) ? 1 : 0; half >= 0; half--) {
        uint32_t hi = (uint32_t)half << 30;
        put(&f.e, 0x0E20C000u | hi | (size << 22) | (mm << 16) | (n << 5) | P); /* smull{2} p, n, m */
        put(&f.e, shl | hi | (d << 5) | A);                                     /* sshll{2} a, d, #(e-1) */
        put(&f.e, addsub | (P << 16) | (A << 5) | A);                           /* a = a +/- p */
        put(&f.e, shrn | (A << 5) | (half ? R : d));
    }
    if (q && !scalar) {
        /* d now holds the low half (upper zeroed); R's low half holds the high results: move them up */
        put(&f.e, 0x6E180400u | (0u << 11) | (R << 5) | d); /* ins vd.d[1], vr.d[0] */
    }
    return f16_end(&f, shared, ret_pc);
}
