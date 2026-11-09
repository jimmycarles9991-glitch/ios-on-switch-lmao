/*
 * a64x_refs.c: the code addresses code refers to, for the offline translators. Stub words go only into dead PAC checks
 * that nothing enters (a64x_dead_checks), and these are the entries code itself makes.
 */
#include "a64x.h"

static int64_t sext(uint64_t v, unsigned bits)
{
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

static int64_t adr_imm(uint32_t w)
{
    return sext(((uint64_t)(w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21);
}

uint32_t a64x_code_refs(const uint32_t *w, uint32_t n, uint32_t i, uint64_t va, uint64_t out[1])
{
    const uint32_t x = w[i];
    const uint64_t pc = va + 4ull * i;
    if ((x & 0x7C000000u) == 0x14000000u) { /* B, BL */
        out[0] = pc + (uint64_t)(sext(x & 0x3FFFFFFu, 26) * 4);
        return 1;
    }
    if ((x & 0xFF000010u) == 0x54000000u || (x & 0x7E000000u) == 0x34000000u) { /* B.cond, CBZ/CBNZ */
        out[0] = pc + (uint64_t)(sext((x >> 5) & 0x7FFFFu, 19) * 4);
        return 1;
    }
    if ((x & 0x7E000000u) == 0x36000000u) { /* TBZ/TBNZ */
        out[0] = pc + (uint64_t)(sext((x >> 5) & 0x3FFFu, 14) * 4);
        return 1;
    }
    if ((x & 0x9F000000u) == 0x10000000u) { /* ADR */
        out[0] = pc + (uint64_t)adr_imm(x);
        return 1;
    }
    if ((x & 0x9F000000u) == 0x90000000u) { /* ADRP, then ADD Xd, Xn, #imm of its register */
        const uint64_t page = (pc & ~0xFFFull) + (uint64_t)(adr_imm(x) * 4096);
        const uint32_t rd = x & 31;
        for (uint32_t k = i + 1; k < n && k <= i + 4; k++) {
            if ((w[k] & 0xFFC00000u) == 0x91000000u && ((w[k] >> 5) & 31) == rd) {
                out[0] = page + ((w[k] >> 10) & 0xFFF);
                return 1;
            }
        }
    }
    return 0;
}

bool a64x_jump_table(const uint32_t w[9], uint64_t br_va, uint64_t *table, uint64_t *base, uint64_t *count)
{
    if (w[8] != 0xD61F0200u || w[7] != 0x8B100230u || (w[6] & 0x9F00001Fu) != 0x10000011u || w[5] != 0xB8B07A30u ||
        (w[4] & 0xFFC003FFu) != 0x91000231u || (w[3] & 0x9F00001Fu) != 0x90000011u || w[2] != 0x9A9F9210u) {
        return false;
    }
    *count = 0;
    if ((w[1] & 0xFFC003FFu) == 0xF100021Fu) {
        *count = ((w[1] >> 10) & 0xFFF) + 1;
    } else if (w[1] == 0xEB11021Fu && (w[0] & 0xFFE0001Fu) == 0xD2800011u) {
        *count = ((w[0] >> 5) & 0xFFFF) + 1;
    }
    const uint64_t pc_adrp = br_va - 20, pc_adr = br_va - 8;
    *table = (pc_adrp & ~0xFFFull) + (uint64_t)(adr_imm(w[3]) * 4096) + ((w[4] >> 10) & 0xFFF);
    *base = pc_adr + (uint64_t)adr_imm(w[6]);
    return true;
}
