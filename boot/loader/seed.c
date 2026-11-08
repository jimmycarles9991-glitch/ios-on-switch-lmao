/* A per-boot /chosen/random-seed from SE2's DRBG: see seed.h. */
#include <stdint.h>
#include "seed.h"

enum {
    SE_SECURITY = 0x000,
    SE_OPERATION = 0x008,
    SE_INT_STATUS = 0x010,
    SE_CONFIG = 0x014,
    SE_IN_LL = 0x018,
    SE_OUT_LL = 0x024,
    SE_CRYPTO_CONFIG = 0x304,
    SE_LAST_BLOCK = 0x318,
    SE_RNG_CONFIG = 0x340,
    SE_RNG_SRC_CONFIG = 0x344,
    SE_STATUS = 0x800,
    SE_ERR_STATUS = 0x804,
};

#define SE_SECURITY_ENG_DIS        (1u << 1)
#define SE_SECURITY_SOFT_NONSECURE (1u << 16)
#define SE_OP_START                1u
#define SE_INT_OP_DONE             (1u << 4)
#define SE_INT_ERROR               (1u << 16)
#define SE_STATUS_BUSY             7u /* STATE (bits 1:0) not idle, or MEM_IF_BUSY (bit 2, T210B01) */
/* ENC_ALG RNG (2 << 12), ENC_MODE KEY192 (1 << 24), DEC_ALG NOP, DST MEMORY. */
#define SE_CONFIG_RNG 0x01002000u
/* INPUT_SEL RANDOM (1 << 3), XOR_BYPASS, CORE_SEL ENCRYPT (1 << 8), key index 0, original IV, AHB memory interface. */
#define SE_CRYPTO_RNG              0x00000108u
#define SE_RNG_SRC_ENTROPY         (1u << 2)
#define SE_RNG_FORCE_INSTANTIATION 1u
#define SE_RNG_FORCE_RESEED        2u
#define SE_RNG_RO_ENT_SRC          (1u << 1) /* bit 0 is its lock: never set here */

enum {
    CAR_RST_DEVICES_V = 0x358,
    CAR_RST_DEVICES_W = 0x35c,
    CAR_CLK_OUT_ENB_V = 0x360,
    CAR_CLK_OUT_ENB_W = 0x364,
    CAR_RST_DEV_W_CLR = 0x43c,
    CAR_CLK_ENB_W_SET = 0x448,
};

#define CAR_SE               (1u << 31) /* CLK_V_SE, shared by SE1, SE2 and PKA1 */
#define CAR_ENTROPY          (1u << 21) /* CLK_W_ENTROPY */
#define AHB_MEM_WRQUE_MST_ID 0x0fc
#define AHB_MASTER_SE2       (1u << 13) /* nvidia,ahb-master-id of se@70412000 (SE1 is 14) */

#define SEED_WORDS  (SWITCH_SEED_BYTES / 4u)
#define SEED_BLOCKS (SWITCH_SEED_BYTES / 16u)
#define SEED_OUT    16u

/* The output list (words 0..2) and the output (from word SEED_OUT), each in cache lines of their own. Written into
 * the raw image (link.ld: no .bss). The MMU is off when this runs, so the address is the physical one, below 4 GiB. */
#ifndef SWITCH_SEED_HOST_TEST
#define SEED_DATA __attribute__((section(".data")))
#else
#define SEED_DATA
#endif
static volatile uint32_t seed_dma[SEED_OUT + SEED_WORDS] __attribute__((aligned(64))) SEED_DATA;

#ifndef SWITCH_SEED_HOST_TEST
uint32_t switch_seed_read32(uint64_t pa)
{
    return *(const volatile uint32_t *)(uintptr_t)pa;
}

void switch_seed_write32(uint64_t pa, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)pa = value;
}

static void barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}
#else
volatile uint32_t *switch_seed_dma_words(void)
{
    return seed_dma;
}

static void barrier(void)
{}
#endif

static uint32_t se_read(uint32_t offset)
{
    return switch_seed_read32(SWITCH_SEED_SE2_PA + offset);
}

static void se_write(uint32_t offset, uint32_t value)
{
    switch_seed_write32(SWITCH_SEED_SE2_PA + offset, value);
}

static uint32_t car_read(uint32_t offset)
{
    return switch_seed_read32(SWITCH_SEED_CAR_PA + offset);
}

/* One DRBG operation: SWITCH_SEED_BYTES into seed_dma from SEED_OUT. Returns MIXED or the failure, with the register
 * in *detail. */
static uint32_t generate(uint32_t mode, uint64_t (*ticks)(void), uint64_t timeout, uint32_t *detail)
{
    uint32_t value = se_read(SE_STATUS);
    if (value & SE_STATUS_BUSY) {
        *detail = value;
        return SWITCH_SEED_STATUS_BUSY;
    }
    const uint32_t list = (uint32_t)(uintptr_t)seed_dma, output = (uint32_t)(uintptr_t)(seed_dma + SEED_OUT);
    seed_dma[0] = 0; /* last entry index */
    seed_dma[1] = output;
    seed_dma[2] = SWITCH_SEED_BYTES;
    for (unsigned i = SEED_OUT; i < SEED_OUT + SEED_WORDS; ++i)
        seed_dma[i] = 0;
    barrier();

    se_write(SE_CONFIG, SE_CONFIG_RNG);
    se_write(SE_CRYPTO_CONFIG, SE_CRYPTO_RNG);
    se_write(SE_RNG_CONFIG, SE_RNG_SRC_ENTROPY | mode);
    se_write(SE_RNG_SRC_CONFIG, se_read(SE_RNG_SRC_CONFIG) | SE_RNG_RO_ENT_SRC);
    se_write(SE_LAST_BLOCK, SEED_BLOCKS - 1u);
    se_write(SE_IN_LL, 0);
    se_write(SE_OUT_LL, list);
    se_write(SE_ERR_STATUS, se_read(SE_ERR_STATUS));
    se_write(SE_INT_STATUS, se_read(SE_INT_STATUS));
    /* An engine that drops our writes (secure-only after all) would never finish: check it took them. */
    value = se_read(SE_CONFIG);
    if (value != SE_CONFIG_RNG || se_read(SE_CRYPTO_CONFIG) != SE_CRYPTO_RNG || se_read(SE_OUT_LL) != list ||
        se_read(SE_LAST_BLOCK) != SEED_BLOCKS - 1u) {
        *detail = value;
        return SWITCH_SEED_STATUS_IGNORED;
    }
    if ((value = se_read(SE_ERR_STATUS)) != 0) {
        *detail = value;
        return SWITCH_SEED_STATUS_ERROR;
    }

    barrier();
    se_write(SE_OPERATION, SE_OP_START);
    const uint64_t start = ticks();
    while (!((value = se_read(SE_INT_STATUS)) & (SE_INT_OP_DONE | SE_INT_ERROR))) {
        if (ticks() - start > timeout) {
            *detail = value;
            return SWITCH_SEED_STATUS_TIMEOUT;
        }
    }
    const uint32_t error = se_read(SE_ERR_STATUS);
    se_write(SE_INT_STATUS, value);
    se_write(SE_ERR_STATUS, error);
    if (error || (value & SE_INT_ERROR)) {
        *detail = error ? error : value;
        return SWITCH_SEED_STATUS_ERROR;
    }
    /* T210B01: the output is in memory once the engine is idle, its memory interface quiet and the AHB write queue
     * drained (hekate _se_op_wait, L4T tegra_se_start_operation ahb_ack). */
    while (((value = se_read(SE_STATUS)) & SE_STATUS_BUSY) ||
           (switch_seed_read32(SWITCH_SEED_AHB_ARB_PA + AHB_MEM_WRQUE_MST_ID) & AHB_MASTER_SE2)) {
        if (ticks() - start > timeout) {
            *detail = value;
            return SWITCH_SEED_STATUS_DRAIN;
        }
    }
    barrier();
    return SWITCH_SEED_STATUS_MIXED;
}

/* Output that is all zero, or with two equal 16-byte blocks, is not what a working DRBG gives. */
static int output_plausible(void)
{
    uint32_t any = 0;
    for (unsigned i = SEED_OUT; i < SEED_OUT + SEED_WORDS; ++i)
        any |= seed_dma[i];
    if (!any)
        return 0;
    for (unsigned a = 0; a < SEED_BLOCKS; ++a) {
        for (unsigned b = a + 1; b < SEED_BLOCKS; ++b) {
            unsigned same = 0;
            for (unsigned w = 0; w < 4; ++w)
                same += seed_dma[SEED_OUT + 4 * a + w] == seed_dma[SEED_OUT + 4 * b + w];
            if (same == 4)
                return 0;
        }
    }
    return 1;
}

static uint32_t engine_seed(uint64_t (*ticks)(void), uint64_t timeout, uint32_t *detail, uint32_t *flags)
{
    /* A clock-gated or reset APB block can hang the bus: nothing in the engine is read unless its clock runs. */
    if (!(car_read(CAR_CLK_OUT_ENB_V) & CAR_SE) || (car_read(CAR_RST_DEVICES_V) & CAR_SE))
        return SWITCH_SEED_STATUS_SE_CLOCK;
    const uint32_t security = se_read(SE_SECURITY);
    if (!(security & SE_SECURITY_SOFT_NONSECURE) || (security & SE_SECURITY_ENG_DIS)) {
        *detail = security;
        return SWITCH_SEED_STATUS_SECURE;
    }
    if (!(car_read(CAR_CLK_OUT_ENB_W) & CAR_ENTROPY) || (car_read(CAR_RST_DEVICES_W) & CAR_ENTROPY)) {
        switch_seed_write32(SWITCH_SEED_CAR_PA + CAR_CLK_ENB_W_SET, CAR_ENTROPY);
        switch_seed_write32(SWITCH_SEED_CAR_PA + CAR_RST_DEV_W_CLR, CAR_ENTROPY);
        *flags |= SWITCH_SEED_FLAG_ENTROPY_ENABLED;
        const uint64_t start = ticks(); /* a few source cycles before the first use (Linux: udelay(2) after enable) */
        while (ticks() - start < timeout / 512u + 1u) {
        }
        const uint32_t clocks = car_read(CAR_CLK_OUT_ENB_W);
        if (!(clocks & CAR_ENTROPY) || (car_read(CAR_RST_DEVICES_W) & CAR_ENTROPY)) {
            *detail = clocks;
            return SWITCH_SEED_STATUS_ENTROPY;
        }
    }
    uint32_t status = generate(SE_RNG_FORCE_RESEED, ticks, timeout, detail);
    if (status == SWITCH_SEED_STATUS_ERROR)
        status = generate(SE_RNG_FORCE_INSTANTIATION, ticks, timeout, detail);
    if (status == SWITCH_SEED_STATUS_MIXED && !output_plausible())
        status = SWITCH_SEED_STATUS_OUTPUT;
    return status;
}

uint32_t switch_seed_mix(uint64_t seed_pa, uint64_t source_pa, uint64_t (*ticks)(void), uint64_t timeout)
{
    uint32_t detail = 0, flags = 0;
    const uint32_t status = engine_seed(ticks, timeout, &detail, &flags);
    const uint64_t now = ticks();
    if (status == SWITCH_SEED_STATUS_MIXED) {
        uint32_t fold = 0;
        for (unsigned i = 0; i < SEED_WORDS; ++i) {
            const uint32_t word = seed_dma[SEED_OUT + i];
            switch_seed_write32(seed_pa + 4 * i, switch_seed_read32(seed_pa + 4 * i) ^ word);
            fold ^= word;
        }
        detail = (fold ^ fold >> 16) & 0xffffu;
    }
    /* The counter too, whatever the engine did: a little variation even for the pinned seed. */
    switch_seed_write32(seed_pa, switch_seed_read32(seed_pa) ^ (uint32_t)now);
    switch_seed_write32(seed_pa + 4, switch_seed_read32(seed_pa + 4) ^ (uint32_t)(now >> 32));
    for (unsigned i = SEED_OUT; i < SEED_OUT + SEED_WORDS; ++i)
        seed_dma[i] = 0;
    const uint32_t word = status | flags | (detail & 0xffffu) << 16;
    switch_seed_write32(source_pa, SWITCH_SEED_MAGIC);
    switch_seed_write32(source_pa + 4, word);
    barrier();
    return word;
}
