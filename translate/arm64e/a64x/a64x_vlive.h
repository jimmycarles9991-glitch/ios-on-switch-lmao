/*
 * a64x_vlive.h (included by a64x_rewrite.c): which SIMD&FP registers are dead after a site, so FP stubs can use
 * them as temporaries instead of spilling Q registers.
 *
 * a64x_vregs() gives, for one instruction, the V registers it may read and those it writes in full (all 128 bits,
 * without depending on the old value). Anything not understood reads every register it names (Rd, Rn, Rm, Ra and
 * Rn+1..Rn+3 for table lookups): that is always safe, it only loses deadness. Partial writes (INS, the "2" narrowing
 * forms, LD1 to one lane, ORR/BIC immediate, SLI/SRI) and accumulating forms (MLA, FMLA, SSRA, dot products, BSL...)
 * count as reads of Rd. SVE/SME words read everything.
 * Calls clobber V16-V31 (AAPCS64: caller-saved, never arguments) and read V0-V15 (arguments; V8-V15 low halves are
 * preserved for the caller). RET and tail calls: V16-V31 dead, V0-V15 live.
 */
#define VM(r) (1u << ((r) & 31))

static bool a64x_vregs(uint32_t w, uint32_t *vr, uint32_t *vw)
{
    uint32_t rd = BITS(w, 4, 0), rn = BITS(w, 9, 5), rm = BITS(w, 20, 16), ra = BITS(w, 14, 10);
    *vr = 0;
    *vw = 0;
    uint32_t op0 = BITS(w, 28, 25);
    if (op0 == 0 || op0 == 2) {
        /* reserved/SME and SVE: everything */
        if (op0 == 0 && !(w >> 16)) {
            return true; /* UDF */
        }
        *vr = 0xFFFFFFFFu;
        return false;
    }
    /* Loads and stores (op0 x1x0) with V = 1 */
    if ((op0 & 5) == 4) {
        if (!BIT(w, 26)) {
            return true; /* GPR load/store */
        }
        if ((w & 0x3B000000u) == 0x18000000u) { /* LDR (literal, SIMD&FP) */
            *vw = VM(rd);
            return true;
        }
        if ((w & 0x3A000000u) == 0x28000000u) { /* LDP/STP/LDNP/STNP (SIMD&FP) */
            uint32_t pr = VM(rd) | VM(ra);
            if (BIT(w, 22))
                *vw = pr;
            else
                *vr = pr;
            return true;
        }
        if ((w & 0x3B000000u) == 0x39000000u || (w & 0x3B200000u) == 0x38000000u || (w & 0x3B200C00u) == 0x38200800u) {
            /* LDR/STR (immediate unsigned, unscaled/pre/post, register) */
            bool load = BIT(w, 22);
            if (load)
                *vw = VM(rd);
            else
                *vr = VM(rd);
            return true;
        }
        if ((w & 0xBF9F0000u) == 0x0C000000u || (w & 0xBFA00000u) == 0x0C800000u) {
            /* LD1-LD4/ST1-ST4 (multiple structures), optional post-index */
            uint32_t opc = BITS(w, 15, 12), nr;
            switch (opc) {
            case 0:
            case 2:
                nr = 4;
                break;
            case 4:
            case 6:
                nr = 3;
                break;
            case 7:
                nr = 1;
                break;
            case 8:
            case 10:
                nr = 2;
                break;
            default:
                *vr = 0xFFFFFFFFu;
                return false;
            }
            uint32_t m = 0;
            for (uint32_t k = 0; k < nr; k++)
                m |= VM(rd + k);
            if (BIT(w, 22))
                *vw = m;
            else
                *vr = m;
            return true;
        }
        if ((w & 0xBF9F0000u) == 0x0D000000u || (w & 0xBF800000u) == 0x0D800000u) {
            /* single structure: lane (partial) or replicate (LDnR: full) */
            uint32_t opc = BITS(w, 15, 13), selem = ((BIT(w, 13) << 1) | BIT(w, 21)) + 1, m = 0;
            for (uint32_t k = 0; k < selem; k++)
                m |= VM(rd + k);
            if (BIT(w, 22) && (opc == 6 || opc == 7))
                *vw = m;
            else
                *vr = m;
            return true;
        }
        *vr = VM(rd) | VM(ra); /* other SIMD&FP load/store forms: conservative */
        return true;
    }
    if ((op0 & 7) != 7) {
        return true; /* not SIMD&FP data processing: no V registers */
    }
    /* Scalar floating point (0 0 S 11110 / 11111) */
    if ((w & 0x5F000000u) == 0x1E000000u && !BIT(w, 29)) {
        if (!BIT(w, 21)) { /* fixed-point conversions */
            uint32_t opc = BITS(w, 18, 16);
            if (opc == 2 || opc == 3)
                *vw = VM(rd);
            else
                *vr = VM(rn);
            return true;
        }
        if (BITS(w, 15, 10) == 0) { /* integer conversions and FMOV general */
            uint32_t opc = BITS(w, 18, 16), rmode = BITS(w, 20, 19);
            if (opc == 2 || opc == 3) {
                *vw = VM(rd);
                return true;
            } /* SCVTF/UCVTF */
            if (opc == 7) { /* FMOV to SIMD&FP */
                if (rmode == 1)
                    *vr = VM(rd);
                else
                    *vw = VM(rd); /* FMOV Vd.D[1], Xn is partial */
                return true;
            }
            *vr = VM(rn); /* to general register */
            return true;
        }
        if (BITS(w, 14, 10) == 0x10 || BITS(w, 12, 10) == 4) { /* 1-source; FMOV (immediate) */
            *vw = VM(rd);
            *vr = BITS(w, 12, 10) == 4 ? 0 : VM(rn);
            return true;
        }
        if (BITS(w, 13, 10) == 8 || BITS(w, 11, 10) == 1) { /* compare, conditional compare */
            *vr = VM(rn) | VM(rm);
            return true;
        }
        /* 2-source, conditional select */
        *vw = VM(rd);
        *vr = VM(rn) | VM(rm);
        return true;
    }
    if ((w & 0xFF000000u) == 0x1F000000u) { /* 3-source */
        *vw = VM(rd);
        *vr = VM(rn) | VM(rm) | VM(ra);
        return true;
    }
    /* Advanced SIMD */
    bool scalar = BIT(w, 28), q = BIT(w, 30), u = BIT(w, 29);
    uint32_t generic_r = VM(rn) | VM(rm);
    if ((w & 0x9F000000u) == 0x0E000000u || (w & 0xDF000000u) == 0x5E000000u) {
        if ((w & 0x9FE08400u) == 0x0E000400u || (w & 0xDFE08400u) == 0x5E000400u) {
            /* copy: DUP (element/general), SMOV, UMOV, INS */
            uint32_t imm4 = BITS(w, 14, 11);
            if (u) {
                *vr = VM(rd) | VM(rn);
                return true;
            } /* INS (element) */
            if (imm4 == 0) {
                *vw = VM(rd);
                *vr = VM(rn);
                return true;
            } /* DUP element */
            if (imm4 == 1) {
                *vw = VM(rd);
                return true;
            } /* DUP general */
            if (imm4 == 3) {
                *vr = VM(rd);
                return true;
            } /* INS general */
            *vr = VM(rn);
            return true; /* SMOV/UMOV */
        }
        if (!BIT(w, 21)) {
            if (BIT(w, 10)) {
                if (BIT(w, 22) && BITS(w, 15, 14) == 0) {
                    /* three same FP16: FMLA/FMLS accumulate */
                    bool acc = BITS(w, 13, 11) == 1;
                    *vr = generic_r | (acc ? VM(rd) : 0);
                    if (!acc)
                        *vw = VM(rd);
                    return true;
                }
                /* three same extra (SQRDMLAH, dot products, FCMLA, FCADD, MMLA) and the rest: keep Rd */
                *vr = generic_r | VM(rd);
                return true;
            }
            if (!scalar && !u && !BIT(w, 15) && BITS(w, 11, 10) == 0 && BITS(w, 23, 22) == 0) {
                /* TBL/TBX: Rn..Rn+len */
                uint32_t len = BITS(w, 14, 13), m = VM(rm);
                for (uint32_t k = 0; k <= len; k++)
                    m |= VM(rn + k);
                bool tbx = BIT(w, 12);
                *vr = m | (tbx ? VM(rd) : 0);
                if (!tbx)
                    *vw = VM(rd);
                return true;
            }
            if (!scalar && !u && !BIT(w, 15) && BITS(w, 11, 10) == 2) { /* UZP/TRN/ZIP */
                *vr = generic_r;
                *vw = VM(rd);
                return true;
            }
            if (!scalar && u && BITS(w, 23, 22) == 0 && !BIT(w, 15) && !BIT(w, 10)) { /* EXT */
                *vr = generic_r;
                *vw = VM(rd);
                return true;
            }
            /* crypto (SHA1/SHA256 three-register...) and the rest */
            *vr = generic_r | VM(rd) | VM(ra);
            return true;
        }
        if (BIT(w, 10)) {
            /* three same: accumulating opcodes keep Rd */
            uint32_t opc = BITS(w, 15, 11), size = BITS(w, 23, 22);
            bool acc = opc == 0x12 || opc == 0x19 || opc == 0x0F || opc == 0x1D || (opc == 0x03 && u && size != 0);
            *vr = generic_r | (acc ? VM(rd) : 0);
            if (!acc)
                *vw = VM(rd);
            return true;
        }
        if (BITS(w, 11, 10) == 0) {
            /* three different */
            uint32_t opc = BITS(w, 15, 12);
            bool pure = opc <= 3 || opc == 7 || (opc >= 12 && opc <= 14) || ((opc == 4 || opc == 6) && (!q || scalar));
            *vr = generic_r | (pure ? 0 : VM(rd));
            if (pure)
                *vw = VM(rd);
            return true;
        }
        if (BITS(w, 11, 10) == 2 && BITS(w, 21, 17) == 0x10) {
            /* two-register misc */
            uint32_t opc = BITS(w, 16, 12);
            bool narrow = opc == 0x12 || opc == 0x14 || opc == 0x16;
            bool acc = opc == 0x06 || opc == 0x03 || (narrow && q && !scalar); /* SADALP, SUQADD/USQADD accumulate */
            *vr = VM(rn) | (acc ? VM(rd) : 0);
            if (!acc)
                *vw = VM(rd);
            return true;
        }
        if (BITS(w, 11, 10) == 2 && (BITS(w, 21, 17) == 0x18 || BITS(w, 22, 17) == 0x3C)) {
            /* across lanes / pairwise (incl. FP16), FP16 two-register misc: pure */
            *vr = VM(rn);
            *vw = VM(rd);
            return true;
        }
        /* AES and the rest */
        *vr = generic_r | VM(rd);
        return true;
    }
    if ((w & 0x9F000400u) == 0x0F000400u || (w & 0xDF000400u) == 0x5F000400u) {
        if (BITS(w, 22, 19) == 0 && !scalar) {
            /* modified immediate: MOVI/MVNI/FMOV full; ORR/BIC immediate keep Rd */
            uint32_t cmode = BITS(w, 15, 12);
            bool rmw = (cmode & 1) && cmode < 12;
            *vr = rmw ? VM(rd) : 0;
            if (!rmw)
                *vw = VM(rd);
            return true;
        }
        /* shift by immediate */
        uint32_t opc = BITS(w, 15, 11);
        bool acc = opc == 0x02 || opc == 0x06 || opc == 0x08 || (opc == 0x0A && u);
        bool narrow = opc >= 0x10 && opc <= 0x13;
        if (narrow && q && !scalar)
            acc = true;
        *vr = VM(rn) | (acc ? VM(rd) : 0);
        if (!acc)
            *vw = VM(rd);
        return true;
    }
    if ((w & 0x9F000400u) == 0x0F000000u || (w & 0xDF000400u) == 0x5F000000u) {
        /* by element: only the multiplies are pure */
        uint32_t opc = BITS(w, 15, 12);
        bool pure = (!u && (opc == 8 || opc == 9 || opc == 10 || opc == 11 || opc == 12 || opc == 13)) ||
                    (u && (opc == 9 || opc == 10));
        uint32_t rm4 = BITS(w, 19, 16);
        *vr = VM(rn) | VM(rm) | VM(rm4) | (pure ? 0 : VM(rd));
        if (pure)
            *vw = VM(rd);
        return true;
    }
    /* crypto and anything else: read every named register, write nothing */
    *vr = VM(rd) | VM(rn) | VM(rm) | VM(ra) | VM(rn + 1) | VM(rn + 2) | VM(rn + 3);
    return true;
}

/* V register v dead after w[site]? (same path walk as flow_dead) */
static bool vflow_dead(const uint32_t *w, int n, int site, uint32_t v, int budget, const struct a64x_walk *walk,
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
    bool scratch = v >= 16;
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
                break;
            }
            uint32_t e = w[i], c;
            if (a64x_rewrite1(e, &c)) {
                e = c;
            }
            if ((e & 0xFFE0001Fu) == 0xD4200000u || (e & 0xFFFF0000u) == 0) {
                break;
            }
            bool call = (e & 0xFC000000u) == 0x94000000u || (e & 0xFFFFFC1Fu) == 0xD63F0000u;
            if (call || e == RET) {
                if (scratch)
                    break; /* clobbered by the callee / not preserved for the caller */
                return false;
            }
            if ((e & 0xFE1FFC00u) == 0xD61F0000u) {
                return false; /* other indirect branches */
            }
            int64_t tgt = -1;
            bool cond = false, uncond = false;
            if ((e & 0xFF000010u) == 0x54000000u) {
                tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
                cond = true;
            } else if ((e & 0x7E000000u) == 0x34000000u) {
                tgt = i + ((int32_t)(BITS(e, 23, 5) << 13) >> 13);
                cond = true;
            } else if ((e & 0x7E000000u) == 0x36000000u) {
                tgt = i + ((int32_t)(BITS(e, 18, 5) << 18) >> 18);
                cond = true;
            } else if ((e & 0xFC000000u) == 0x14000000u) {
                tgt = i + ((int32_t)(BITS(e, 25, 0) << 6) >> 6);
                uncond = true;
            }
            if (cond || uncond) {
                if (tgt < 0 || tgt >= n) {
                    if (!tail_ok || !scratch)
                        return false;
                    if (uncond)
                        break; /* tail call */
                    i++;
                    continue;
                }
                if (uncond) {
                    i = (int)tgt;
                    continue;
                }
                if (sp >= A64X_WALK_STACK)
                    return false;
                stack[sp++] = (int)tgt;
                i++;
                continue;
            }
            uint32_t vr, vw;
            a64x_vregs(e, &vr, &vw);
            if (vr & VM(v)) {
                return false;
            }
            if (vw & VM(v)) {
                break;
            }
            i++;
        }
    }
    return true;
}

/* V registers (from candidates) dead after fn[site]; fn = the whole function, branches out of it are tail calls */
uint32_t a64x_dead_vregs_flow(const uint32_t *fn, int n, int site, uint32_t candidates, int budget, bool whole_function,
                              const struct a64x_walk *walk)
{
    uint32_t dead = 0;
    if (n > 65536) {
        n = 65536;
    }
    if (n > (int)walk->words) {
        return 0;
    }
    for (uint32_t v = 0; v < 32; v++) {
        if ((candidates & VM(v)) && vflow_dead(fn, n, site, v, budget, walk, whole_function)) {
            dead |= VM(v);
        }
    }
    return dead;
}

/* for the host tests */
bool a64x_vregs_public(uint32_t w, uint32_t *vr, uint32_t *vw);

bool a64x_vregs_public(uint32_t w, uint32_t *vr, uint32_t *vw)
{
    return a64x_vregs(w, vr, vw);
}
