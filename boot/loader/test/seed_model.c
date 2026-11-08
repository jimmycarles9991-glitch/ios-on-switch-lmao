/* boot/loader/seed.c against a model of what it touches: CAR's SE and ENTROPY clock/reset bits, SE2's registers
 * (security, config, lists, RNG, status, interrupt and error latches) with a DRBG that writes its output into the
 * output list's buffer when started, the AHB write queue, the counter, and the seed and 8-byte report in an ADT. Every
 * access seed.c makes goes through switch_seed_read32/write32 below; anything outside the model aborts.
 * seed_scenarios() is run by seed_test.cpp (BUILD.bazel builds seed.c with SWITCH_SEED_HOST_TEST). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../seed.h"
#include "bazel/testing/c_check.h"

volatile uint32_t *switch_seed_dma_words(void);

#define CAR       0x60006000u
#define AHB       0x6000c000u
#define SE2       0x70412000u
#define SEED_PA   0x80000100u
#define SOURCE_PA 0x80000400u
#define WORDS     (SWITCH_SEED_BYTES / 4u)

enum {
    SE_CLOCK_OFF = 1,
    SE_SECURE = 2,
    DROP_WRITES = 4,
    NEVER_DONE = 8,
    ERROR_FIRST = 16,
    ERROR_ALWAYS = 32,
    NEVER_DRAIN = 64,
    ZERO_OUTPUT = 128,
    ENTROPY_OFF = 256,
    ENTROPY_STUCK = 512,
    REPEAT_OUTPUT = 1024
};

static struct {
    unsigned flags;
    uint32_t clk_v, rst_v, clk_w, rst_w;
    uint32_t security, config, crypto, rng, rng_src, last, in_ll, out_ll, status, ints, err, wrque;
    uint32_t modes[4];
    unsigned starts, se_accesses, drains, busy_reads;
    uint64_t counter;
    uint32_t seed[WORDS], source[2];
    uint32_t drbg[WORDS];
} m;

static uint64_t ticks(void)
{
    return m.counter += 19;
} /* a microsecond per look at 19.2 MHz */

static void start(void)
{
    CHECK(m.config == 0x01002000u && m.crypto == 0x108u && m.last == SWITCH_SEED_BYTES / 16u - 1u && m.in_ll == 0);
    CHECK((m.rng & ~3u) == 4u);                   /* SRC_ENTROPY, a mode, nothing else */
    CHECK((m.rng_src & 2u) && !(m.rng_src & 1u)); /* RO_ENT_SRC on, its lock never set */
    CHECK(m.out_ll == (uint32_t)(uintptr_t)switch_seed_dma_words());
    volatile uint32_t *dma = switch_seed_dma_words();
    CHECK(dma[0] == 0 && dma[1] == (uint32_t)(uintptr_t)(dma + 16) && dma[2] == SWITCH_SEED_BYTES);
    for (unsigned i = 16; i < 16 + WORDS; ++i)
        CHECK(dma[i] == 0); /* cleared before the start */
    m.modes[m.starts < 4 ? m.starts : 3] = m.rng & 3u;
    ++m.starts;
    if (m.flags & NEVER_DONE)
        return;
    if ((m.flags & ERROR_ALWAYS) || ((m.flags & ERROR_FIRST) && m.starts == 1)) {
        m.ints |= 1u << 16;
        m.err = 0x40u;
        return;
    }
    for (unsigned i = 0; i < WORDS; ++i) {
        uint32_t word = (m.flags & ZERO_OUTPUT) ? 0 : 0x9e3779b9u * (i + 7u) ^ m.starts * 0x85ebca6bu;
        if (m.flags & REPEAT_OUTPUT)
            word = 0x1234u + (i & 3u);
        m.drbg[i] = word;
        dma[16 + i] = word;
    }
    m.ints |= 1u << 4;
    m.status = 4;       /* memory interface busy for a while */
    m.wrque = 1u << 13; /* and the AHB write queue */
    m.drains = 3;
}

uint32_t switch_seed_read32(uint64_t pa)
{
    if (pa >= SEED_PA && pa < SEED_PA + SWITCH_SEED_BYTES)
        return m.seed[(pa - SEED_PA) / 4];
    if (pa >= SOURCE_PA && pa < SOURCE_PA + 8)
        return m.source[(pa - SOURCE_PA) / 4];
    switch (pa) {
    case CAR + 0x358:
        return m.rst_v;
    case CAR + 0x35c:
        return m.rst_w;
    case CAR + 0x360:
        return m.clk_v;
    case CAR + 0x364:
        return (m.flags & ENTROPY_STUCK) ? m.clk_w & ~(1u << 21) : m.clk_w;
    case AHB + 0xfc:
        if (m.wrque && !(m.flags & NEVER_DRAIN) && !--m.drains)
            m.wrque = 0;
        return m.wrque;
    default:
        break;
    }
    CHECK(pa >= SE2 && pa < SE2 + 0x1000);
    CHECK((m.clk_v & (1u << 31)) && !(m.rst_v & (1u << 31))); /* never with the clock off or in reset */
    ++m.se_accesses;
    switch ((uint32_t)pa - SE2) {
    case 0x000:
        return m.security;
    case 0x010:
        return m.ints;
    case 0x014:
        return m.config;
    case 0x018:
        return m.in_ll;
    case 0x024:
        return m.out_ll;
    case 0x304:
        return m.crypto;
    case 0x318:
        return m.last;
    case 0x340:
        return m.rng;
    case 0x344:
        return m.rng_src;
    case 0x800: /* the memory interface goes quiet after a few looks, unless the model says never */
        if (m.status && !(m.flags & NEVER_DRAIN) && ++m.busy_reads >= 3)
            m.status = 0;
        return m.status;
    case 0x804:
        return m.err;
    default:
        abort();
    }
}

void switch_seed_write32(uint64_t pa, uint32_t value)
{
    if (pa >= SEED_PA && pa < SEED_PA + SWITCH_SEED_BYTES) {
        m.seed[(pa - SEED_PA) / 4] = value;
        return;
    }
    if (pa >= SOURCE_PA && pa < SOURCE_PA + 8) {
        m.source[(pa - SOURCE_PA) / 4] = value;
        return;
    }
    if (pa == CAR + 0x448) {
        CHECK(value == 1u << 21);
        m.clk_w |= value;
        return;
    } /* CLK_ENB_W_SET */
    if (pa == CAR + 0x43c) {
        CHECK(value == 1u << 21);
        m.rst_w &= ~value;
        return;
    } /* RST_DEV_W_CLR */
    CHECK(pa >= SE2 && pa < SE2 + 0x1000);
    CHECK((m.clk_v & (1u << 31)) && !(m.rst_v & (1u << 31)));
    CHECK(m.security & (1u << 16)); /* nothing written to a secure-only engine */
    ++m.se_accesses;
    if (m.flags & DROP_WRITES)
        return;
    switch ((uint32_t)pa - SE2) {
    case 0x008:
        CHECK(value == 1);
        start();
        return;
    case 0x010:
        m.ints &= ~value;
        return;
    case 0x014:
        m.config = value;
        return;
    case 0x018:
        m.in_ll = value;
        return;
    case 0x024:
        m.out_ll = value;
        return;
    case 0x304:
        m.crypto = value;
        return;
    case 0x318:
        m.last = value;
        return;
    case 0x340:
        m.rng = value;
        return;
    case 0x344:
        m.rng_src = value;
        return;
    case 0x804:
        m.err &= ~value;
        return;
    default:
        abort();
    }
}

static uint32_t pinned(unsigned i)
{
    return 0x11111111u * (i % 15u + 1u) ^ i << 24;
}

static void pin_seed(void)
{
    for (unsigned i = 0; i < WORDS; ++i)
        m.seed[i] = pinned(i);
}

static uint32_t run(unsigned flags)
{
    memset(&m, 0, sizeof(m));
    m.flags = flags;
    m.clk_v = (flags & SE_CLOCK_OFF) ? 0 : 1u << 31;
    m.clk_w = (flags & (ENTROPY_OFF | ENTROPY_STUCK)) ? 0 : 1u << 21;
    m.rst_w = (flags & ENTROPY_OFF) ? 1u << 21 : 0;
    m.security = (flags & SE_SECURE) ? 0x35u : 0x10035u; /* as read on hardware: 0x10035 */
    m.counter = 0x123456789ull;
    pin_seed();
    const uint32_t word = switch_seed_mix(SEED_PA, SOURCE_PA, ticks, 38400);
    CHECK(m.source[0] == SWITCH_SEED_MAGIC && m.source[1] == word);
    volatile uint32_t *dma = switch_seed_dma_words();
    for (unsigned i = 16; i < 16 + WORDS; ++i)
        CHECK(dma[i] == 0); /* nothing of the output stays in the image */
    return word;
}

/* The seed after a run: pinned ^ output (if mixed) ^ the counter in words 0 and 1. */
static void check_seed(int mixed)
{
    for (unsigned i = 2; i < WORDS; ++i)
        CHECK(m.seed[i] == (pinned(i) ^ (mixed ? m.drbg[i] : 0)));
    const uint32_t c0 = m.seed[0] ^ pinned(0) ^ (mixed ? m.drbg[0] : 0);
    const uint32_t c1 = m.seed[1] ^ pinned(1) ^ (mixed ? m.drbg[1] : 0);
    const uint64_t counter = (uint64_t)c1 << 32 | c0;
    CHECK(counter > 0x123456789ull && counter <= m.counter); /* a counter value read during the run */
}

unsigned seed_scenarios(void);

unsigned seed_scenarios(void)
{
    unsigned scenarios = 0;
    uint32_t w;

    w = run(0); /* the shipped case: clocks on, the DRBG answers */
    CHECK((w & 0xffffu) == SWITCH_SEED_STATUS_MIXED && m.starts == 1 && m.modes[0] == 2);
    check_seed(1);
    uint32_t fold = 0;
    for (unsigned i = 0; i < WORDS; ++i)
        fold ^= m.drbg[i];
    CHECK(w >> 16 == ((fold ^ fold >> 16) & 0xffffu));
    ++scenarios;

    w = run(ENTROPY_OFF); /* BL31 left the entropy clock off (SC7): the loader turns it on first */
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_MIXED && (w & SWITCH_SEED_FLAG_ENTROPY_ENABLED));
    CHECK((m.clk_w & (1u << 21)) && !(m.rst_w & (1u << 21)));
    check_seed(1);
    ++scenarios;

    w = run(ENTROPY_STUCK);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_ENTROPY && m.starts == 0);
    check_seed(0);
    ++scenarios;

    w = run(SE_CLOCK_OFF); /* not one engine register touched */
    CHECK((w & 0xffffu) == SWITCH_SEED_STATUS_SE_CLOCK && m.se_accesses == 0);
    check_seed(0);
    ++scenarios;

    w = run(SE_SECURE); /* read SE_SECURITY, nothing else */
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_SECURE && w >> 16 == 0x35u && m.se_accesses == 1);
    check_seed(0);
    ++scenarios;

    w = run(DROP_WRITES);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_IGNORED && m.starts == 0);
    check_seed(0);
    ++scenarios;

    w = run(NEVER_DONE); /* bounded: about 2 ms of counter, then the pinned seed */
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_TIMEOUT && m.starts == 1);
    CHECK(m.counter - 0x123456789ull < 38400u + 4000u);
    check_seed(0);
    ++scenarios;

    w = run(ERROR_FIRST); /* forced reseed errors (DRBG never instantiated): instantiate once instead */
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_MIXED && m.starts == 2 && m.modes[0] == 2 && m.modes[1] == 1);
    check_seed(1);
    ++scenarios;

    w = run(ERROR_ALWAYS);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_ERROR && w >> 16 == 0x40u && m.starts == 2);
    check_seed(0);
    ++scenarios;

    w = run(NEVER_DRAIN);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_DRAIN);
    check_seed(0);
    ++scenarios;

    w = run(ZERO_OUTPUT);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_OUTPUT);
    check_seed(0);
    ++scenarios;

    w = run(REPEAT_OUTPUT);
    CHECK((w & 0xffu) == SWITCH_SEED_STATUS_OUTPUT);
    check_seed(0);
    ++scenarios;

    /* Two boots (different engine output) give different seeds from the same pinned bytes. */
    w = run(0);
    uint32_t first[WORDS];
    memcpy(first, m.seed, sizeof(first));
    memset(&m, 0, sizeof(m));
    m.clk_v = 1u << 31;
    m.clk_w = 1u << 21;
    m.security = 0x10035u;
    m.counter = 0x123456789ull;
    m.starts = 5; /* the model's output depends on its start count */
    pin_seed();
    (void)switch_seed_mix(SEED_PA, SOURCE_PA, ticks, 38400);
    CHECK(memcmp(first + 2, m.seed + 2, sizeof(first) - 8) != 0);
    ++scenarios;

    return scenarios;
}
