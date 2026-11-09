/*
 * a64x_rewrite.c: turn A12 (ARMv8.3) instructions into ARMv8.0 code.
 *
 *  - a64x_rewrite1(): the 4-byte-for-4-byte rewrites. Pointer authentication is the identity (user's rule):
 *    nothing is ever signed, every authentication succeeds, PACGA yields 0.
 *  - a64x_stub(): out-of-line ARMv8.0 code for an instruction that needs more than one word (LSE atomics, far
 *    LDRA). The site becomes `B stub`; the stub ends with `B site+4`. The stub is the sequence LLVM emits for the
 *    same C11 operation on ARMv8.0 (LDAXR/STLXR loops), so memory ordering is at least as strong as the original.
 *  - a64x_dead_regs(): which X registers are provably dead at a site (written before read on the straight-line
 *    path after it), so a stub can use them instead of spilling.
 *
 * Darwin arm64 lets leaf code use 128 bytes below SP (red zone), so a stub that must spill first moves SP down by
 * 128 + 16*n and stores above that.
 */
#include "a64x.h"

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(w, n)       (((w) >> (n)) & 1u)

#define NOP 0xD503201Fu
#define RET 0xD65F03C0u

static inline uint32_t enc_b(uint64_t from, uint64_t to)
{
    return 0x14000000u | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3FFFFFF);
}

static inline uint32_t enc_cbnz(bool x, uint32_t rt, int64_t delta)
{
    return (x ? 0xB5000000u : 0x35000000u) | (((uint32_t)(delta >> 2) & 0x7FFFF) << 5) | rt;
}

/* MOV (register) = ORR Rd, ZR, Rm */
static inline uint32_t enc_mov(bool x, uint32_t rd, uint32_t rm)
{
    return (x ? 0xAA0003E0u : 0x2A0003E0u) | (rm << 16) | rd;
}

/* ADD/SUB (immediate), 12-bit, optional LSL 12; Rd/Rn may be SP */
static inline uint32_t enc_addi(bool x, uint32_t rd, uint32_t rn, uint32_t imm12, bool lsl12)
{
    return (x ? 0x91000000u : 0x11000000u) | (lsl12 ? 1u << 22 : 0) | (imm12 << 10) | (rn << 5) | rd;
}

static inline uint32_t enc_subi(bool x, uint32_t rd, uint32_t rn, uint32_t imm12, bool lsl12)
{
    return (x ? 0xD1000000u : 0x51000000u) | (lsl12 ? 1u << 22 : 0) | (imm12 << 10) | (rn << 5) | rd;
}

/* logical (shifted register), shift 0: AND/ORR/EOR/BIC */
static inline uint32_t enc_log(bool x, uint32_t opc_n, uint32_t rd, uint32_t rn, uint32_t rm)
{
    return (x ? 0x80000000u : 0) | opc_n | (rm << 16) | (rn << 5) | rd;
}

#define LOG_AND 0x0A000000u
#define LOG_BIC 0x0A200000u
#define LOG_ORR 0x2A000000u
#define LOG_EOR 0x4A000000u

static inline uint32_t enc_add(bool x, uint32_t rd, uint32_t rn, uint32_t rm)
{
    return (x ? 0x8B000000u : 0x0B000000u) | (rm << 16) | (rn << 5) | rd;
}

/* CMP (shifted reg) = SUBS ZR, Rn, Rm */
static inline uint32_t enc_cmp(bool x, uint32_t rn, uint32_t rm)
{
    return (x ? 0xEB00001Fu : 0x6B00001Fu) | (rm << 16) | (rn << 5);
}

/* CSEL Rd, Rn, Rm, cond */
static inline uint32_t enc_csel(bool x, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t cond)
{
    return (x ? 0x9A800000u : 0x1A800000u) | (rm << 16) | (cond << 12) | (rn << 5) | rd;
}

/* SXTB/SXTH/UXTB/UXTH (32-bit): SBFM/UBFM Wd, Wn, #0, #7/#15 */
static inline uint32_t enc_sxt(uint32_t size, uint32_t rd, uint32_t rn)
{
    return 0x13000000u | ((size ? 15u : 7u) << 10) | (rn << 5) | rd;
}

static inline uint32_t enc_uxt(uint32_t size, uint32_t rd, uint32_t rn)
{
    return 0x53000000u | ((size ? 15u : 7u) << 10) | (rn << 5) | rd;
}

/* AND Wd, Wn, #0xff / #0xffff (logical immediate, 32-bit: N=0, immr=0, imms=7/15) */
static inline uint32_t enc_and_mask(uint32_t size, uint32_t rd, uint32_t rn)
{
    return 0x12000000u | ((size ? 15u : 7u) << 10) | (rn << 5) | rd;
}

/* exclusive load/store, size 0..3 */
static inline uint32_t enc_ldxr(uint32_t size, bool acq, uint32_t rt, uint32_t rn)
{
    return (size << 30) | 0x085F7C00u | (acq ? 1u << 15 : 0) | (rn << 5) | rt;
}

static inline uint32_t enc_stxr(uint32_t size, bool rel, uint32_t rs, uint32_t rt, uint32_t rn)
{
    return (size << 30) | 0x08007C00u | (rel ? 1u << 15 : 0) | (rs << 16) | (rn << 5) | rt;
}

static inline uint32_t enc_ldxp(bool x, bool acq, uint32_t rt, uint32_t rt2, uint32_t rn)
{
    return (x ? 0xC87F0000u : 0x887F0000u) | (acq ? 1u << 15 : 0) | (rt2 << 10) | (rn << 5) | rt;
}

static inline uint32_t enc_stxp(bool x, bool rel, uint32_t rs, uint32_t rt, uint32_t rt2, uint32_t rn)
{
    return (x ? 0xC8200000u : 0x88200000u) | (rel ? 1u << 15 : 0) | (rs << 16) | (rt2 << 10) | (rn << 5) | rt;
}

#define CLREX 0xD503305Fu

/* STP/LDP Xt1, Xt2, [SP, #imm] (signed offset, scaled by 8) */
static inline uint32_t enc_stp_sp(uint32_t t1, uint32_t t2, int32_t off)
{
    return 0xA9000000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (31u << 5) | t1;
}

static inline uint32_t enc_ldp_sp(uint32_t t1, uint32_t t2, int32_t off)
{
    return 0xA9400000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (31u << 5) | t1;
}

static inline uint32_t enc_str_sp(uint32_t t, int32_t off)
{
    return 0xF9000000u | (((uint32_t)off / 8) << 10) | (31u << 5) | t;
}

static inline uint32_t enc_ldr_sp(uint32_t t, int32_t off)
{
    return 0xF9400000u | (((uint32_t)off / 8) << 10) | (31u << 5) | t;
}

/* MRS Xt, NZCV / MSR NZCV, Xt */
static inline uint32_t enc_mrs_nzcv(uint32_t t)
{
    return 0xD53B4200u | t;
}

static inline uint32_t enc_msr_nzcv(uint32_t t)
{
    return 0xD51B4200u | t;
}

#define COND_GT 0xC
#define COND_LT 0xB
#define COND_HI 0x8
#define COND_LO 0x3

/*
 * Kernel mode: PSTATE features the A57 does not have. With none of them implemented, the state they control is fixed
 * at what the A57 does: no PAN/UAO (the kernel is built without PAN), no DIT (the A57's integer and SIMD timing is
 * what it is; corecrypto only sets DIT as a hint), no SSBS control, no MTE tag checks. Reading one gives 0.
 */
bool a64x_rewrite1_kernel(uint32_t w, uint32_t *out)
{
    if ((w & 0xFFF8F01Fu) == 0xD500401Fu) { /* MSR (immediate) */
        uint32_t op1 = BITS(w, 18, 16), op2 = BITS(w, 7, 5);
        if ((op1 == 0 && (op2 == 3 || op2 == 4)) || (op1 == 3 && (op2 == 1 || op2 == 2 || op2 == 4))) {
            *out = NOP; /* UAO, PAN, SSBS, DIT, TCO */
            return true;
        }
        return false;
    }
    if ((w & 0xFFD00000u) == 0xD5100000u) { /* MRS/MSR (register) */
        uint32_t op0 = 2 + BIT(w, 19), op1 = BITS(w, 18, 16), crn = BITS(w, 15, 12), crm = BITS(w, 11, 8);
        uint32_t op2 = BITS(w, 7, 5), rt = BITS(w, 4, 0);
        bool hit = op0 == 3 && crn == 4 && crm == 2 && ((op1 == 0 && (op2 == 3 || op2 == 4)) || (op1 == 3 && op2 >= 5));
        if (!hit) {
            return false;
        }
        *out = BIT(w, 21) && rt != 31 ? 0xD2800000u | rt : NOP; /* MRS -> MOVZ Xt, #0; MSR -> NOP */
        return true;
    }
    return false;
}

bool a64x_rewrite1(uint32_t w, uint32_t *out)
{
    enum a64x_class c = a64x_classify(w);
    uint32_t rn = BITS(w, 9, 5), rt = BITS(w, 4, 0);
    switch (c) {
    case A64X_PAC_BR:
        *out = 0xD61F0000u | (rn << 5); /* BR Xn */
        return true;
    case A64X_PAC_BLR:
        *out = 0xD63F0000u | (rn << 5); /* BLR Xn */
        return true;
    case A64X_PAC_RET:
        *out = RET;
        return true;
    case A64X_PAC_DP:   /* PAC*: signing is the identity; AUT*: authentication always succeeds */
    case A64X_PAC_XPAC: /* nothing is signed, so nothing to strip */
        *out = NOP;
        return true;
    case A64X_PAC_PACGA:
        *out = 0xAA1F03E0u | rt; /* MOV Xd, XZR: PACGA yields 0 */
        return true;
    case A64X_PAC_LDRA: {
        /* LDRAA/LDRAB Xt, [Xn, #simm10*8]{!}: the address is Xn + offset, unauthenticated */
        int32_t imm = (int32_t)((BIT(w, 22) << 9) | BITS(w, 20, 12));
        if (imm & 0x200) {
            imm -= 0x400;
        }
        imm *= 8;
        if (BIT(w, 11)) {
            if (imm >= -256 && imm <= 255) {
                *out = 0xF8400C00u | (((uint32_t)imm & 0x1FF) << 12) | (rn << 5) | rt; /* LDR Xt, [Xn, #imm]! */
                return true;
            }
            return false;
        }
        if (imm >= 0) {
            *out = 0xF9400000u | ((uint32_t)(imm / 8) << 10) | (rn << 5) | rt; /* LDR Xt, [Xn, #imm] */
            return true;
        }
        if (imm >= -256) {
            *out = 0xF8400000u | (((uint32_t)imm & 0x1FF) << 12) | (rn << 5) | rt; /* LDUR Xt, [Xn, #imm] */
            return true;
        }
        return false;
    }
    case A64X_RCPC_LDAPR:
        /* LDAPR{B,H} -> LDAR{B,H}: RCpc acquire -> RCsc acquire (stronger) */
        *out = (w & 0xC00003FFu) | 0x08DFFC00u;
        return true;
    case A64X_LOR:
        /* LDLAR -> LDAR, STLLR -> STLR (LORegion ordering -> full acquire/release) */
        *out = w | (1u << 15);
        return true;
    case A64X_DPB:
        *out = 0xD50B7A20u | rt; /* DC CVAP -> DC CVAC */
        return true;
    default:
        return false;
    }
}

#define XMASK(r) ((r) == 31 ? 0u : (1u << (r)))

/*
 * GPRs an instruction reads and writes (x0..x30; SP/XZR ignored). Returns false for anything not decoded here, which
 * ends a liveness scan. Only integer classes whose operands are certain are decoded.
 */
bool a64x_regs(uint32_t w, uint32_t *rd_mask, uint32_t *wr_mask)
{
    uint32_t rd = BITS(w, 4, 0), rn = BITS(w, 9, 5), rm = BITS(w, 20, 16), ra = BITS(w, 14, 10);
    *rd_mask = *wr_mask = 0;
    /* ADR/ADRP */
    if ((w & 0x1F000000u) == 0x10000000u) {
        *wr_mask = XMASK(rd);
        return true;
    }
    /* ADD/SUB immediate (Rd may be SP when S=0, Rn may be SP) */
    if ((w & 0x1F800000u) == 0x11000000u) {
        *rd_mask = XMASK(rn);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* logical immediate (Rd may be SP when not ANDS) */
    if ((w & 0x1F800000u) == 0x12000000u) {
        *rd_mask = XMASK(rn);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* MOVN/MOVZ/MOVK */
    if ((w & 0x1F800000u) == 0x12800000u) {
        uint32_t opc = BITS(w, 30, 29);
        if (opc == 1) {
            return false;
        }
        if (opc == 3) {
            *rd_mask = XMASK(rd); /* MOVK keeps the other bits */
        }
        *wr_mask = XMASK(rd);
        return true;
    }
    /* SBFM/UBFM (BFM reads Rd) */
    if ((w & 0x1F800000u) == 0x13000000u) {
        uint32_t opc = BITS(w, 30, 29);
        if (opc == 3) {
            return false;
        }
        *rd_mask = XMASK(rn) | (opc == 1 ? XMASK(rd) : 0);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* logical shifted register; add/sub shifted; add/sub extended (Rn may be SP) */
    if ((w & 0x1F000000u) == 0x0A000000u || (w & 0x1F200000u) == 0x0B000000u || (w & 0x1F200000u) == 0x0B200000u) {
        *rd_mask = XMASK(rn) | XMASK(rm);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* ADC/SBC, CSEL/CSINC/CSINV/CSNEG, data-processing 2 source, 3 source */
    if ((w & 0x1FE00000u) == 0x1A000000u || (w & 0x1FE00000u) == 0x1A800000u || (w & 0x5FE00000u) == 0x1AC00000u) {
        *rd_mask = XMASK(rn) | XMASK(rm);
        *wr_mask = XMASK(rd);
        return true;
    }
    if ((w & 0x1F000000u) == 0x1B000000u) {
        *rd_mask = XMASK(rn) | XMASK(rm) | XMASK(ra);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* data-processing 1 source (RBIT, REV, CLZ...), not the PAC ones */
    if ((w & 0x5FE00000u) == 0x5AC00000u && BITS(w, 20, 16) == 0) {
        *rd_mask = XMASK(rn);
        *wr_mask = XMASK(rd);
        return true;
    }
    /* CCMP/CCMN register/immediate: read only */
    if ((w & 0x1FE00000u) == 0x1A400000u) {
        *rd_mask = XMASK(rn) | (BIT(w, 11) ? 0 : XMASK(rm));
        return true;
    }
    /* load/store register (unsigned imm, unscaled/pre/post, register offset), integer only */
    if ((w & 0x3B000000u) == 0x39000000u || (w & 0x3B200000u) == 0x38000000u || (w & 0x3B200C00u) == 0x38200800u) {
        if (BIT(w, 26)) {
            /* FP/SIMD register: only the base (and index) are GPRs */
            *rd_mask = XMASK(rn) | (((w & 0x3B200C00u) == 0x38200800u) ? XMASK(rm) : 0);
            if ((w & 0x3B000000u) == 0x38000000u && (BITS(w, 11, 10) == 1 || BITS(w, 11, 10) == 3)) {
                *wr_mask = XMASK(rn);
            }
            return true;
        }
        uint32_t opc = BITS(w, 23, 22), size = BITS(w, 31, 30);
        bool load = opc != 0;
        if (size == 3 && opc == 2) {
            return false; /* PRFM: no register written, but keep it simple */
        }
        *rd_mask = XMASK(rn);
        if ((w & 0x3B200C00u) == 0x38200800u) {
            *rd_mask |= XMASK(rm);
        }
        if ((w & 0x3B000000u) == 0x38000000u && (BITS(w, 11, 10) == 1 || BITS(w, 11, 10) == 3)) {
            *wr_mask |= XMASK(rn); /* writeback */
        }
        if (load) {
            *wr_mask |= XMASK(rd);
        } else {
            *rd_mask |= XMASK(rd);
        }
        return true;
    }
    /* LDR (literal), integer */
    if ((w & 0x3F000000u) == 0x18000000u && BITS(w, 31, 30) != 3) {
        *wr_mask = XMASK(rd);
        return true;
    }
    /* LDP/STP (integer, all addressing modes) */
    if ((w & 0x3E000000u) == 0x28000000u && !BIT(w, 26)) {
        uint32_t rt2 = ra;
        bool load = BIT(w, 22);
        *rd_mask = XMASK(rn);
        if (BITS(w, 24, 23) == 1 || BITS(w, 24, 23) == 3) {
            *wr_mask |= XMASK(rn);
        }
        if (load) {
            *wr_mask |= XMASK(rd) | XMASK(rt2);
        } else {
            *rd_mask |= XMASK(rd) | XMASK(rt2);
        }
        return true;
    }
    /* NOP and the PAC hints (identity) */
    if (w == NOP || a64x_is_pac_hint(w)) {
        return true;
    }
    return false;
}

uint32_t a64x_dead_regs(const uint32_t *next, int n)
{
    uint32_t dead = 0, read = 0;
    for (int i = 0; i < n; i++) {
        uint32_t w = next[i], r, wr, c;
        /* calls: x16/x17 can be clobbered by any stub or veneer the call goes through */
        if ((w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u ||
            (a64x_rewrite1(w, &c) && (c & 0xFFFFFC1Fu) == 0xD63F0000u)) {
            dead |= (XMASK(16) | XMASK(17)) & ~read;
            break;
        }
        /* returns: x16/x17 are scratch across the boundary. (BR may be a jump table inside the function: stop.) */
        if (w == RET || (a64x_rewrite1(w, &c) && c == RET)) {
            dead |= (XMASK(16) | XMASK(17)) & ~read;
            break;
        }
        if (!a64x_regs(w, &r, &wr)) {
            break;
        }
        read |= r;
        dead |= wr & ~read;
    }
    return dead & ~XMASK(18) & ~XMASK(29) & ~XMASK(30); /* never x18 (platform), fp, lr */
}

/*
 * Path-sensitive version: follows conditional branches (both ways) and branches inside the window. A register is dead
 * if every path writes it before reading it, or reaches a call/return with it unread (x16/x17 only). A path that
 * meets anything not understood (indirect branch, unknown instruction, leaving the window) makes nothing dead.
 */
struct dfs {
    const uint32_t *w;
    int n, budget, maxdepth;
};

static uint32_t dfs_dead(struct dfs *d, int i, uint32_t read, uint32_t written, int depth)
{
    const uint32_t all = 0x7FFFFFFFu;
    while (i < d->n) {
        if (--d->budget < 0 || depth > d->maxdepth) {
            return 0;
        }
        uint32_t w = d->w[i], c, r, wr;
        bool pacx = a64x_rewrite1(w, &c);
        uint32_t e = pacx ? c : w;
        /* BRK/UDF: this path never continues (a trap): it does not constrain the answer */
        if ((e & 0xFFE0001Fu) == 0xD4200000u || (e & 0xFFFF0000u) == 0) {
            return all;
        }
        if ((e & 0xFC000000u) == 0x94000000u || (e & 0xFFFFFC1Fu) == 0xD63F0000u || e == RET) {
            uint32_t extra = 0;
            if ((e & 0xFFFFFC1Fu) == 0xD63F0000u) {
                read |= XMASK(BITS(e, 9, 5));
            }
            extra = (XMASK(16) | XMASK(17)) & ~read;
            if (e != RET) {
                extra |= XMASK(30) & ~read; /* BL/BLR overwrite the link register */
            } else {
                read |= XMASK(30);
            }
            return (written | extra) & ~(e == RET ? (read & XMASK(30)) : 0) & all;
        }
        /* conditional branches: B.cond, CBZ/CBNZ, TBZ/TBNZ */
        int64_t tgt = -1;
        bool cond = false;
        if ((e & 0xFF000010u) == 0x54000000u) {
            tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
            cond = true;
        } else if ((e & 0x7E000000u) == 0x34000000u) {
            read |= XMASK(BITS(e, 4, 0));
            tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
            cond = true;
        } else if ((e & 0x7E000000u) == 0x36000000u) {
            read |= XMASK(BITS(e, 4, 0));
            tgt = i + ((int32_t)(BITS(e, 18, 5) << 18) >> 18);
            cond = true;
        } else if ((e & 0xFC000000u) == 0x14000000u) {
            tgt = i + ((int32_t)(BITS(e, 25, 0) << 6) >> 6);
            if (tgt < 0 || tgt >= d->n || tgt <= i) {
                return 0; /* out of the window or backwards (loop): unknown */
            }
            i = (int)tgt;
            continue;
        }
        if (cond) {
            if (tgt < 0 || tgt >= d->n || tgt <= i) {
                return 0;
            }
            uint32_t a = dfs_dead(d, i + 1, read, written, depth + 1);
            if (!a) {
                return 0;
            }
            uint32_t b = dfs_dead(d, (int)tgt, read, written, depth + 1);
            return a & b;
        }
        if (!a64x_regs(e, &r, &wr)) {
            /* indirect branches, ERET and exception-generating instructions (SVC reads x0-x8, x16): unknown */
            if ((e & 0xFE1FFC00u) == 0xD61F0000u || (e & 0xFF000000u) == 0xD4000000u ||
                (e & 0xFFFFFFFFu) == 0xD69F03E0u) {
                return 0;
            }
            /*
			 * Anything else falls through: count every register field it has (Rt, Rn, Rt2/Ra, Rs/Rm) as read
			 * and nothing as written. That only ever keeps registers live, so it is safe. CASP also reads the
			 * odd registers of its pairs.
			 */
            uint32_t f0 = BITS(e, 4, 0), f1 = BITS(e, 9, 5), f2 = BITS(e, 14, 10), f3 = BITS(e, 20, 16);
            r = XMASK(f0) | XMASK(f1) | XMASK(f2) | XMASK(f3);
            if ((e & 0xBFA07C00u) == 0x08207C00u) {
                r |= XMASK(f0 + 1 < 31 ? f0 + 1 : 31) | XMASK(f3 + 1 < 31 ? f3 + 1 : 31);
            }
            wr = 0;
        }
        read |= r & ~written;
        written |= wr & ~read;
        i++;
    }
    return 0;
}

uint32_t a64x_dead_regs_window(const uint32_t *window, int n, int site)
{
    return a64x_dead_regs_window_lr(window, n, site) & ~XMASK(30);
}

/* same, but x30 may be reported dead (a site can then BL to a shared stub that returns with RET) */
uint32_t a64x_dead_regs_window_lr(const uint32_t *window, int n, int site)
{
    return a64x_dead_regs_budget(window, n, site, 96, 6);
}

/* same with an explicit search budget (instructions visited) and branch depth: the offline planner looks further */
uint32_t a64x_dead_regs_budget(const uint32_t *window, int n, int site, int budget, int depth)
{
    struct dfs d = {window, n, budget, depth};
    uint32_t dead = dfs_dead(&d, site + 1, 0, 0, 0);
    return dead & ~XMASK(18) & ~XMASK(29);
}

/*
 * Flow-based deadness for one register: x is dead at `site` if no path from site+1 reaches a read of x without first
 * passing an instruction that writes it. Paths are followed through loops (visited set), conditional and in-window
 * unconditional branches. A call (BL/BLR) writes x30 and clobbers x16/x17; RET reads x30 and ends the path for
 * the others; BRK/UDF end the path; anything that leaves the window or is not understood counts as a read.
 */
static bool flow_dead(const uint32_t *w, int n, int site, uint32_t x, int budget, const struct a64x_walk *walk,
                      bool tail_ok)
{
    int *stack = walk->stack, sp = 0;
    uint8_t *visited = walk->visited;
    if (site + 1 >= n) {
        return false;
    }
    stack[sp++] = site + 1;
    for (int i = 0; i < (n + 7) / 8; i++) {
        visited[i] = 0;
    }
    while (sp > 0) {
        int i = stack[--sp];
        while (true) {
            if (i < 0 || i >= n || --budget < 0) {
                return false;
            }
            if (visited[i >> 3] & (1 << (i & 7))) {
                break;
            }
            visited[i >> 3] |= (uint8_t)(1 << (i & 7));
            if (i == site) {
                break; /* back at the site: its own effects are the stub's business; this path adds nothing */
            }
            uint32_t e = w[i], c, r, wr;
            if (a64x_rewrite1(e, &c)) {
                e = c;
            }
            if ((e & 0xFFE0001Fu) == 0xD4200000u || (e & 0xFFFF0000u) == 0) {
                break; /* BRK/UDF: never continues */
            }
            bool call = (e & 0xFC000000u) == 0x94000000u || (e & 0xFFFFFC1Fu) == 0xD63F0000u;
            if (call) {
                if ((e & 0xFFFFFC1Fu) == 0xD63F0000u && BITS(e, 9, 5) == x) {
                    return false; /* BLR through x */
                }
                if (x == 16 || x == 17 || x == 30) {
                    break; /* written by the call */
                }
                return false;
            }
            if (e == RET) {
                if (x == 30) {
                    return false;
                }
                if (x == 16 || x == 17) {
                    break;
                }
                return false;
            }
            int64_t tgt = -1;
            bool cond = false, uncond = false;
            if ((e & 0xFF000010u) == 0x54000000u) {
                tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
                cond = true;
            } else if ((e & 0x7E000000u) == 0x34000000u) {
                if (BITS(e, 4, 0) == x) {
                    return false;
                }
                tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
                cond = true;
            } else if ((e & 0x7E000000u) == 0x36000000u) {
                if (BITS(e, 4, 0) == x) {
                    return false;
                }
                tgt = i + ((int32_t)(BITS(e, 18, 5) << 18) >> 18);
                cond = true;
            } else if ((e & 0xFC000000u) == 0x14000000u) {
                tgt = i + ((int32_t)(BITS(e, 25, 0) << 6) >> 6);
                uncond = true;
            }
            if (cond || uncond) {
                if (tgt < 0 || tgt >= n) {
                    /*
					 * With the window = the whole function, a branch out of it is a tail call: x16/x17 are
					 * scratch across it, x30 is the return address the callee will use.
					 */
                    if (!tail_ok || !(x == 16 || x == 17)) {
                        return false;
                    }
                    if (uncond) {
                        break;
                    }
                    i++;
                    continue;
                }
                if (uncond) {
                    i = (int)tgt;
                    continue;
                }
                if (sp >= A64X_WALK_STACK) {
                    return false;
                }
                stack[sp++] = (int)tgt;
                i++;
                continue;
            }
            if (!a64x_regs(e, &r, &wr)) {
                if ((e & 0xFE1FFC00u) == 0xD61F0000u || (e & 0xFF000000u) == 0xD4000000u || e == 0xD69F03E0u) {
                    return false;
                }
                uint32_t f0 = BITS(e, 4, 0), f1 = BITS(e, 9, 5), f2 = BITS(e, 14, 10), f3 = BITS(e, 20, 16);
                r = XMASK(f0) | XMASK(f1) | XMASK(f2) | XMASK(f3);
                if ((e & 0xBFA07C00u) == 0x08207C00u) {
                    r |= XMASK(f0 + 1 < 31 ? f0 + 1 : 31) | XMASK(f3 + 1 < 31 ? f3 + 1 : 31);
                }
                wr = 0;
            }
            if (r & XMASK(x)) {
                return false;
            }
            if (wr & XMASK(x)) {
                break;
            }
            i++;
        }
    }
    return true;
}

struct a64x_walk a64x_walk_in(void *buf, uint32_t words)
{
    struct a64x_walk walk = {buf, (uint8_t *)buf + A64X_WALK_STACK * sizeof(int), words};
    return walk;
}

uint32_t a64x_dead_regs_flow(const uint32_t *window, int n, int site, uint32_t candidates, int budget,
                             const struct a64x_walk *walk)
{
    uint32_t dead = 0;
    if (n > 16384) {
        n = 16384;
    }
    if (n > (int)walk->words) {
        return 0;
    }
    for (uint32_t x = 0; x < 31; x++) {
        if ((candidates & (1u << x)) && x != 18 && x != 29 && flow_dead(window, n, site, x, budget, walk, false)) {
            dead |= 1u << x;
        }
    }
    return dead;
}

uint32_t a64x_dead_regs_flow_fn(const uint32_t *fn, int n, int site, uint32_t candidates, int budget,
                                const struct a64x_walk *walk)
{
    uint32_t dead = 0;
    if (n > 65536) {
        n = 65536;
    }
    if (n > (int)walk->words) {
        return 0;
    }
    for (uint32_t x = 0; x < 31; x++) {
        if ((candidates & (1u << x)) && x != 18 && x != 29 && flow_dead(fn, n, site, x, budget, walk, true)) {
            dead |= 1u << x;
        }
    }
    return dead;
}

#include "a64x_vlive.h"

struct emit {
    uint32_t *out;
    int n, max;
    uint64_t pc0;
};

static inline void put(struct emit *e, uint32_t w)
{
    if (e->n < e->max) {
        e->out[e->n] = w;
    }
    e->n++;
}

static inline uint64_t here(struct emit *e)
{
    return e->pc0 + 4 * (uint64_t)e->n;
}

/* pick n scratch registers: dead ones first, else spill candidates; avoid every register in `avoid` */
static int pick_scratch(uint32_t dead, uint32_t avoid, int n, uint32_t *regs, uint32_t *spill_mask)
{
    static const uint8_t pref[] = {16, 17, 9, 10, 11, 12, 13, 14, 15, 8,  7,  6,  5,  4,
                                   3,  2,  1, 0,  19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
    int k = 0;
    *spill_mask = 0;
    for (unsigned i = 0; i < sizeof(pref) && k < n; i++) {
        if ((dead & (1u << pref[i])) && !(avoid & (1u << pref[i]))) {
            regs[k++] = pref[i];
            avoid |= 1u << pref[i];
        }
    }
    for (unsigned i = 0; i < sizeof(pref) && k < n; i++) {
        if (!(avoid & (1u << pref[i]))) {
            regs[k++] = pref[i];
            avoid |= 1u << pref[i];
            *spill_mask |= 1u << pref[i];
        }
    }
    return k == n ? 0 : -1;
}

/* n scratch registers, plus one for the address if the base is SP and SP has to move */
static int pick_regs(uint32_t dead, uint32_t avoid, int n, bool base_sp, uint32_t *sc, uint32_t *adr, uint32_t *spill)
{
    if (pick_scratch(dead, avoid, n, sc, spill) < 0) {
        return -1;
    }
    *adr = 31;
    if (base_sp && *spill) {
        uint32_t more, sp2, av = avoid;
        for (int i = 0; i < n; i++) {
            av |= 1u << sc[i];
        }
        if (pick_scratch(dead, av, 1, &more, &sp2) < 0) {
            return -1;
        }
        *adr = more;
        *spill |= sp2;
    }
    return 0;
}

/* spill frame: SP moves below the 128-byte red zone; spilled registers are stored at [SP, #0..] */
#define REDZONE 128u

static int spill_bytes(uint32_t spill_mask)
{
    int n = __builtin_popcount(spill_mask);
    return n ? (int)REDZONE + ((n + 1) & ~1) * 8 : 0;
}

static void emit_spill(struct emit *e, uint32_t spill_mask, bool save)
{
    /*
	 * SP moves once, folded into the first store (pre-index) and the last load (post-index):
	 *   stp r0, r1, [sp, #-frame]! ; stp r2, r3, [sp, #16]   ...   ldp r2, r3, [sp, #16] ; ldp r0, r1, [sp], #frame
	 * so a two-register spill costs two instructions in all.
	 */
    int frame = spill_bytes(spill_mask);
    if (!frame) {
        return;
    }
    uint32_t r[32];
    int n = 0;
    for (int i = 0; i < 31; i++) {
        if (spill_mask & (1u << i)) {
            r[n++] = (uint32_t)i;
        }
    }
    if (save) {
        if (n >= 2) {
            put(e,
                0xA9800000u | (((uint32_t)(-frame / 8) & 0x7F) << 15) | (r[1] << 10) | (31u << 5) | r[0]); /* stp pre */
        } else {
            put(e, 0xF8000C00u | (((uint32_t)(-frame) & 0x1FF) << 12) | (31u << 5) | r[0]); /* str pre */
        }
        for (int i = 2; i < n; i += 2) {
            put(e, i + 1 < n ? enc_stp_sp(r[i], r[i + 1], i * 8) : enc_str_sp(r[i], i * 8));
        }
    } else {
        for (int i = 2; i < n; i += 2) {
            put(e, i + 1 < n ? enc_ldp_sp(r[i], r[i + 1], i * 8) : enc_ldr_sp(r[i], i * 8));
        }
        if (n >= 2) {
            put(e,
                0xA8C00000u | (((uint32_t)(frame / 8) & 0x7F) << 15) | (r[1] << 10) | (31u << 5) | r[0]); /* ldp post */
        } else {
            put(e, 0xF8400400u | (((uint32_t)frame & 0x1FF) << 12) | (31u << 5) | r[0]); /* ldr post */
        }
    }
}

/*
 * Base register for the exclusive accesses. If Rn is SP and the stub moved SP, the address is SP + frame, computed
 * into a scratch register `adr` (which must then be in the scratch set).
 */
static uint32_t emit_base(struct emit *e, uint32_t rn, uint32_t frame, uint32_t adr)
{
    if (rn != 31 || frame == 0) {
        return rn;
    }
    put(e, enc_addi(true, adr, 31, frame, false));
    return adr;
}

static int stub_ldop_swp(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t *out, int max)
{
    uint32_t size = BITS(w, 31, 30), rs = BITS(w, 20, 16), rn = BITS(w, 9, 5), rt = BITS(w, 4, 0);
    bool x = size == 3, acq = BIT(w, 23), rel = BIT(w, 22);
    bool swp = BIT(w, 15);
    uint32_t opc = BITS(w, 14, 12);
    bool minmax = !swp && opc >= 4;
    bool narrow_minmax = minmax && size < 2;
    /* old value goes straight to Rt unless Rt is XZR or aliases Rs/Rn */
    bool direct = rt != 31 && rt != rs && rt != rn;
    int need = 1 /* status */ + (swp ? 0 : 1) /* new */ + (direct ? 0 : 1) /* old */ + (minmax ? 1 : 0) /* nzcv */ +
               (narrow_minmax ? 1 : 0) /* extended Rs */;
    uint32_t avoid = XMASK(rs) | XMASK(rn) | XMASK(rt) | XMASK(18) | XMASK(29) | XMASK(30);
    uint32_t sc[8], spill, adr;
    if (pick_regs(dead, avoid, need, rn == 31, sc, &adr, &spill) < 0) {
        return -1;
    }
    int k = 0;
    uint32_t st = sc[k++];
    uint32_t nw = swp ? rs : sc[k++];
    uint32_t old = direct ? rt : sc[k++];
    uint32_t nz = minmax ? sc[k++] : 0;
    uint32_t xs = narrow_minmax ? sc[k++] : rs;
    uint32_t frame_regs = spill;
    uint32_t frame = (uint32_t)spill_bytes(frame_regs);

    struct emit e = {out, 0, max, stub_pc};
    emit_spill(&e, frame_regs, true);
    uint32_t base = emit_base(&e, rn, frame, adr);
    if (minmax) {
        put(&e, enc_mrs_nzcv(nz));
    }
    if (narrow_minmax) {
        bool sgn = opc == 4 || opc == 5;
        put(&e, sgn ? enc_sxt(size, xs, rs) : enc_uxt(size, xs, rs));
    }
    uint64_t loop = here(&e);
    put(&e, enc_ldxr(size, acq, old, base));
    if (!swp) {
        switch (opc) {
        case 0:
            put(&e, enc_add(x, nw, old, rs));
            break;
        case 1:
            put(&e, enc_log(x, LOG_BIC, nw, old, rs));
            break;
        case 2:
            put(&e, enc_log(x, LOG_EOR, nw, old, rs));
            break;
        case 3:
            put(&e, enc_log(x, LOG_ORR, nw, old, rs));
            break;
        default: {
            /* compare in the operand width; narrow signed values need the loaded value sign-extended */
            uint32_t cmpv = old;
            if (size < 2 && (opc == 4 || opc == 5)) {
                put(&e, enc_sxt(size, nw, old));
                cmpv = nw;
            }
            put(&e, enc_cmp(x, cmpv, xs));
            uint32_t cond = opc == 4 ? COND_GT : opc == 5 ? COND_LT : opc == 6 ? COND_HI : COND_LO;
            put(&e, enc_csel(x, nw, old, xs, cond));
            break;
        }
        }
    }
    put(&e, enc_stxr(size, rel, st, nw, base));
    put(&e, enc_cbnz(false, st, (int64_t)(loop - here(&e))));
    if (minmax) {
        put(&e, enc_msr_nzcv(nz));
    }
    if (!direct && rt != 31) {
        put(&e, enc_mov(x, rt, old)); /* B/H: LDXRB/H already zero-extended */
    }
    emit_spill(&e, frame_regs, false);
    put(&e, enc_b(here(&e), ret_pc));
    return e.n;
}

static int stub_cas(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t *out, int max)
{
    uint32_t size = BITS(w, 31, 30), rs = BITS(w, 20, 16), rn = BITS(w, 9, 5), rt = BITS(w, 4, 0);
    bool x = size == 3, acq = BIT(w, 22), rel = BIT(w, 15);
    uint32_t avoid = XMASK(rs) | XMASK(rn) | XMASK(rt) | XMASK(18) | XMASK(29) | XMASK(30);
    uint32_t sc[4], spill, adr;
    if (pick_regs(dead, avoid, 2, rn == 31, sc, &adr, &spill) < 0) {
        return -1;
    }
    uint32_t old = sc[0], tmp = sc[1];
    uint32_t frame_regs = spill;
    uint32_t frame = (uint32_t)spill_bytes(frame_regs);
    struct emit e = {out, 0, max, stub_pc};
    emit_spill(&e, frame_regs, true);
    uint32_t base = emit_base(&e, rn, frame, adr);
    /*
	 *  loop: ldaxr old, [base]
	 *        eor   tmp, old, Rs        ; (and tmp, tmp, #0xff/#0xffff for B/H)
	 *        cbnz  tmp, fail
	 *        stlxr tmp, Rt, [base]
	 *        cbnz  tmp, loop
	 *        b     done
	 *  fail: clrex
	 *  done: mov   Rs, old
	 */
    uint64_t loop = here(&e);
    put(&e, enc_ldxr(size, acq, old, base));
    put(&e, enc_log(x, LOG_EOR, tmp, old, rs));
    if (size < 2) {
        put(&e, enc_and_mask(size, tmp, tmp));
    }
    int fail_br = e.n;
    put(&e, 0); /* cbnz tmp, fail (patched) */
    put(&e, enc_stxr(size, rel, tmp, rt, base));
    put(&e, enc_cbnz(false, tmp, (int64_t)(loop - here(&e))));
    put(&e, 0x14000002u); /* b done (skip clrex) */
    uint64_t fail = here(&e);
    put(&e, CLREX);
    if (fail_br < max) {
        out[fail_br] = enc_cbnz(x, tmp, (int64_t)(fail - (stub_pc + 4 * (uint64_t)fail_br)));
    }
    if (rs != 31) {
        put(&e, enc_mov(x, rs, old));
    }
    emit_spill(&e, frame_regs, false);
    put(&e, enc_b(here(&e), ret_pc));
    return e.n;
}

static int stub_casp(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t *out, int max)
{
    uint32_t rs = BITS(w, 20, 16), rn = BITS(w, 9, 5), rt = BITS(w, 4, 0);
    bool x = BIT(w, 30), acq = BIT(w, 22), rel = BIT(w, 15);
    if ((rs & 1) || (rt & 1) || rs == 30 || rt == 30) {
        return -1; /* register pairs must be even; odd ones are unallocated */
    }
    uint32_t avoid =
        XMASK(rs) | XMASK(rs + 1) | XMASK(rn) | XMASK(rt) | XMASK(rt + 1) | XMASK(18) | XMASK(29) | XMASK(30);
    uint32_t sc[5], spill, adr;
    if (pick_regs(dead, avoid, 4, rn == 31, sc, &adr, &spill) < 0) {
        return -1;
    }
    uint32_t o1 = sc[0], o2 = sc[1], t1 = sc[2], t2 = sc[3];
    uint32_t frame_regs = spill;
    uint32_t frame = (uint32_t)spill_bytes(frame_regs);
    struct emit e = {out, 0, max, stub_pc};
    emit_spill(&e, frame_regs, true);
    uint32_t base = emit_base(&e, rn, frame, adr);
    /*
	 *  loop: ldaxp o1, o2, [base]
	 *        eor t1, o1, Rs ; eor t2, o2, Rs+1 ; orr t1, t1, t2
	 *        cbnz t1, fail
	 *        stlxp t1, Rt, Rt+1, [base] ; cbnz t1, loop ; b done
	 *  fail: stlxp t1, o1, o2, [base] ; cbnz t1, loop     (store back: the pair read is only atomic if a store succeeds)
	 *  done: mov Rs, o1 ; mov Rs+1, o2
	 */
    uint64_t loop = here(&e);
    put(&e, enc_ldxp(x, acq, o1, o2, base));
    put(&e, enc_log(x, LOG_EOR, t1, o1, rs));
    put(&e, enc_log(x, LOG_EOR, t2, o2, rs + 1));
    put(&e, enc_log(x, LOG_ORR, t1, t1, t2));
    int fail_br = e.n;
    put(&e, 0);
    put(&e, enc_stxp(x, rel, t1, rt, rt + 1, base));
    put(&e, enc_cbnz(false, t1, (int64_t)(loop - here(&e))));
    put(&e, 0x14000003u); /* b done */
    uint64_t fail = here(&e);
    put(&e, enc_stxp(x, rel, t1, o1, o2, base));
    put(&e, enc_cbnz(false, t1, (int64_t)(loop - here(&e))));
    if (fail_br < max) {
        out[fail_br] = enc_cbnz(x, t1, (int64_t)(fail - (stub_pc + 4 * (uint64_t)fail_br)));
    }
    put(&e, enc_mov(x, rs, o1));
    put(&e, enc_mov(x, rs + 1, o2));
    emit_spill(&e, frame_regs, false);
    put(&e, enc_b(here(&e), ret_pc));
    return e.n;
}

static int stub_ldra(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t *out, int max)
{
    uint32_t rn = BITS(w, 9, 5), rt = BITS(w, 4, 0);
    int32_t imm = (int32_t)((BIT(w, 22) << 9) | BITS(w, 20, 12));
    if (imm & 0x200) {
        imm -= 0x400;
    }
    imm *= 8;
    bool wb = BIT(w, 11);
    uint32_t mag = (uint32_t)(imm < 0 ? -imm : imm);
    uint32_t dst = wb ? rn : rt; /* register that receives the address */
    if (dst == 31 && !wb) {
        return -1; /* load to XZR with a far negative offset: leave to the trap path */
    }
    struct emit e = {out, 0, max, stub_pc};
    bool lsl12 = mag > 4095;
    uint32_t imm12 = lsl12 ? mag >> 12 : mag;
    if (lsl12 && (mag & 0xFFF)) {
        return -1; /* cannot happen for |imm| <= 4096 */
    }
    put(&e, imm < 0 ? enc_subi(true, dst, rn, imm12, lsl12) : enc_addi(true, dst, rn, imm12, lsl12));
    put(&e, 0xF9400000u | (dst << 5) | rt); /* LDR Xt, [dst] */
    put(&e, enc_b(here(&e), ret_pc));
    return e.n;
}

/* forward-branch fixups inside a stub */
static inline int mark(struct emit *e)
{
    return e->n;
}

static void fix_imm19(struct emit *e, int at, int target)
{
    if (at < e->max) {
        e->out[at] |= ((uint32_t)(target - at) & 0x7FFFFu) << 5;
    }
}

static void fix_imm26(struct emit *e, int at, int target)
{
    if (at < e->max) {
        e->out[at] |= (uint32_t)(target - at) & 0x3FFFFFFu;
    }
}

#define BCOND(c) (0x54000000u | (c))
#define B_FWD    0x14000000u
#define COND_EQ  0x0
#define COND_NE  0x1
#define COND_HS  0x2
#define COND_MI  0x4
#define COND_PL  0x5

/* CCMN Wzr, #0, #0, cond: Z=1,N=C=V=0 when cond holds, else NZCV=0000 */
static inline uint32_t enc_ccmn_zero(uint32_t cond)
{
    return 0x3A400BE0u | (cond << 12);
}

#define CMN_WZR_1 0x310007FFu /* NZCV = 0000 */

/*
 * FJCVTZS Wd, Dn (JavaScript ToInt32): Wd = truncate(Dn) modulo 2^32 (0 for NaN and infinities); Z = 1 iff Dn is an
 * integer in [-2^31, 2^31-1] other than -0.0; N = C = V = 0. Scratch: two X registers. The common case (1 <= |x| < 2^31)
 * is ten instructions; FPSR cumulative flags are not reproduced exactly.
 */
static int stub_jscvt(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t dead, uint32_t *out, int max)
{
    uint32_t rd = BITS(w, 4, 0), dn = BITS(w, 9, 5);
    uint32_t avoid = XMASK(rd) | XMASK(18) | XMASK(29) | XMASK(30);
    uint32_t sc[3], spill;
    int need = rd == 31 ? 3 : 2;
    if (pick_scratch(dead, avoid, need, sc, &spill) < 0) {
        return -1;
    }
    uint32_t a = sc[0], b = sc[1], c = rd == 31 ? sc[2] : rd;
    struct emit e = {out, 0, max, stub_pc};
    emit_spill(&e, spill, true);
    put(&e, 0x9E660000u | (dn << 5) | a);              /* fmov  xa, dn */
    put(&e, 0xD340F800u | (52u << 16) | (a << 5) | b); /* ubfx  xb, xa, #52, #11 */
    put(&e, enc_subi(true, b, b, 1023, false));        /* sub   xb, xb, #1023      (e) */
    put(&e, 0xF100001Fu | (31u << 10) | (b << 5));     /* cmp   xb, #31 */
    int j_slow = mark(&e);
    put(&e, BCOND(COND_HS));                         /* b.hs  slow               (e < 0 or e >= 31) */
    put(&e, enc_addi(true, b, b, 12, false));        /* add   xb, xb, #12 */
    put(&e, 0x9AC02000u | (b << 16) | (a << 5) | b); /* lsl   xb, xa, xb         (fraction bits) */
    put(&e, 0x1E780000u | (dn << 5) | rd);           /* fcvtzs wd, dn            (exact range) */
    put(&e, 0xEA00001Fu | (b << 16) | (b << 5));     /* tst   xb, xb */
    put(&e, enc_ccmn_zero(COND_EQ));                 /* Z = integral; N = C = V = 0 */
    int j_done1 = mark(&e);
    put(&e, B_FWD);
    /* slow: e < 0 (|x| < 1) or e >= 31 */
    int slow = mark(&e);
    put(&e, 0xF100001Fu | (b << 5)); /* cmp   xb, #0  (B.cond, not TBNZ: +-1 MB when split) */
    int j_small = mark(&e);
    put(&e, BCOND(0xB));                           /* b.lt  small */
    put(&e, 0xF100001Fu | (63u << 10) | (b << 5)); /* cmp   xb, #63 */
    int j_huge = mark(&e);
    put(&e, BCOND(COND_HS));                   /* b.hs  huge               (|x| >= 2^63, inf, nan) */
    put(&e, 0x9E780000u | (dn << 5) | b);      /* fcvtzs xb, dn            (exact for |x| < 2^63) */
    put(&e, enc_mov(false, c, b));             /* mov   wc, wb             (mod 2^32) */
    put(&e, 0xD2E00000u | (0xC1E0u << 5) | b); /* mov   xb, #0xC1E0 << 48  (-2^31) */
    put(&e, enc_cmp(true, a, b));
    put(&e, enc_ccmn_zero(COND_EQ)); /* Z = (x == -2^31) */
    int j_done2 = mark(&e);
    put(&e, B_FWD);
    /* huge: |x| = m * 2^s, s = e - 52 >= 11; zero once s >= 32 (also inf and NaN, e = 1024) */
    int huge = mark(&e);
    put(&e, 0xF100001Fu | (84u << 10) | (b << 5)); /* cmp   xb, #84 */
    int j_zero = mark(&e);
    put(&e, BCOND(COND_HS));
    put(&e, enc_subi(true, b, b, 52, false));                          /* s */
    put(&e, 0x9240CC00u | (a << 5) | c);                               /* and   xc, xa, #(1 << 52) - 1 */
    put(&e, 0xB24C0000u | (c << 5) | c);                               /* orr   xc, xc, #1 << 52 */
    put(&e, 0x9AC02000u | (b << 16) | (c << 5) | c);                   /* lsl   xc, xc, xb */
    put(&e, 0xEA00001Fu | (a << 16) | (a << 5));                       /* tst   xa, xa             (N = sign) */
    put(&e, 0x5A800400u | (c << 16) | (COND_PL << 12) | (c << 5) | c); /* cneg wc, wc, mi */
    put(&e, CMN_WZR_1);                                                /* Z = 0 */
    int j_done3 = mark(&e);
    put(&e, B_FWD);
    int zero = mark(&e);
    put(&e, 0x2A1F03E0u | c); /* mov   wc, wzr */
    put(&e, CMN_WZR_1);
    int j_done4 = mark(&e);
    put(&e, B_FWD);
    /* small: |x| < 1 -> 0; Z only for +0.0 */
    int small = mark(&e);
    put(&e, 0x2A1F03E0u | c);
    put(&e, 0xEA00001Fu | (a << 16) | (a << 5)); /* tst   xa, xa */
    put(&e, enc_ccmn_zero(COND_EQ));
    int done = mark(&e);
    if (c != rd && rd != 31) {
        put(&e, enc_mov(false, rd, c));
    }
    emit_spill(&e, spill, false);
    put(&e, enc_b(here(&e), ret_pc));
    fix_imm19(&e, j_slow, slow);
    fix_imm19(&e, j_small, small);
    fix_imm19(&e, j_huge, huge);
    fix_imm19(&e, j_zero, zero);
    fix_imm26(&e, j_done1, done);
    fix_imm26(&e, j_done2, done);
    fix_imm26(&e, j_done3, done);
    fix_imm26(&e, j_done4, done);
    return e.n;
}

/*
 * SIMD scratch: the GPR liveness does not cover V registers, so vector temporaries are always spilled (one STR/STP of
 * Q registers below the red zone, one LDR/LDP back). Picked from V16-V31 (caller-saved, never arguments), avoiding the
 * instruction's own operands.
 */
/*
 * n temporaries: V registers dead at the site first (no save needed), then V31..V16 (saved); `spill` gets the
 * ones to save.
 */
static int pick_vtemps_dead(uint32_t vdead, uint32_t avoid, int n, uint32_t *v, uint32_t *spill, int *nspill)
{
    int k = 0;
    *nspill = 0;
    for (int r = 31; r >= 0 && k < n; r--) {
        if ((vdead & (1u << r)) && !(avoid & (1u << r))) {
            v[k++] = (uint32_t)r;
            avoid |= 1u << r;
        }
    }
    for (uint32_t r = 31; r >= 16 && k < n; r--) {
        if (!(avoid & (1u << r))) {
            v[k++] = r;
            spill[(*nspill)++] = r;
            avoid |= 1u << r;
        }
    }
    return k == n ? 0 : -1;
}

static void emit_vspill(struct emit *e, const uint32_t *v, int n, bool save)
{
    if (!n) {
        return;
    }
    uint32_t frame = REDZONE + 16u * (uint32_t)((n + 1) & ~1);
    if (save) {
        put(e, n >= 2 ? 0xAD800000u | ((((uint32_t)(-(int32_t)frame / 16)) & 0x7F) << 15) | (v[1] << 10) | (31u << 5) |
                            v[0]
                      : 0x3C800C00u | ((((uint32_t)(-(int32_t)frame)) & 0x1FF) << 12) | (31u << 5) | v[0]);
        for (int i = 2; i < n; i += 2) {
            put(e, i + 1 < n ? 0xAD000000u | ((uint32_t)i << 15) | (v[i + 1] << 10) | (31u << 5) | v[i]
                             : 0x3D800000u | ((uint32_t)i << 10) | (31u << 5) | v[i]);
        }
    } else {
        for (int i = 2; i < n; i += 2) {
            put(e, i + 1 < n ? 0xAD400000u | ((uint32_t)i << 15) | (v[i + 1] << 10) | (31u << 5) | v[i]
                             : 0x3DC00000u | ((uint32_t)i << 10) | (31u << 5) | v[i]);
        }
        put(e, n >= 2 ? 0xACC00000u | (((frame / 16) & 0x7F) << 15) | (v[1] << 10) | (31u << 5) | v[0]
                      : 0x3CC00400u | ((frame & 0x1FF) << 12) | (31u << 5) | v[0]);
    }
}

/* vector encoders: q = 128-bit, size = element size field as in the instruction */
static inline uint32_t v_rev64(bool q, uint32_t size, uint32_t d, uint32_t n)
{
    return 0x0E200800u | ((uint32_t)q << 30) | (size << 22) | (n << 5) | d;
}

static inline uint32_t v_ext8(uint32_t d, uint32_t n, uint32_t m)
{
    return 0x6E004000u | (m << 16) | (n << 5) | d;
}

static inline uint32_t v_trn(bool two, bool q, uint32_t size, uint32_t d, uint32_t n, uint32_t m)
{
    return (two ? 0x0E006800u : 0x0E002800u) | ((uint32_t)q << 30) | (size << 22) | (m << 16) | (n << 5) | d;
}

static inline uint32_t v_fneg(bool q, bool dbl, uint32_t d, uint32_t n)
{
    return 0x2EA0F800u | ((uint32_t)q << 30) | ((uint32_t)dbl << 22) | (n << 5) | d;
}

static inline uint32_t v_fmla(bool q, bool dbl, uint32_t d, uint32_t n, uint32_t m)
{
    return 0x0E20CC00u | ((uint32_t)q << 30) | ((uint32_t)dbl << 22) | (m << 16) | (n << 5) | d;
}

static inline uint32_t v_fadd(bool q, bool dbl, uint32_t d, uint32_t n, uint32_t m)
{
    return 0x0E20D400u | ((uint32_t)q << 30) | ((uint32_t)dbl << 22) | (m << 16) | (n << 5) | d;
}

static inline uint32_t v_dup_d(uint32_t d, uint32_t n, uint32_t idx)
{
    return 0x4E000400u | ((8u | (idx << 4)) << 16) | (n << 5) | d;
}

/* complex pairs: swap the two halves of every pair, duplicate the real/imaginary half, merge (a.re, b.im) */
static void cx_swap(struct emit *e, bool q, bool dbl, uint32_t d, uint32_t n)
{
    put(e, dbl ? v_ext8(d, n, n) : v_rev64(q, 2, d, n));
}

static void cx_dup(struct emit *e, bool q, bool dbl, bool imag, uint32_t d, uint32_t n)
{
    put(e, v_trn(imag, q, dbl ? 3 : 2, d, n, n));
}

/*
 * m-part of a rotation, into t: rot 90 -> (-m.im, m.re), rot 270 -> (m.im, -m.re), using u as a second temporary.
 * (a0, b1) merges are TRN1(a, swap(b)).
 */
static void cx_rot_m(struct emit *e, bool q, bool dbl, bool r270, uint32_t t, uint32_t u, uint32_t m)
{
    uint32_t sz = dbl ? 3 : 2;
    if (!r270) {
        cx_swap(e, q, dbl, u, m);             /* (m.im, m.re) */
        put(e, v_fneg(q, dbl, u, u));         /* (-m.im, -m.re) */
        put(e, v_trn(false, q, sz, t, u, m)); /* (-m.im, m.re) */
    } else {
        put(e, v_fneg(q, dbl, u, m));         /* (-m.re, -m.im) */
        cx_swap(e, q, dbl, t, m);             /* (m.im, m.re) */
        put(e, v_trn(false, q, sz, t, t, u)); /* (m.im, -m.re) */
    }
}

/*
 * FCADD / FCMLA (vector and by element), single and double precision: lane-for-lane FADD/FMLA on permuted operands,
 * so every lane is the same fused operation on the same values (NaN signs included) as the original.
 */
static int stub_fcma(uint32_t w, uint64_t stub_pc, uint64_t ret_pc, uint32_t vdead, uint32_t *out, int max)
{
    bool q = BIT(w, 30);
    uint32_t size = BITS(w, 23, 22), d = BITS(w, 4, 0), n = BITS(w, 9, 5), m;
    bool byelem = (w & 0xBF000000u) == 0x2F000000u;
    bool fcadd = !byelem && (w & 0x0000E400u) == 0x0000E400u && !BIT(w, 21) && BITS(w, 15, 13) == 7;
    uint32_t rot;
    int idx = -1;
    if (size == 1 || (size == 3 && !q) || size == 0) {
        return -1; /* half precision goes through the FP16 path (none in the cache); 1d is reserved */
    }
    bool dbl = size == 3;
    if (byelem) {
        if (size != 2 || !q) {
            return -1; /* .2s by element has a single pair: index 0 only; not seen */
        }
        m = BITS(w, 20, 16);
        idx = (int)BIT(w, 11);
        rot = BITS(w, 14, 13);
    } else if (fcadd) {
        m = BITS(w, 20, 16);
        rot = BIT(w, 12) ? 3 : 1;
    } else {
        m = BITS(w, 20, 16);
        rot = BITS(w, 12, 11);
    }
    uint32_t avoid = (1u << d) | (1u << n) | (1u << m), v[2], vs[2];
    int nt = 2, nvs;
    if (pick_vtemps_dead(vdead, avoid, nt, v, vs, &nvs) < 0) {
        return -1;
    }
    uint32_t t = v[0], u = v[1];
    struct emit e = {out, 0, max, stub_pc};
    emit_vspill(&e, vs, nvs, true);
    uint32_t mm = m;
    if (idx >= 0) {
        put(&e, v_dup_d(t, m, (uint32_t)idx)); /* the indexed complex pair, in every pair */
        mm = t;
    }
    if (fcadd) {
        cx_rot_m(&e, q, dbl, rot == 3, t, u, mm);
        put(&e, v_fadd(q, dbl, d, n, t));
    } else {
        switch (rot) {
        case 0: /* d.re += n.re*m.re ; d.im += n.re*m.im */
            t = mm;
            cx_dup(&e, q, dbl, false, u, n);
            break;
        case 2: /* d.re += n.re*(-m.re) ; d.im += n.re*(-m.im) */
            put(&e, v_fneg(q, dbl, t, mm));
            cx_dup(&e, q, dbl, false, u, n);
            break;
        case 1: /* d.re += n.im*(-m.im) ; d.im += n.im*m.re */
            cx_rot_m(&e, q, dbl, false, t, u, mm);
            cx_dup(&e, q, dbl, true, u, n);
            break;
        default: /* d.re += n.im*m.im ; d.im += n.im*(-m.re) */
            cx_rot_m(&e, q, dbl, true, t, u, mm);
            cx_dup(&e, q, dbl, true, u, n);
            break;
        }
        put(&e, v_fmla(q, dbl, d, u, t));
    }
    emit_vspill(&e, vs, nvs, false);
    put(&e, enc_b(here(&e), ret_pc));
    return e.n;
}

#include "a64x_fp16.h"

int a64x_stub(uint32_t w, uint64_t site_pc, uint64_t stub_pc, uint32_t dead, uint32_t *out, int max)
{
    return a64x_stub_v(w, site_pc, stub_pc, dead, 0, out, max);
}

int a64x_stub_v(uint32_t w, uint64_t site_pc, uint64_t stub_pc, uint32_t dead, uint32_t vdead, uint32_t *out, int max)
{
    uint64_t ret_pc = site_pc + 4;
    switch (a64x_classify(w)) {
    case A64X_LSE_LDOP:
    case A64X_LSE_SWP:
        return stub_ldop_swp(w, stub_pc, ret_pc, dead, out, max);
    case A64X_LSE_CAS:
        return stub_cas(w, stub_pc, ret_pc, dead, out, max);
    case A64X_LSE_CASP:
        return stub_casp(w, stub_pc, ret_pc, dead, out, max);
    case A64X_PAC_LDRA: {
        uint32_t one;
        if (a64x_rewrite1(w, &one)) {
            return -1;
        }
        return stub_ldra(w, stub_pc, ret_pc, out, max);
    }
    case A64X_JSCVT:
        return stub_jscvt(w, stub_pc, ret_pc, dead, out, max);
    case A64X_FCMA:
        return stub_fcma(w, stub_pc, ret_pc, vdead, out, max);
    case A64X_FP16:
        return stub_fp16(w, stub_pc, ret_pc, dead, vdead, out, max);
    case A64X_RDM:
        return stub_rdm(w, stub_pc, ret_pc, dead, vdead, out, max);
    case A64X_SB: {
        /* SB: the speculation barrier's ARMv8.0 equivalent */
        struct emit e = {out, 0, max, stub_pc};
        put(&e, 0xD5033F9Fu); /* dsb sy */
        put(&e, 0xD5033FDFu); /* isb */
        put(&e, enc_b(here(&e), ret_pc));
        return e.n;
    }
    default:
        return -1;
    }
}

uint32_t a64x_branch(uint64_t from, uint64_t to)
{
    int64_t d = (int64_t)(to - from);
    if (d < -(1LL << 27) || d >= (1LL << 27) || (d & 3)) {
        return 0;
    }
    return enc_b(from, to);
}
