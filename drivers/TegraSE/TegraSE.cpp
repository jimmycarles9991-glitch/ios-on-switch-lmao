// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE: the T210B01 Security Engines SE1 (0x70012000, SPI 58 = INTID 90) and SE2 (0x70412000, SPI 190 = INTID 222,
// L4T tegra210b01-soc-base.dtsi) as the kernel's AES and SHA engines.
//
// What runs here:
//   - Both engines behind one queue: a caller takes whichever is free (TegraSE.h), so two callers run in parallel.
//     The AppleKeyStore stand-in's boot probe shares them through tegra_se_claim.
//   - DMA straight from the caller's pages: a linked list (one {address, size} entry per physically contiguous run)
//     built from the kernel virtual range (ml_vtophys), cleaned/invalidated in place. The engine takes 32-bit
//     addresses: a buffer with any byte above 4 GiB goes through the engine's bounce buffer (256 KiB, below 4 GiB) and
//     is counted, as is anything bigger the engine cannot reach (then the CPU does it).
//   - Keyslots 1..13 of each engine as a key cache (0 is L4T's SRK slot, 14/15 Mariko's boot keys and the
//     AppleKeyStore stand-in's): a key already in a slot is not written again.
//   - Completion by interrupt. After the end interrupt the only wait is the bounded drain T210B01 needs before the
//     output is in memory (memory interface idle, AHB write queue empty: L4T ahb_ack, hekate _se_op_wait), which has
//     no interrupt of its own; TegraSecurityEngine.cpp does the same.
//   - AES-128/192/256 ECB, CBC, CTR, OFB (both directions), AES-CMAC, SHA-1/224/256/384/512 one-shot, and SHA-1/256/
//     384/512 compress from any state (the state loaded into HASH_RESULT, SHA_CONFIG continue, MSG_LEFT past the end
//     so the engine does not pad): that is what corecrypto's digest compress does, so XNU's own hashing (the
//     code-signing page hashes included) runs here through switch_digest_compress_hook (libkern/crypto/
//     register_crypto.c). Every check is still corecrypto's; only the block arithmetic moves.
//   - The DRBG: 64 bytes into the kernel PRNG (write_random) at start.
//   - Small, one-off, latency-bound work stays on the A57s' AES/SHA instructions (code-signing's 16 KiB page hashes on
//     page-in, short digests); from each algorithm's cutover up it goes to the engines. The cutover is 64 KiB; a debug
//     kernel replaces it with the crossover its timing run measures 90 s after start (1 KiB to 1 MiB, every algorithm,
//     each engine against the CPU): the smallest size at which every engine is no slower than the CPU.
//
// Correctness: known answers per algorithm on each engine at start; then the first 32 operations of each algorithm and
// one in 512 after are also done on the CPU and compared. A failed answer or a mismatch logs one line and moves that
// algorithm (or that engine) back to the CPU for the boot; the caller still gets the CPU's result. An engine that
// never ends an operation, or whose end interrupt does not arrive, is one line and off while it has never completed
// one; after it has (the path proven, DMA possibly in flight into a caller's pages) it is a one-line panic.
//
// Registers and sequences: Linux L4T drivers/crypto/tegra-se.c (NVIDIA; GPL-2.0) tegra_se_config_algo/config_crypto/
// config_sha/start_operation/read_hash_result/write_key_table with the tegra210b01 chip data; hekate bdk/sec/se.c and
// se_t210.h (CTCaer, naehrwert; GPL-2.0) for the key table and partial SHA; switchroot BL31 security_engine.c. Written
// from the register facts in those sources, no code copied.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccmode_impl.h>
#pragma clang diagnostic pop
#include "TegraSE.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern task_t kernel_task;
int write_random(void *buffer, u_int numBytes);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
void thread_deallocate(thread_t thread);
int get_preemption_level(void);
// libkern/crypto/register_crypto.c (xnu patch): XNU's SHA digests' compress offers its blocks here first.
extern int (*volatile switch_digest_compress_hook)(const struct ccdigest_info *original, ccdigest_state_t state,
                                                   size_t nblocks, const void *data);
extern const struct ccdigest_info *switch_digest_originals[4];  // corecrypto's SHA-1, -256, -384, -512
}

namespace {
constexpr uint64_t kAhbArbPA = 0x6000c000;

enum : uint32_t {
    SECURITY = 0x000,
    OPERATION = 0x008,
    INT_ENABLE = 0x00c,
    INT_STATUS = 0x010,
    CONFIG = 0x014,
    IN_LL = 0x018,
    OUT_LL = 0x024,
    HASH_RESULT = 0x030,
    SHA_CONFIG = 0x200,
    SHA_MSG_LENGTH = 0x204,
    SHA_MSG_LEFT = 0x214,
    PERKEY = 0x280,
    KEY_ACCESS = 0x284,
    CRYPTO_CONFIG = 0x304,
    CRYPTO_CTR = 0x308,
    LAST_BLOCK = 0x318,
    KEYTABLE_ADDR = 0x31c,
    KEYTABLE_DATA = 0x320,
    RNG_CONFIG = 0x340,
    RNG_SRC_CONFIG = 0x344,
    STATUS = 0x800,
    ERR_STATUS = 0x804,
    SPARE = 0x80c,
};

constexpr uint32_t kSoftNonsecure = 1u << 16, kEngineDisabled = 1u << 1;
constexpr uint32_t kIntDone = 1u << 4, kIntError = 1u << 16;
constexpr uint32_t kStatusBusy = 7;  // STATE not idle, or MEM_IF_BUSY (T210B01)
constexpr uint32_t kAhbWrque = 0x0fc;
constexpr uint32_t kCarRstV = 0x358, kCarClkV = 0x360, kCarSourceSe = 0x42c;
constexpr unsigned kClockSe = 127, kClockEntropy = 149;  // TEGRA210_CLK_SE (bank V bit 31), _ENTROPY (W bit 21)

// CONFIG: ENC_ALG << 12 (AES 1, RNG 2, SHA 3), DEC_ALG << 8 (AES 1), ENC_MODE << 24 / DEC_MODE << 16 (AES key size
// 0/1/2 = 128/192/256; SHA1 0, SHA224 4, SHA256 5, SHA384 6, SHA512 7), DST << 2 (memory 0, hash registers 1).
constexpr uint32_t kDstHash = 1u << 2;
// CRYPTO_CONFIG: HASH_ENB 0, XOR_POS << 1 (bypass 0, top 2, bottom 3), INPUT_SEL << 3 (AHB 0, RANDOM 1, AESOUT 2,
// LNR_CTR 3), VCTRAM_SEL << 5 (AHB 0, AESOUT 2, PREVAHB 3), IV_SEL << 7 (original 0), CORE_SEL << 8 (encrypt 1),
// CTR_CNTN << 11, KEY_INDEX << 24, MEMIF << 31 (AHB 0).
constexpr uint32_t kCryptoEncrypt = 1u << 8;

constexpr unsigned kEngines = 2, kFirstSlot = 1, kLastSlot = 13;
constexpr uint32_t kBounceBytes = 256 * 1024;
constexpr uint32_t kListBytes = 0x1000, kInList = 0x000, kOutList = 0x800, kScratch = 0xf80;  // in the list page
constexpr unsigned kMaxEntries = 220;
constexpr uint32_t kOpTimeoutMs = 200;
constexpr size_t kHookMinBytes = 1024;  // smaller compresses (finals, short updates) stay on the CPU
constexpr uint32_t kCheckFirst = 32;

struct Dma {
    switch_dma_buffer buffer;
    uint8_t *va;
    uint32_t pa;
};

struct Slot {
    uint8_t key[32];
    unsigned bytes;
    uint64_t used;
};

struct Engine {
    const char *name;
    vm_offset_t pa;
    uint32_t intid, ahbMaster;
    volatile uint8_t *regs;
    IOFilterInterruptEventSource *source;
    int event;
    volatile uint32_t seen, status, owner;
    bool up, busy, proven;
    uint16_t slotsUsable;  // bit per keyslot the engine lets EL1 update and use
    Slot slots[16];
    uint64_t clock;  // LRU stamp
    Dma list, bounceIn, bounceOut;
    uint64_t ops;
};

Engine gEngines[kEngines] = {
    {"SE1", 0x70012000, 32 + 58, 1u << 14, nullptr, nullptr, 0, 0, 0, 0, false, false, false, 0, {}, 0, {}, {}, {}, 0},
    {"SE2", 0x70412000, 32 + 190, 1u << 13, nullptr, nullptr, 0, 0, 0, 0, false, false, false, 0, {}, 0, {}, {}, {}, 0},
};
volatile uint8_t *gAhb;
IOWorkLoop *gLoop;
IOLock *gQueue;
volatile bool gSleeping;  // tegra_se_suspend to tegra_se_resume: callers take the CPU path
int gQueueEvent;
char gWhy[224];

// Algorithms, each on or off for the boot, with its counters.
enum Alg : unsigned {
    SHA1,
    SHA224,
    SHA256,
    SHA384,
    SHA512,
    ECB,
    CBC,
    CTR,
    OFB,
    CMAC,
    ALGS
};

const char *const kAlgName[ALGS] = {"sha1",    "sha224",  "sha256",  "sha384",  "sha512",
                                    "aes-ecb", "aes-cbc", "aes-ctr", "aes-ofb", "aes-cmac"};

struct AlgState {
    volatile bool on;
    volatile uint32_t ops;
    volatile uint64_t bytes, bounced;
};

AlgState gAlg[ALGS];
// The CPU keeps what is small and one-off (the A57s' AES/SHA instructions answer sooner); from this many bytes an
// algorithm goes to the engines. Compiled defaults (code-signing's 16 KiB page hashes and single TLS records stay on the
// CPU), replaced at a debug kernel's 90 s timing run by each algorithm's measured crossover.
volatile size_t gCutover[ALGS] = {65536, 65536, 65536, 65536, 65536, 65536, 65536, 65536, 65536, 65536};
// The driver's own known-answer and timing threads: every size to the engines, whatever the cutover.
thread_t volatile gForced[2];

bool forced()
{
    const auto *const self = current_thread();
    return gForced[0] == self || gForced[1] == self;
}

volatile bool gReady;  // bring-up done: other callers may use the engines

// Only `only` (or none) handed out by the queue, under its lock: the known answers and the timing run.
void forceOnly(const Engine *only)
{
    IOLockLock(gQueue);
    for (;;) {
        bool idle = true;
        for (const Engine &e : gEngines)
            idle &= !e.busy || e.owner != TEGRA_SE_OWNER_DRIVER;
        if (idle)
            break;
        uint64_t deadline = 0;
        clock_interval_to_deadline(1, kMillisecondScale, &deadline);
        IOLockSleepDeadline(gQueue, &gQueueEvent, deadline, THREAD_UNINT);
    }
    for (Engine &e : gEngines)
        e.busy = (only != nullptr) && &e != only;
    IOLockUnlock(gQueue);
}

volatile uint64_t gUnreachable, gUnsleepable;  // bytes left to the CPU: not mappable / caller cannot sleep
bool gStateLoad;                               // SHA compress from a loaded state works (the hook is installed)

// Work the engines declined for a reason of the caller's, done on the CPU instead: one error line per process and reason
// (kits/SwitchRefusal), and the traffic counters only a debug kernel's traffic line reads. The size cutovers are not
// counted here: below them the CPU is measured faster.
void tally(volatile uint64_t &counter, uint64_t bytes)
{
    if (&counter == &gUnsleepable) {
        switch_refusal("TegraSE", "cannot sleep", "%llu bytes done on the CPU: the caller cannot wait for an engine",
                       bytes);
    } else {
        switch_refusal("TegraSE", "unreachable buffer", "%llu bytes done on the CPU: a buffer the engines cannot map",
                       bytes);
    }
#if DEVELOPMENT || DEBUG
    __atomic_fetch_add(&counter, bytes, __ATOMIC_RELAXED);
#endif
}

uint32_t rd(const Engine &e, uint32_t offset)
{
    return switch_mmio_read32(e.regs, offset);
}

void wr(const Engine &e, uint32_t offset, uint32_t value)
{
    switch_mmio_write32(e.regs, offset, value);
}

bool clockOn()
{
    return switch_car_clock_enabled(kClockSe) && !switch_car_reset_asserted(kClockSe);
}

bool allocate(Dma *dma, uint32_t size)
{
    if (!switch_dma_alloc(&dma->buffer, size))
        return false;
    dma->va = dma->buffer.va;
    dma->pa = static_cast<uint32_t>(dma->buffer.pa);
    return true;
}

uint64_t tenths(uint64_t ticks)  // of a microsecond
{
    return switch_abs_to_ns(ticks) / 100;
}

// Primary interrupt context: an operation's end (or error) acknowledged, its thread woken. Nothing else.
// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter fixes the signature
bool filter(OSObject *, IOFilterInterruptEventSource *sender)
{
    for (Engine &e : gEngines) {
        if (e.source != sender || !e.regs)
            continue;
        const uint32_t status = rd(e, INT_STATUS);
        if (!(status & (kIntDone | kIntError)))
            return false;
        wr(e, INT_STATUS, status);
        e.status = status;
        e.seen = e.seen + 1;
        thread_wakeup(&e.event);
        return false;
    }
    return false;
}

void wipeSlots(Engine &e);

void engineOff(Engine &e, const char *what)
{
    if (clockOn())
        wipeSlots(e);  // no key stays in an engine that is off
    e.up = false;
    IOLog("TegraSE: %s off for this boot: %s\n", e.name, what);
}

// Starts what the caller programmed (lists in the list page) and sleeps until the end interrupt.
bool run(Engine &e, uint32_t in, uint32_t out)
{
    wr(e, IN_LL, in);
    wr(e, OUT_LL, out);
    wr(e, ERR_STATUS, rd(e, ERR_STATUS));
    wr(e, INT_STATUS, rd(e, INT_STATUS));
    if (rd(e, IN_LL) != in || rd(e, OUT_LL) != out) {
        snprintf(gWhy, sizeof gWhy, "list registers did not take (IN_LL %#x OUT_LL %#x)", rd(e, IN_LL), rd(e, OUT_LL));
        engineOff(e, gWhy);
        return false;
    }
    if (rd(e, STATUS) & kStatusBusy) {
        snprintf(gWhy, sizeof gWhy, "busy before start (STATUS %#x)", rd(e, STATUS));
        engineOff(e, gWhy);
        return false;
    }
    const uint32_t seen = e.seen;
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kOpTimeoutMs) * 1000000, &interval);
    assert_wait_deadline(&e.event, THREAD_UNINT, mach_absolute_time() + interval);
    switch_dcache_sync();  // completes mapSide's and the callers' cache maintenance before the engine starts
    wr(e, OPERATION, 1);
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
    if (result == THREAD_TIMED_OUT || e.seen == seen) {
        const uint32_t status = rd(e, INT_STATUS);
        char gic[160];
        switch_interrupt_describe(e.intid, gic, sizeof gic);
        const bool ended = (status & (kIntDone | kIntError)) != 0u;
        if (e.proven) {
            panic("TegraSE: %s %s in %u ms (INT_STATUS %#x STATUS %#x ERR %#x; %s)", e.name,
                  ended ? "end interrupt lost" : "operation never ended", kOpTimeoutMs, status, rd(e, STATUS),
                  rd(e, ERR_STATUS), gic);
        }
        snprintf(gWhy, sizeof gWhy, "%s in %u ms, never-booted path (INT_STATUS %#x STATUS %#x ERR %#x; %s)",
                 ended ? "end interrupt never delivered" : "operation never ended", kOpTimeoutMs, status, rd(e, STATUS),
                 rd(e, ERR_STATUS), gic);
        wr(e, INT_ENABLE, 0);
        engineOff(e, gWhy);
        return false;
    }
    const uint32_t error = rd(e, ERR_STATUS);
    if ((e.status & kIntError) || error) {
        wr(e, ERR_STATUS, error);
        snprintf(gWhy, sizeof gWhy, "operation error (INT_STATUS %#x ERR_STATUS %#x)", e.status, error);
        return false;  // the operation's algorithm decides (a programming problem, not the engine)
    }
    for (unsigned i = 0; (rd(e, STATUS) & kStatusBusy) || (switch_mmio_read32(gAhb, kAhbWrque) & e.ahbMaster); ++i) {
        if (i > 100000) {
            if (e.proven)
                panic("TegraSE: %s output never drained (STATUS %#x)", e.name, rd(e, STATUS));
            snprintf(gWhy, sizeof gWhy, "output never drained (STATUS %#x)", rd(e, STATUS));
            engineOff(e, gWhy);
            return false;
        }
    }
    e.proven = true;
#if DEVELOPMENT || DEBUG
    e.ops++;
#endif
    return true;
}

bool claim(Engine &e, uint32_t owner)
{
    return __sync_bool_compare_and_swap(&e.owner, 0u, owner) || e.owner == owner;
}

// Any free engine (both run in parallel for two callers); null when none is up.
Engine *acquire()
{
    IOLockLock(gQueue);
    for (;;) {
        if (gSleeping) {
            IOLockUnlock(gQueue);
            return nullptr;
        }
        bool anyUp = false;
        for (Engine &e : gEngines) {
            anyUp |= e.up;
            if (e.up && !e.busy && claim(e, TEGRA_SE_OWNER_DRIVER)) {
                e.busy = true;
                IOLockUnlock(gQueue);
                if (!clockOn()) {  // system sleep turned the clock off (BL31 SC7): never touch the engine then
                    IOLockLock(gQueue);
                    e.busy = false;
                    tegra_se_release(static_cast<unsigned>(&e - gEngines) + 1, TEGRA_SE_OWNER_DRIVER);
                    IOLockUnlock(gQueue);
                    return nullptr;
                }
                return &e;
            }
        }
        if (!anyUp) {
            IOLockUnlock(gQueue);
            return nullptr;
        }
        uint64_t deadline = 0;
        clock_interval_to_deadline(1, kMillisecondScale, &deadline);  // the AppleKeyStore probe does not wake us
        IOLockSleepDeadline(gQueue, &gQueueEvent, deadline, THREAD_UNINT);
    }
}

void giveBack(Engine *e)
{
    IOLockLock(gQueue);
    e->busy = false;
    __sync_bool_compare_and_swap(&e->owner, TEGRA_SE_OWNER_DRIVER, 0u);
    IOLockWakeup(gQueue, &gQueueEvent, true);
    IOLockUnlock(gQueue);
}

struct Side {
    const uint8_t *va;  // the caller's range
    size_t len;
    bool bounced;  // through the engine's bounce buffer
};

// Builds the list at `at` in the list page for [va, va + len): physically contiguous runs of the kernel mapping, or
// the bounce buffer if any byte is above 4 GiB (input copied in). False if neither works (the CPU does it).
bool mapSide(const Engine &e, uint32_t at, const void *va, size_t len, bool output, Side *side, const Dma &bounce)
{
    side->va = static_cast<const uint8_t *>(va);
    side->len = len;
    side->bounced = false;
    uint32_t *list = reinterpret_cast<uint32_t *>(e.list.va + at);
    unsigned n = 0;
    // An output that shares a cache line with other data goes through the bounce buffer: that line could be written
    // back over the engine's bytes, or its other bytes lost to the invalidate after.
    bool high = output && (((reinterpret_cast<uintptr_t>(va) | len) & 63) != 0u);
    for (size_t done = 0; done < len && !high;) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(va) + done;
        const size_t room = 4096 - (a & 4095);
        const size_t chunk = len - done < room ? len - done : room;  // the smallest mapping granule
        const uint64_t pa = ml_vtophys(a);
        if (!pa)
            return false;
        if (pa + chunk > 0x100000000ULL) {
            high = true;
            break;
        }
        if (n && list[1 + 2 * (n - 1)] + list[2 + 2 * (n - 1)] == static_cast<uint32_t>(pa) &&
            list[2 + 2 * (n - 1)] + chunk < 0xfffff0) {
            list[2 + 2 * (n - 1)] += static_cast<uint32_t>(chunk);
        } else {
            if (n == kMaxEntries) {
                high = true;  // too fragmented for one list: the bounce buffer, if it fits
                break;
            }
            list[1 + 2 * n] = static_cast<uint32_t>(pa);
            list[2 + 2 * n] = static_cast<uint32_t>(chunk);
            ++n;
        }
        done += chunk;
    }
    if (high) {
        if (len > kBounceBytes)
            return false;
        side->bounced = true;
        if (!output)
            memcpy(bounce.va, va, len);
        switch_dcache_clean_invalidate_lines(bounce.va, len);
        list[0] = 0;
        list[1] = bounce.pa;
        list[2] = static_cast<uint32_t>(len);
    } else {
        list[0] = n - 1;
        switch_dcache_clean_invalidate_lines(va, len);  // input to memory; output lines gone before the engine writes
    }
    // No barrier here: run()'s DSB before the start completes these with everything else the operation set up.
    switch_dcache_clean_invalidate_lines(list, 4 + 8 * (side->bounced ? 1 : n));
    return true;
}

void finishOutput(const Side &side, const Dma &bounce)
{
    if (side.bounced) {
        switch_dcache_invalidate(bounce.va, side.len);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): an output side is the caller's writable buffer
        memcpy(const_cast<uint8_t *>(side.va), bounce.va, side.len);
    } else {
        switch_dcache_invalidate(side.va, side.len);
    }
}

void writeQuad(const Engine &e, unsigned slot, unsigned quad, const uint8_t *bytes)
{
    uint32_t w[4];
    memcpy(w, bytes, 16);
    for (unsigned i = 0; i < 4; ++i) {
        wr(e, KEYTABLE_ADDR, slot << 4 | quad << 2 | i);
        wr(e, KEYTABLE_DATA, w[i]);
    }
}

// The slot holding `key`, written into the least recently used one if no slot has it.
unsigned keySlot(Engine &e, const uint8_t *key, unsigned bytes)
{
    unsigned victim = 0;
    uint64_t oldest = ~0ULL;
    for (unsigned s = kFirstSlot; s <= kLastSlot; ++s) {
        if (!(e.slotsUsable & (1u << s)))
            continue;
        Slot &slot = e.slots[s];
        if (slot.bytes == bytes && !memcmp(slot.key, key, bytes)) {
            slot.used = ++e.clock;
            return s;
        }
        if (slot.used < oldest) {
            oldest = slot.used;
            victim = s;
        }
    }
    if (!victim)
        return 0;
    Slot &slot = e.slots[victim];
    uint8_t padded[32] = {};
    memcpy(padded, key, bytes);
    writeQuad(e, victim, 0, padded);
    writeQuad(e, victim, 1, padded + 16);
    memcpy(slot.key, padded, 32);
    slot.bytes = bytes;
    slot.used = ++e.clock;
    bzero(padded, sizeof padded);
    return victim;
}

void wipeSlots(Engine &e)
{
    static const uint8_t zero[16] = {};
    for (unsigned s = kFirstSlot; s <= kLastSlot; ++s) {
        if (!(e.slotsUsable & (1u << s)) || !e.slots[s].bytes)
            continue;
        writeQuad(e, s, 0, zero);
        writeQuad(e, s, 1, zero);
        writeQuad(e, s, 2, zero);
        bzero(&e.slots[s], sizeof e.slots[s]);
    }
}

uint32_t keyMode(unsigned bytes)
{
    if (bytes == 32)
        return 2;
    return bytes == 24 ? 1 : 0;
}

// nblocks AES blocks from the lists. ECB/CBC in the given direction; CTR/OFB always the encrypt core.
bool aesRun(Engine &e, tegra_se_mode mode, bool encrypt, unsigned slot, unsigned keyBytes, const uint8_t *iv,
            size_t nblocks)
{
    const uint32_t km = keyMode(keyBytes);
    const bool decrypt = !encrypt && (mode == TEGRA_SE_ECB || mode == TEGRA_SE_CBC);
    wr(e, CONFIG, decrypt ? (1u << 8 | km << 16) : (1u << 12 | km << 24));
    uint32_t crypto = slot << 24;
    switch (mode) {
    case TEGRA_SE_ECB:
        crypto |= encrypt ? kCryptoEncrypt : 0;
        break;
    case TEGRA_SE_CBC:
        crypto |= encrypt ? (2u << 1 | 2u << 5 | kCryptoEncrypt) : (3u << 1 | 3u << 5);
        break;
    case TEGRA_SE_CTR:
        crypto |= 3u << 1 | 3u << 3 | kCryptoEncrypt | 1u << 11;
        break;
    case TEGRA_SE_OFB:
        crypto |= 3u << 1 | 2u << 3 | kCryptoEncrypt;
        break;
    }
    wr(e, CRYPTO_CONFIG, crypto);
    if (mode == TEGRA_SE_CTR) {
        uint32_t w[4];
        memcpy(w, iv, 16);
        for (unsigned i = 0; i < 4; ++i)
            wr(e, CRYPTO_CTR + 4 * i, w[i]);
    } else if (mode != TEGRA_SE_ECB) {
        writeQuad(e, slot, 2, iv);  // the original IV
    }
    wr(e, SPARE, mode == TEGRA_SE_CTR || mode == TEGRA_SE_OFB ? 1 : 0);  // L4T sets it for CTR and OFB
    wr(e, LAST_BLOCK, static_cast<uint32_t>(nblocks - 1));
    const bool ok = run(e, e.list.pa + kInList, e.list.pa + kOutList);
    wr(e, SPARE, 0);
    return ok;
}

uint32_t shaMode(unsigned alg)
{
    switch (alg) {
    case SHA1:
        return 0;
    case SHA224:
        return 4;
    case SHA256:
        return 5;
    case SHA384:
        return 6;
    default:
        return 7;
    }
}

void setLengths(const Engine &e, uint64_t lengthBits, uint64_t leftBits)
{
    wr(e, SHA_MSG_LENGTH, static_cast<uint32_t>(lengthBits));
    wr(e, SHA_MSG_LEFT, static_cast<uint32_t>(leftBits));
    wr(e, SHA_MSG_LENGTH + 4, static_cast<uint32_t>(lengthBits >> 32));
    wr(e, SHA_MSG_LEFT + 4, static_cast<uint32_t>(leftBits >> 32));
    for (unsigned i = 2; i < 4; ++i) {
        wr(e, SHA_MSG_LENGTH + 4 * i, 0);
        wr(e, SHA_MSG_LEFT + 4 * i, 0);
    }
}

// SHA over the in-list: a whole message (state null: the engine's initial value and padding), or len bytes (whole
// blocks) continued from `words` 32-bit state words loaded into HASH_RESULT, not padded, the new state read back.
bool shaRun(Engine &e, unsigned alg, size_t len, uint32_t *state, unsigned words)
{
    wr(e, CONFIG, 3u << 12 | shaMode(alg) << 24 | kDstHash);
    wr(e, CRYPTO_CONFIG, 0);
    if (!state) {
        setLengths(e, static_cast<uint64_t>(len) << 3, static_cast<uint64_t>(len) << 3);
        wr(e, SHA_CONFIG, 1);  // INIT_HASH
    } else {
        // hekate se_sha_hash_256_partial_update: MSG_LEFT past the data, so the engine stops at the end unpadded.
        setLengths(e, static_cast<uint64_t>(len - 1) << 3, static_cast<uint64_t>(len + 1) << 3);
        for (unsigned i = 0; i < words; ++i)
            wr(e, HASH_RESULT + 4 * i, state[i]);
        wr(e, SHA_CONFIG, 0);  // continue from HASH_RESULT
    }
    if (!run(e, e.list.pa + kInList, 0))
        return false;
    if (state) {
        for (unsigned i = 0; i < words; ++i)
            state[i] = rd(e, HASH_RESULT + 4 * i);
    }
    return true;
}

// The digest bytes of a finished SHA (L4T tegra_se_read_hash_result; 64-bit words come out low half first).
void shaDigest(const Engine &e, unsigned alg, uint8_t *out)
{
    static const unsigned bytes[] = {20, 28, 32, 48, 64};
    const unsigned n = bytes[alg] / 4;
    uint32_t w[16];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = __builtin_bswap32(rd(e, HASH_RESULT + 4 * i));
    if (alg >= SHA384) {
        for (unsigned i = 0; i < 16; i += 2) {
            const uint32_t x = w[i];
            w[i] = w[i + 1];
            w[i + 1] = x;
        }
    }
    memcpy(out, w, static_cast<size_t>(n) * 4);
}

// The CPU's answer (kernel corecrypto's AES modes, the A57s' AES instructions): the reference every check uses.
bool cpuAes(tegra_se_mode mode, bool encrypt, const uint8_t *key, unsigned keyBytes, const uint8_t *ivIn,
            const uint8_t *in, uint8_t *out, size_t len)
{
    if (!g_crypto_funcs)
        return false;
    static const uint8_t zero[16] = {};
    uint8_t iv[16];
    memcpy(iv, ivIn ? ivIn : zero, 16);
    bool ok = false;
    if (mode == TEGRA_SE_CTR) {
        const struct ccmode_ctr *m = g_crypto_funcs->ccaes_ctr_crypt;
        void *ctx = m ? IOMalloc(m->size) : nullptr;
        if (ctx) {
            ok = (m->init(m, static_cast<ccctr_ctx *>(ctx), keyBytes, key, iv) == 0) &&
                 (m->ctr(static_cast<ccctr_ctx *>(ctx), len, in, out) == 0);
            bzero(ctx, m->size);
            IOFree(ctx, m->size);
        }
        return ok;
    }
    if (mode == TEGRA_SE_CBC) {
        if (encrypt) {
            aes_encrypt_ctx c;
            ok = (aes_encrypt_key(key, static_cast<int>(keyBytes), &c) == 0) &&
                 (aes_encrypt_cbc(in, iv, static_cast<unsigned>(len / 16), out, &c) == 0);
            bzero(&c, sizeof c);
        } else {
            aes_decrypt_ctx c;
            ok = (aes_decrypt_key(key, static_cast<int>(keyBytes), &c) == 0) &&
                 (aes_decrypt_cbc(in, iv, static_cast<unsigned>(len / 16), out, &c) == 0);
            bzero(&c, sizeof c);
        }
        return ok;
    }
    const struct ccmode_ecb *m =
        mode == TEGRA_SE_ECB && !encrypt ? g_crypto_funcs->ccaes_ecb_decrypt : g_crypto_funcs->ccaes_ecb_encrypt;
    void *ctx = m ? IOMalloc(m->size) : nullptr;
    if (!ctx)
        return false;
    ok = (m->init(m, static_cast<ccecb_ctx *>(ctx), keyBytes, key) == 0);
    if (ok && mode == TEGRA_SE_ECB) {
        ok = (m->ecb(static_cast<ccecb_ctx *>(ctx), len / 16, in, out) == 0);
    } else if (ok) {  // OFB: the keystream is the IV encrypted again and again
        uint8_t block[16];
        for (size_t at = 0; ok && at < len; at += 16) {
            ok = (m->ecb(static_cast<ccecb_ctx *>(ctx), 1, iv, block) == 0);
            for (unsigned i = 0; i < 16 && at + i < len; ++i)
                out[at + i] = in[at + i] ^ block[i];
            memcpy(iv, block, 16);
        }
        bzero(block, sizeof block);
    }
    bzero(ctx, m->size);
    IOFree(ctx, m->size);
    return ok;
}

bool shouldCheck(unsigned alg)
{
#if DEVELOPMENT || DEBUG
    constexpr uint32_t kCheckEvery = 512;
    const uint32_t n = __atomic_fetch_add(&gAlg[alg].ops, 1, __ATOMIC_RELAXED);
    return n < kCheckFirst || n % kCheckEvery == 0;
#else
    // Boot known-answer and loaded-state qualification still run. Production requests use the qualified engine once;
    // a second full CPU encryption/hash is development diagnostics, not completion or hardware-error handling.
    (void)alg;
    return false;
#endif
}

void algOff(unsigned alg, const char *what)
{
    if (!gAlg[alg].on)
        return;
    gAlg[alg].on = false;
    IOLog("TegraSE: %s back on the CPU for this boot: %s\n", kAlgName[alg], what);
}

bool canSleep()
{
    return (ml_at_interrupt_context() == 0) && (ml_get_interrupts_enabled() != 0) && get_preemption_level() == 0;
}

void counted(unsigned alg, size_t len, const Side &a, const Side &b)
{
    tally(gAlg[alg].bytes, len);
    if (a.bounced || b.bounced)
        tally(gAlg[alg].bounced, len);
}

// whole (a multiple of 16) bytes on an engine; iv is the operation's starting IV/counter, not changed here.
int aesLocked(tegra_se_mode mode, bool encrypt, const uint8_t *key, unsigned keyBytes, const uint8_t *iv,
              const void *in, const void *out, size_t whole)
{
    const unsigned alg = ECB + static_cast<unsigned>(mode);
    Engine *e = acquire();
    if (!e)
        return -1;
    int result = -1;
    Side a;
    Side b;
    const unsigned slot = keySlot(*e, key, keyBytes);
    if (slot && mapSide(*e, kInList, in, whole, false, &a, e->bounceIn) &&
        mapSide(*e, kOutList, out, whole, true, &b, e->bounceOut)) {
        if (aesRun(*e, mode, encrypt, slot, keyBytes, iv, whole / 16)) {
            finishOutput(b, e->bounceOut);
            counted(alg, whole, a, b);
            result = 0;
        } else {
            result = -2;  // the engine ran (or tried): the output buffer may hold anything
            algOff(alg, gWhy);
        }
    } else {
        tally(gUnreachable, whole);
    }
    giveBack(e);
    return result;
}

void addCounter(uint8_t ctr[16], uint64_t blocks)
{
    for (int i = 15; i >= 0 && blocks; --i) {
        const unsigned sum = ctr[i] + static_cast<unsigned>(blocks & 0xff);
        ctr[i] = static_cast<uint8_t>(sum);
        blocks = (blocks >> 8) + (sum >> 8);
    }
}
}  // namespace

extern "C" int tegra_se_aes(tegra_se_mode mode, bool encrypt, const uint8_t *key, unsigned keyBytes, uint8_t *iv,
                            const void *in, void *out, size_t len)
{
    const unsigned alg = ECB + (unsigned)mode;
    if (mode > TEGRA_SE_OFB || (keyBytes != 16 && keyBytes != 24 && keyBytes != 32) || !len || !gAlg[alg].on ||
        (len < gCutover[alg] && !forced()) || (!gReady && !forced()) || !canSleep())
        return -1;
    const bool stream = mode == TEGRA_SE_CTR || mode == TEGRA_SE_OFB;
    if (!stream && (len & 15))
        return -1;
    const size_t whole = len & ~(size_t)15;
    const uint8_t *i8 = static_cast<const uint8_t *>(in);
    uint8_t *o8 = static_cast<uint8_t *>(out);
    uint8_t ivIn[16] = {};
    uint8_t lastIn[16] = {};
    if (mode != TEGRA_SE_ECB)
        memcpy(ivIn, iv, 16);
    if (whole)
        memcpy(lastIn, i8 + whole - 16, 16);
    // In place (or overlapping), a failed engine run would lose the input the CPU needs: keep it.
    const bool overlap = i8 < o8 + len && o8 < i8 + len;
    uint8_t *saved = nullptr;
    uint8_t *reference = nullptr;
    if (overlap && whole) {
        saved = static_cast<uint8_t *>(IOMalloc(whole));
        if (!saved)
            return -1;
    }
    if (saved)
        memcpy(saved, i8, whole);
    if (shouldCheck(alg)) {  // the CPU's answer first, from the untouched input
        reference = static_cast<uint8_t *>(IOMalloc(len));
        if (reference && !cpuAes(mode, encrypt, key, keyBytes, ivIn, i8, reference, len)) {
            IOFree(reference, len);
            reference = nullptr;
        }
    }
    int result = whole ? aesLocked(mode, encrypt, key, keyBytes, ivIn, in, out, whole) : 0;
    uint8_t next[16];  // the IV/counter after the whole blocks
    memcpy(next, ivIn, 16);
    if (!result) {
        if (mode == TEGRA_SE_CTR)
            addCounter(next, whole / 16);
        if (mode == TEGRA_SE_OFB && whole) {
            for (unsigned k = 0; k < 16; ++k)
                next[k] = o8[whole - 16 + k] ^ lastIn[k];
        }
        if (len > whole) {  // a CTR/OFB tail: one keystream block on the CPU
            uint8_t block[16];
            if (cpuAes(TEGRA_SE_ECB, true, key, keyBytes, nullptr, next, block, 16)) {
                for (size_t k = whole; k < len; ++k)
                    o8[k] = i8[k] ^ block[k - whole];
                if (mode == TEGRA_SE_CTR)
                    addCounter(next, 1);
                if (mode == TEGRA_SE_OFB)
                    memcpy(next, block, 16);
            } else {
                result = -2;
            }
            bzero(block, sizeof block);
        }
    }
    if (result == -2 && saved) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): in place, the input is the caller's output buffer
        memcpy(const_cast<uint8_t *>(i8), saved, whole);  // the CPU redoes it from this
    }
    if (!result && reference && memcmp(reference, o8, len)) {
        algOff(alg, "result differs from the CPU's");
        memcpy(o8, reference, len);  // the caller gets the CPU's
        if (mode == TEGRA_SE_OFB) {  // the CPU's keystream state: the IV encrypted once per block
            memcpy(next, ivIn, 16);
            for (size_t at = 0; at < len; at += 16)
                cpuAes(TEGRA_SE_ECB, true, key, keyBytes, nullptr, next, next, 16);
        }
    }
    if (!result) {
        switch (mode) {
        case TEGRA_SE_CBC:
            memcpy(iv, encrypt ? o8 + whole - 16 : lastIn, 16);
            break;
        case TEGRA_SE_CTR:
        case TEGRA_SE_OFB:
            memcpy(iv, next, 16);
            break;
        case TEGRA_SE_ECB:
            break;
        }
    }
    if (reference) {
        bzero(reference, len);
        IOFree(reference, len);
    }
    if (saved) {
        bzero(saved, whole);
        IOFree(saved, whole);
    }
    return result ? -1 : 0;
}

extern "C" int tegra_se_cmac(const uint8_t *key, unsigned keyBytes, const void *in, size_t len, uint8_t mac[16])
{
    if ((keyBytes != 16 && keyBytes != 24 && keyBytes != 32) || !gAlg[CMAC].on || (len < gCutover[CMAC] && !forced()) ||
        (!gReady && !forced()) || !canSleep())
        return -1;
    // Subkeys from L = AES_K(0) (NIST SP 800-38B 6.1), on the CPU: one block.
    uint8_t zero[16] = {};
    uint8_t l[16];
    uint8_t k[16];
    uint8_t last[16] = {};
    if (!cpuAes(TEGRA_SE_ECB, true, key, keyBytes, nullptr, zero, l, 16))
        return -1;
    const bool complete = (len != 0u) && ((len % 16) == 0u);
    for (unsigned round = 0; round < (complete ? 1u : 2u); ++round) {
        const uint8_t *from = round ? k : l;
        uint8_t carry = 0;
        uint8_t next[16];
        for (int i = 15; i >= 0; --i) {
            next[i] = (uint8_t)((uint32_t)from[i] << 1 | carry);
            carry = from[i] >> 7;
        }
        if (carry)
            next[15] ^= 0x87;
        memcpy(k, next, 16);
    }
    const size_t head = complete ? len - 16 : len - len % 16;  // whole blocks before the last
    const uint8_t *msg = static_cast<const uint8_t *>(in);
    memcpy(last, msg + head, len - head);
    if (!complete)
        last[len - head] = 0x80;
    for (unsigned i = 0; i < 16; ++i)
        last[i] ^= k[i];

    Engine *e = acquire();
    if (!e)
        return -1;
    int result = -1;
    Side a = {};
    const unsigned slot = keySlot(*e, key, keyBytes);
    bool mapped = (slot != 0u) && ((head == 0u) || mapSide(*e, kInList, msg, head, false, &a, e->bounceIn));
    if (mapped) {
        // The modified last block from the list page's scratch, after the caller's pages: one more list entry.
        uint32_t *list = reinterpret_cast<uint32_t *>(e->list.va + kInList);
        uint8_t *scratch = e->list.va + kScratch;
        memcpy(scratch, last, 16);
        const unsigned n = head ? list[0] + 1 : 0;
        if (n >= kMaxEntries) {
            mapped = false;
        } else {
            list[0] = n;
            list[1 + 2 * n] = e->list.pa + kScratch;
            list[2 + 2 * n] = 16;
            switch_dcache_clean_invalidate(e->list.va, kListBytes);
        }
    }
    if (mapped) {
        const uint32_t km = keyMode(keyBytes);
        wr(*e, CONFIG, 1u << 12 | km << 24 | kDstHash);
        wr(*e, CRYPTO_CONFIG, slot << 24 | 2u << 1 | 2u << 5 | kCryptoEncrypt | 1u);  // CBC encrypt, HASH_ENB
        writeQuad(*e, slot, 2, zero);
        wr(*e, LAST_BLOCK, (uint32_t)(head / 16));
        if (run(*e, e->list.pa + kInList, 0)) {
            uint32_t w[4];
            for (unsigned i = 0; i < 4; ++i)
                w[i] = rd(*e, HASH_RESULT + 4 * i);
            memcpy(mac, w, 16);
            tally(gAlg[CMAC].bytes, len);
            result = 0;
        } else {
            algOff(CMAC, gWhy);
        }
        bzero(e->list.va + kScratch, 16);
    } else {
        tally(gUnreachable, len);
    }
    giveBack(e);
    if (!result && shouldCheck(CMAC)) {  // CBC-MAC of the same blocks on the CPU
        uint8_t chain[16] = {};
        uint8_t block[16];
        for (size_t at = 0; at < head; at += 16) {
            for (unsigned i = 0; i < 16; ++i)
                block[i] = chain[i] ^ msg[at + i];
            cpuAes(TEGRA_SE_ECB, true, key, keyBytes, nullptr, block, chain, 16);
        }
        for (unsigned i = 0; i < 16; ++i)
            block[i] = chain[i] ^ last[i];
        cpuAes(TEGRA_SE_ECB, true, key, keyBytes, nullptr, block, chain, 16);
        if (memcmp(chain, mac, 16)) {
            algOff(CMAC, "result differs from the CPU's");
            memcpy(mac, chain, 16);
        }
    }
    bzero(l, sizeof l);
    bzero(k, sizeof k);
    bzero(last, sizeof last);
    return result;
}

extern "C" int tegra_se_digest(unsigned which, const void *in, size_t len, uint8_t *out)
{
    unsigned alg = ALGS;
    switch (which) {
    case 1:
        alg = SHA1;
        break;
    case 224:
        alg = SHA224;
        break;
    case 256:
        alg = SHA256;
        break;
    case 384:
        alg = SHA384;
        break;
    case 512:
        alg = SHA512;
        break;
    default:
        break;
    }
    if (alg == ALGS || !len || !gAlg[alg].on || (len < gCutover[alg] && !forced()) || (!gReady && !forced()) ||
        !canSleep())
        return -1;
    Engine *e = acquire();
    if (!e)
        return -1;
    int result = -1;
    Side a;
    if (mapSide(*e, kInList, in, len, false, &a, e->bounceIn)) {
        if (shaRun(*e, alg, len, nullptr, 0)) {
            shaDigest(*e, alg, out);
            counted(alg, len, a, a);
            result = 0;
        } else {
            algOff(alg, gWhy);
        }
    } else {
        tally(gUnreachable, len);
    }
    giveBack(e);
    return result;
}

namespace {
// switch_digest_compress_hook: corecrypto's compress of XNU's SHA-1/256/384/512, from any state, on an engine.
int compressHook(const struct ccdigest_info *original, ccdigest_state_t state, size_t nblocks, const void *data)
{
    unsigned alg = 0;
    if (original->output_size == 20) {
        alg = SHA1;
    } else if (original->output_size == 32 && original->block_size == 64) {
        alg = SHA256;
    } else if (original->block_size == 128) {
        alg = SHA512;  // SHA-384's compress is SHA-512's from SHA-384's initial value
    } else {
        return -1;
    }
    const size_t len = nblocks * original->block_size;
    if (len < kHookMinBytes || (len < gCutover[alg] && !forced()) || !gAlg[alg].on)
        return -1;
    if (!canSleep()) {
        tally(gUnsleepable, len);
        return -1;
    }
    const unsigned words = static_cast<unsigned>(original->state_size / 4);
    uint32_t *s = reinterpret_cast<uint32_t *>(state);
    uint32_t before[16];
    memcpy(before, s, static_cast<size_t>(words) * 4);
    Engine *e = acquire();
    if (!e)
        return -1;
    int result = -1;
    Side a;
    uint32_t next[16];
    memcpy(next, before, static_cast<size_t>(words) * 4);
    if (mapSide(*e, kInList, data, len, false, &a, e->bounceIn)) {
        if (shaRun(*e, alg, len, next, words)) {
            counted(alg, len, a, a);
            result = 0;
        } else {
            algOff(alg, gWhy);
        }
    } else {
        tally(gUnreachable, len);
    }
    giveBack(e);
    if (result)
        return result;
    if (shouldCheck(alg)) {
        original->compress(state, nblocks, data);  // the CPU's answer, into the caller's state
        if (memcmp(s, next, static_cast<size_t>(words) * 4))
            algOff(alg, "compress differs from the CPU's");
        return 0;
    }
    memcpy(s, next, static_cast<size_t>(words) * 4);
    return 0;
}

const uint8_t kKey256[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                             0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                             0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
const uint8_t kFipsPlain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
const uint8_t kFips[3][16] = {
    {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a},  // C.1 AES-128
    {0xdd, 0xa9, 0x7c, 0xa4, 0x86, 0x4c, 0xdf, 0xe0, 0x6e, 0xaf, 0x70, 0xa0, 0xec, 0x0d, 0x71, 0x91},  // C.2 AES-192
    {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89},  // C.3 AES-256
};
const uint8_t kKey[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                          0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
const uint8_t kPlain[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
                            0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
                            0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
                            0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
                            0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
const uint8_t kIv[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const uint8_t kCbc1[16] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
                           0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};  // SP 800-38A F.2.1 block 1
const uint8_t kOfb1[16] = {0x3b, 0x3f, 0xd9, 0x2e, 0xb7, 0x2d, 0xad, 0x20,
                           0x33, 0x34, 0x49, 0xf8, 0xe8, 0x3c, 0xfb, 0x4a};  // F.4.1 block 1
const uint8_t kCtrIv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                            0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
const uint8_t kCtr4[16] = {0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1,
                           0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee};  // F.5.1 block 4
const uint8_t kCmac16[16] = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
                             0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};  // RFC 4493 example 2
const uint8_t kCmac40[16] = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
                             0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};  // example 3
const uint8_t kAbc[5][16] = {
    {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c},  // SHA-1
    {0x23, 0x09, 0x7d, 0x22, 0x34, 0x05, 0xd8, 0x22, 0x86, 0x42, 0xa4, 0x77, 0xbd, 0xa2, 0x55, 0xb3},  // SHA-224
    {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23},  // SHA-256
    {0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b, 0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07},  // SHA-384
    {0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31},  // SHA-512
};

// Runs every known answer on one engine (forced onto it); returns the algorithms that answered right.
uint32_t knownAnswers(const Engine &only)
{
    gForced[0] = current_thread();
    forceOnly(&only);  // the queue hands out only this engine meanwhile
    uint32_t good = 0;
    uint8_t buf[64];
    uint8_t out[64];
    uint8_t iv[16];
    for (unsigned alg = SHA1; alg <= SHA512; ++alg) {
        static const unsigned which[] = {1, 224, 256, 384, 512};
        gAlg[alg].on = true;
        if (!tegra_se_digest(which[alg], "abc", 3, out) && !memcmp(out, kAbc[alg], 16))
            good |= 1u << alg;
    }
    gAlg[ECB].on = true;
    bool ecb = true;
    for (unsigned k = 0; k < 3; ++k) {
        ecb &= (tegra_se_aes(TEGRA_SE_ECB, true, kKey256, 16 + 8 * k, nullptr, kFipsPlain, out, 16) == 0) &&
               (memcmp(out, kFips[k], 16) == 0);
        ecb &= (tegra_se_aes(TEGRA_SE_ECB, false, kKey256, 16 + 8 * k, nullptr, kFips[k], out, 16) == 0) &&
               (memcmp(out, kFipsPlain, 16) == 0);
    }
    if (ecb)
        good |= 1u << ECB;
    gAlg[CBC].on = true;
    memcpy(iv, kIv, 16);
    bool cbc = (tegra_se_aes(TEGRA_SE_CBC, true, kKey, 16, iv, kPlain, buf, 64) == 0) && (memcmp(buf, kCbc1, 16) == 0);
    memcpy(iv, kIv, 16);
    cbc = cbc && (tegra_se_aes(TEGRA_SE_CBC, false, kKey, 16, iv, buf, out, 64) == 0) && (memcmp(out, kPlain, 64) == 0);
    if (cbc)
        good |= 1u << CBC;
    gAlg[CTR].on = true;
    memcpy(iv, kCtrIv, 16);
    if (!tegra_se_aes(TEGRA_SE_CTR, true, kKey, 16, iv, kPlain, out, 64) && !memcmp(out + 48, kCtr4, 16))
        good |= 1u << CTR;
    gAlg[OFB].on = true;
    memcpy(iv, kIv, 16);
    if (!tegra_se_aes(TEGRA_SE_OFB, true, kKey, 16, iv, kPlain, out, 64) && !memcmp(out, kOfb1, 16))
        good |= 1u << OFB;
    gAlg[CMAC].on = true;
    if (!tegra_se_cmac(kKey, 16, kPlain, 16, out) && !memcmp(out, kCmac16, 16) &&
        !tegra_se_cmac(kKey, 16, kPlain, 40, out) && !memcmp(out, kCmac40, 16))
        good |= 1u << CMAC;
    for (AlgState &state : gAlg) {
        state.on = false;
        state.ops = 0;
    }
    forceOnly(nullptr);
    gForced[0] = nullptr;
    return good;
}

// SHA compress from a loaded state, against corecrypto's compress of the same blocks from a state that is not the
// initial value: what the hook needs. Runs on one engine (forced) with gAlg on for the duration.
bool stateLoadWorks(const Engine &only, unsigned alg, const struct ccdigest_info *di)
{
    if (!di)
        return false;
    forceOnly(&only);
    static uint8_t data[4096];
    for (unsigned i = 0; i < sizeof data; ++i)
        data[i] = static_cast<uint8_t>(i * 167 + 13);
    uint32_t cpu[16] = {};
    uint32_t se[16];
    memcpy(cpu, di->initial_state, di->state_size);
    di->compress(reinterpret_cast<ccdigest_state_t>(cpu), 1, data);  // a state past the initial value
    memcpy(se, cpu, di->state_size);
    const size_t blocks = (sizeof data - di->block_size) / di->block_size;
    di->compress(reinterpret_cast<ccdigest_state_t>(cpu), blocks, data + di->block_size);
    gAlg[alg].on = true;
    gForced[0] = current_thread();
    gAlg[alg].ops = kCheckFirst;  // no cross-check inside: this is the check
    const bool ok = (compressHook(di, reinterpret_cast<ccdigest_state_t>(se), blocks, data + di->block_size) == 0) &&
                    (memcmp(se, cpu, di->state_size) == 0);
    gAlg[alg].on = false;
    gForced[0] = nullptr;
    gAlg[alg].ops = 0;
    forceOnly(nullptr);
    return ok;
}

bool engineUp(Engine &e)
{
    e.regs = switch_mmio_map(e.pa, 0x2000);
    if (!e.regs)
        return false;
    const uint32_t security = rd(e, SECURITY);
    const uint32_t perkey = rd(e, PERKEY);
    if (!(security & kSoftNonsecure) || (security & kEngineDisabled)) {
        IOLog("TegraSE: %s not usable from EL1 (SECURITY %#x), off\n", e.name, security);
        return false;
    }
    for (unsigned s = kFirstSlot; s <= kLastSlot; ++s) {
        const uint32_t access = rd(e, KEY_ACCESS + 4 * s);
        if ((perkey & (1u << s)) && (access & 0x4a) == 0x4a)
            e.slotsUsable |= static_cast<uint16_t>(1u << s);  // update, IV, use
    }
    if (!e.slotsUsable) {
        IOLog("TegraSE: %s has no keyslot EL1 may update (PERKEY %#x), off\n", e.name, perkey);
        return false;
    }
    if (!allocate(&e.list, kListBytes) || !allocate(&e.bounceIn, kBounceBytes) ||
        !allocate(&e.bounceOut, kBounceBytes)) {
        IOLog("TegraSE: %s: no DMA memory below 4 GiB, off\n", e.name);
        return false;
    }
    IOService *nub = switch_interrupt_nub(e.intid, false);  // sleeps until the GIC is up
    e.source =
        nub ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, nullptr, &filter, nub, 0) : nullptr;
    if (!e.source || gLoop->addEventSource(e.source) != kIOReturnSuccess) {
        IOLog("TegraSE: %s: cannot attach INTID %u, off\n", e.name, e.intid);
        return false;
    }
    wr(e, INT_STATUS, rd(e, INT_STATUS));
    wr(e, INT_ENABLE, kIntDone | kIntError);
    e.source->enable();
    e.up = true;
    return true;
}

bool rng(Engine &e, uint8_t out[64])
{
    if (!switch_car_clock_enabled(kClockEntropy) || switch_car_reset_asserted(kClockEntropy))
        return false;
    uint32_t *list = reinterpret_cast<uint32_t *>(e.list.va + kOutList);
    list[0] = 0;
    list[1] = e.bounceOut.pa;
    list[2] = 64;
    switch_dcache_clean_invalidate(list, 12);
    switch_dcache_clean_invalidate(e.bounceOut.va, 64);
    wr(e, CONFIG, 2u << 12 | 1u << 24);  // RNG, KEY192, memory
    wr(e, CRYPTO_CONFIG, 1u << 3 | kCryptoEncrypt);
    wr(e, RNG_CONFIG, 1u << 2 | 2u);  // SRC_ENTROPY | FORCE_RESEED
    wr(e, RNG_SRC_CONFIG, rd(e, RNG_SRC_CONFIG) | 1u << 1);
    wr(e, LAST_BLOCK, 3);
    if (!run(e, 0, e.list.pa + kOutList))
        return false;
    switch_dcache_invalidate(e.bounceOut.va, 64);
    memcpy(out, e.bounceOut.va, 64);
    bzero(e.bounceOut.va, 64);
    switch_dcache_clean_invalidate(e.bounceOut.va, 64);
    uint32_t any = 0;
    for (unsigned i = 0; i < 64; ++i)
        any |= out[i];
    bool repeat = false;
    for (unsigned a = 0; a < 4; ++a) {
        for (unsigned b = a + 1; b < 4; ++b)
            repeat |= memcmp(out + static_cast<size_t>(16) * a, out + static_cast<size_t>(16) * b, 16) == 0;
    }
    return (any != 0u) && !repeat;
}

#if DEVELOPMENT || DEBUG
struct Parallel {
    unsigned engine, rounds;
    uint8_t *buf;
    size_t len;
    volatile bool done;
};

void parallelWorker(void *arg, wait_result_t)
{
    Parallel *p = static_cast<Parallel *>(arg);
    gForced[1] = current_thread();
    uint8_t digest[32];
    for (unsigned r = 0; r < p->rounds; ++r)
        (void)tegra_se_digest(256, p->buf, p->len, digest);
    gForced[1] = nullptr;
    IOLockLock(gQueue);
    p->done = true;
    IOLockWakeup(gQueue, p, false);
    IOLockUnlock(gQueue);
    thread_terminate(current_thread());
}

struct Row {
    unsigned alg;
    tegra_se_mode mode;
    bool encrypt;
    unsigned keyBytes;
    const char *name;
};

const Row kRows[] = {
    {SHA1, TEGRA_SE_ECB, true, 0, "sha1"},
    {SHA224, TEGRA_SE_ECB, true, 0, "sha224"},
    {SHA256, TEGRA_SE_ECB, true, 0, "sha256"},
    {SHA384, TEGRA_SE_ECB, true, 0, "sha384"},
    {SHA512, TEGRA_SE_ECB, true, 0, "sha512"},
    {ECB, TEGRA_SE_ECB, true, 16, "aes128-ecb-enc"},
    {ECB, TEGRA_SE_ECB, false, 32, "aes256-ecb-dec"},
    {CBC, TEGRA_SE_CBC, true, 16, "aes128-cbc-enc"},
    {CBC, TEGRA_SE_CBC, false, 16, "aes128-cbc-dec"},
    {CTR, TEGRA_SE_CTR, true, 16, "aes128-ctr"},
    {CTR, TEGRA_SE_CTR, true, 32, "aes256-ctr"},
    {OFB, TEGRA_SE_OFB, true, 16, "aes128-ofb"},
    {CMAC, TEGRA_SE_ECB, true, 16, "aes128-cmac"},
};

// One call of `row` on whichever engine the queue hands out (the caller forces one), or on the CPU.
int engineCall(const Row &row, const uint8_t *in, uint8_t *out, size_t n)
{
    static const unsigned which[] = {1, 224, 256, 384, 512};
    uint8_t iv[16];
    memcpy(iv, kIv, 16);
    if (row.alg <= SHA512)
        return tegra_se_digest(which[row.alg], in, n, out);
    if (row.alg == CMAC)
        return tegra_se_cmac(kKey256, row.keyBytes, in, n, out);
    return tegra_se_aes(row.mode, row.encrypt, kKey256, row.keyBytes, iv, in, out, n);
}

void cpuCall(const Row &row, const uint8_t *in, uint8_t *out, size_t n)
{
    if (row.alg <= SHA512) {  // SHA-224 costs SHA-256's compress, SHA-384 SHA-512's
        unsigned original = 3;
        if (row.alg == SHA1) {
            original = 0;
        } else if (row.alg <= SHA256) {
            original = 1;
        }
        const struct ccdigest_info *di = switch_digest_originals[original];
        if (di) {
            uint32_t st[16];
            memcpy(st, di->initial_state, di->state_size);
            di->compress(reinterpret_cast<ccdigest_state_t>(st), n / di->block_size, in);
        }
        return;
    }
    if (row.alg == CMAC) {  // CBC-MAC: the same block chain as CBC encryption
        aes_encrypt_ctx c;
        aes_encrypt_key(kKey256, static_cast<int>(row.keyBytes), &c);
        aes_encrypt_cbc(in, kIv, static_cast<unsigned>(n / 16), out, &c);
        bzero(&c, sizeof c);
        return;
    }
    cpuAes(row.mode, row.encrypt, kKey256, row.keyBytes, kIv, in, out, n);
}

void release(Dma *dma)
{
    switch_dma_free(&dma->buffer);
    dma->va = nullptr;
    dma->pa = 0;
}

// Per algorithm and size: microseconds of the whole call on each engine (lists, cache maintenance, interrupt, wake)
// against the CPU (kernel corecrypto, input cold in the cache like the engine's), best of 5; then each algorithm's
// cutover set to the smallest size at which every engine is no slower than the CPU; then both engines at once.
void timing()
{
    static const uint32_t sizes[5] = {1024, 4096, 16384, 65536, 1048576};
    Dma in = {};
    Dma out = {};
    if (!allocate(&in, sizes[4]) || !allocate(&out, sizes[4])) {
        IOLog("TegraSE: timing run skipped: no 1 MiB DMA buffers below 4 GiB\n");
        release(&in);
        return;
    }
    for (uint32_t i = 0; i < sizes[4]; ++i)
        in.va[i] = static_cast<uint8_t>(i * 2654435761u >> 13);
    gForced[0] = current_thread();
    size_t cutover[ALGS] = {};
    for (const Row &row : kRows) {
        if (!gAlg[row.alg].on)
            continue;
        size_t rowCut = 0;
        for (const Engine &e : gEngines) {
            if (!e.up)
                continue;
            char line[256];
            int at = snprintf(line, sizeof line, "TegraSE: %s %s us vs cpu:", row.name, e.name);
            size_t engineCut = ~static_cast<size_t>(0);
            for (unsigned s = 0; s < 5; ++s) {
                const uint32_t n = sizes[s];
                uint64_t seBest = ~0ULL;
                uint64_t cpuBest = ~0ULL;
                for (unsigned rep = 0; rep < 5; ++rep) {
                    forceOnly(&e);
                    gAlg[row.alg].ops = kCheckFirst + 1;  // no cross-check inside the timed call
                    switch_dcache_clean_invalidate(in.va, n);
                    uint64_t t0 = mach_absolute_time();
                    const int r = engineCall(row, in.va, out.va, n);
                    const uint64_t t1 = mach_absolute_time() - t0;
                    forceOnly(nullptr);
                    if (r)
                        break;
                    if (t1 < seBest)
                        seBest = t1;
                    switch_dcache_clean_invalidate(in.va, n);
                    t0 = mach_absolute_time();
                    cpuCall(row, in.va, out.va, n);
                    const uint64_t c1 = mach_absolute_time() - t0;
                    if (c1 < cpuBest)
                        cpuBest = c1;
                }
                if (seBest <= cpuBest && engineCut == ~static_cast<size_t>(0))
                    engineCut = n;
                const uint64_t a = tenths(seBest);
                const uint64_t c = tenths(cpuBest);
                if (at > 0 && static_cast<size_t>(at) < sizeof line) {
                    at += snprintf(line + at, sizeof line - static_cast<size_t>(at), " %uK %llu.%llu vs %llu.%llu%s",
                                   n / 1024, a / 10, a % 10, c / 10, c % 10, s < 4 ? "," : "");
                }
            }
            IOLog("%s\n", line);
            if (engineCut > rowCut)
                rowCut = engineCut;  // every engine no slower
        }
        if (rowCut > cutover[row.alg])
            cutover[row.alg] = rowCut;  // the slowest variant of the algorithm decides
    }
    char line[256];
    int at = snprintf(line, sizeof line, "TegraSE: cutover (bytes; CPU below):");
    for (unsigned alg = 0; alg < ALGS; ++alg) {
        if (!gAlg[alg].on)
            continue;
        // Never faster at the sizes measured: only work past 1 MiB goes to the engines (bulk, not latency-bound).
        if (cutover[alg] == ~static_cast<size_t>(0)) {
            gCutover[alg] = 2 * static_cast<size_t>(sizes[4]);
        } else {
            gCutover[alg] = cutover[alg] ? cutover[alg] : sizes[4];
        }
        if (at > 0 && static_cast<size_t>(at) < sizeof line) {
            at += snprintf(line + at, sizeof line - static_cast<size_t>(at), " %s %zu%s", kAlgName[alg], gCutover[alg],
                           cutover[alg] == ~static_cast<size_t>(0) ? " (slower at every size)" : "");
        }
    }
    IOLog("%s\n", line);
    // Both engines at once: two threads hashing 1 MiB 16 times each, against one thread alone.
    if (gEngines[0].up && gEngines[1].up && gAlg[SHA256].on) {
        gAlg[SHA256].ops = kCheckFirst + 1;
        Parallel p = {1, 16, in.va, sizes[4], false};
        uint8_t digest[32];
        uint64_t t0 = mach_absolute_time();
        for (unsigned r = 0; r < 16; ++r)
            (void)tegra_se_digest(256, in.va, sizes[4], digest);
        const uint64_t one = mach_absolute_time() - t0;
        thread_t t = nullptr;
        t0 = mach_absolute_time();
        if (kernel_thread_start(parallelWorker, &p, &t) == KERN_SUCCESS) {
            thread_deallocate(t);
            for (unsigned r = 0; r < 16; ++r)
                (void)tegra_se_digest(256, in.va, sizes[4], digest);
            IOLockLock(gQueue);
            while (!p.done)
                IOLockSleep(gQueue, &p, THREAD_UNINT);
            IOLockUnlock(gQueue);
            const uint64_t two = mach_absolute_time() - t0;
            IOLog("TegraSE: sha256 1 MiB x 16: one caller %llu us, two callers %llu us for twice the work (SE1 %llu "
                  "ops, SE2 %llu ops so far)\n",
                  tenths(one) / 10, tenths(two) / 10, gEngines[0].ops, gEngines[1].ops);
        }
    }
    gForced[0] = nullptr;
    bzero(in.va, sizes[4]);
    release(&in);
    release(&out);
}

void stats(const char *when)
{
    char line[256];
    int at = snprintf(line, sizeof line, "TegraSE %s: MiB (bounced) by algorithm:", when);
    for (unsigned alg = 0; alg < ALGS; ++alg) {
        if (!gAlg[alg].bytes || at <= 0 || static_cast<size_t>(at) >= sizeof line)
            continue;
        at += snprintf(line + at, sizeof line - static_cast<size_t>(at), " %s %llu (%llu)", kAlgName[alg],
                       gAlg[alg].bytes >> 20, gAlg[alg].bounced >> 20);
    }
    if (at > 0 && static_cast<size_t>(at) < sizeof line) {
        snprintf(line + at, sizeof line - static_cast<size_t>(at),
                 "; to the CPU: unreachable %llu MiB, unsleepable %llu MiB", gUnreachable >> 20, gUnsleepable >> 20);
    }
    os_log(OS_LOG_DEFAULT, "%{public}s", line);
}
#endif

void seThread(void *, wait_result_t)
{
    gAhb = switch_mmio_map(kAhbArbPA, 0x1000);
    gLoop = IOWorkLoop::workLoop();
    gQueue = IOLockAlloc();
    if (!gAhb || !gLoop || !gQueue) {
        IOLog("TegraSE: off for this boot: cannot map CAR/AHB or allocate\n");
        return;
    }
    // A clock-gated or reset APB block can hang the bus: no engine register is read unless the clock runs.
    if (!clockOn()) {
        IOLog("TegraSE: off for this boot: SE clock off or in reset (CLK_OUT_ENB_V %#x RST_DEVICES_V %#x)\n",
              switch_car_read(kCarClkV), switch_car_read(kCarRstV));
        return;
    }
    uint32_t answered = 0;
    unsigned up = 0;
    char line[256];
    int at = snprintf(line, sizeof line, "TegraSE: clock source %#x;", switch_car_read(kCarSourceSe));
    for (unsigned which = 0; which < kEngines; ++which) {
        Engine &e = gEngines[which];
        if (!tegra_se_claim(which + 1, TEGRA_SE_OWNER_DRIVER, 10000)) {
            IOLog("TegraSE: %s still held by the AppleKeyStore probe after 10 s, off\n", e.name);
            continue;
        }
        if (!engineUp(e)) {
            tegra_se_release(which + 1, TEGRA_SE_OWNER_DRIVER);
            continue;
        }
        tegra_se_release(which + 1, TEGRA_SE_OWNER_DRIVER);
        const uint64_t t0 = mach_absolute_time();
        const uint32_t good = knownAnswers(e);
        const uint64_t us = tenths(mach_absolute_time() - t0) / 10;
        if (!e.up)
            continue;
        if (good != (1u << ALGS) - 1) {
            char missing[96] = "";
            for (unsigned alg = 0; alg < ALGS; ++alg) {
                if (good & (1u << alg))
                    continue;
                strlcat(missing, kAlgName[alg], sizeof missing);
                strlcat(missing, " ", sizeof missing);
            }
            IOLog("TegraSE: %s wrong known answers, those stay on the CPU: %s(%s)\n", e.name, missing, gWhy);
        }
        answered = up ? (answered & good) : good;  // an algorithm needs every engine that is up
        ++up;
        if (at > 0 && static_cast<size_t>(at) < sizeof line) {
            at +=
                snprintf(line + at, sizeof line - static_cast<size_t>(at),
                         " %s up, %u keyslots, known answers %#x in %llu us, "
                         "INTID %u (%u interrupts);",
                         e.name, __builtin_popcount(e.slotsUsable), good, us, e.intid, switch_interrupt_count(e.intid));
        }
    }
    if (!up) {
        IOLog("TegraSE: off for this boot: no engine up\n");
        return;
    }
    // The hook needs SHA compress from a loaded state; corecrypto's own compress is the reference.
    gStateLoad = true;
    for (const Engine &e : gEngines) {
        if (!e.up)
            continue;
        gStateLoad =
            gStateLoad && ((answered & (1u << SHA256)) != 0u) && stateLoadWorks(e, SHA256, switch_digest_originals[1]);
        gStateLoad =
            gStateLoad && ((answered & (1u << SHA512)) != 0u) && stateLoadWorks(e, SHA512, switch_digest_originals[3]);
        if ((answered & (1u << SHA1)) && !stateLoadWorks(e, SHA1, switch_digest_originals[0]))
            answered &= ~(1u << SHA1);
    }
    for (unsigned alg = 0; alg < ALGS; ++alg)
        gAlg[alg].on = ((answered & (1u << alg)) != 0u);
    uint8_t seed[64];
    bool reseeded = false;
    gForced[0] = current_thread();
    for (const Engine &e : gEngines) {
        if (reseeded || !e.up)
            continue;
        forceOnly(&e);
        Engine *got = acquire();
        forceOnly(nullptr);
        if (got) {  // held (busy and claimed) until giveBack
            reseeded = rng(*got, seed);
            giveBack(got);
        }
    }
    gForced[0] = nullptr;
    if (reseeded)
        write_random(seed, sizeof seed);
    bzero(seed, sizeof seed);
    gReady = true;
    if (gStateLoad)
        switch_digest_compress_hook = compressHook;
    IOLog("%s\n", line);
    IOLog("TegraSE: on the engines: %#x of %#x (sha1 sha224 sha256 sha384 sha512 ecb cbc ctr ofb cmac); XNU's SHA "
          "compress %s; kernel PRNG %s\n",
          answered, (1u << ALGS) - 1,
          gStateLoad ? "routed here (code-signing hashes included)" : "left on the CPU: state load did not answer",
          reseeded ? "reseeded from the DRBG" : "not reseeded (entropy clock off or DRBG output implausible)");

#if DEVELOPMENT || DEBUG
    IOSleep(90000);  // the timing run waits for the boot to be over
    if (clockOn())
        timing();
    for (;;) {
        stats(gStateLoad ? "traffic" : "traffic (hook off)");
        IOSleep(300000);
    }
#endif
}
}  // namespace

extern "C" bool tegra_se_claim(unsigned engine, uint32_t owner, uint32_t ms)
{
    if (engine < 1 || engine > kEngines)
        return false;
    Engine &e = gEngines[engine - 1];
    for (uint32_t waited = 0;; ++waited) {
        if (claim(e, owner))
            return true;
        if (waited >= ms)
            return false;
        IOSleep(1);
    }
}

extern "C" void tegra_se_release(unsigned engine, uint32_t owner)
{
    if (engine < 1 || engine > kEngines)
        return;
    (void)__sync_bool_compare_and_swap(&gEngines[engine - 1].owner, owner, 0u);
}

extern "C" bool tegra_se2_claim(uint32_t owner, uint32_t ms)
{
    return tegra_se_claim(2, owner, ms);
}

extern "C" void tegra_se2_release(uint32_t owner)
{
    tegra_se_release(2, owner);
}

extern "C" int tegra_se_start(void)
{
    thread_t t = nullptr;
    if (kernel_thread_start(seThread, nullptr, &t) != KERN_SUCCESS)
        return -1;
    thread_deallocate(t);
    return 0;
}

// System sleep (TegraSE.h). BL31 saves both engines' context at SYSTEM_SUSPEND and expects them idle; its result is
// only logged, never a refusal, so an operation still running then would corrupt the save silently.
namespace {
constexpr uint32_t kOwnerSleep = 0x534c50u;
constexpr uint32_t kSleepWaitMs = 500;
}  // namespace

extern "C" void tegra_se_suspend(void)
{
    if (!gQueue)
        return;
    gSleeping = true;
    for (unsigned i = 0; i < kEngines; ++i) {
        Engine &e = gEngines[i];
        if (!e.up)
            continue;
        if (!tegra_se_claim(i + 1, kOwnerSleep, kSleepWaitMs)) {
            IOLog("TegraSE: %s still owned (%u) at sleep; BL31's context save may see it busy\n", e.name, e.owner);
            continue;
        }
        IOLockLock(gQueue);
        for (uint32_t waited = 0; e.busy && waited < kSleepWaitMs; ++waited) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(1, kMillisecondScale, &deadline);
            IOLockSleepDeadline(gQueue, &gQueueEvent, deadline, THREAD_UNINT);
        }
        e.busy = true;
        IOLockUnlock(gQueue);
        e.source->disable();
    }
}

extern "C" void tegra_se_resume(void)
{
    if (!gQueue)
        return;
    for (unsigned i = 0; i < kEngines; ++i) {
        Engine &e = gEngines[i];
        if (!e.up || e.owner != kOwnerSleep)
            continue;
        if (clockOn()) {
            wr(e, INT_STATUS, rd(e, INT_STATUS));
            wr(e, INT_ENABLE, kIntDone | kIntError);
        }
        e.source->enable();
        IOLockLock(gQueue);
        e.busy = false;
        IOLockWakeup(gQueue, &gQueueEvent, false);
        IOLockUnlock(gQueue);
        tegra_se_release(i + 1, kOwnerSleep);
    }
    IOLockLock(gQueue);
    gSleeping = false;
    IOLockWakeup(gQueue, &gQueueEvent, false);
    IOLockUnlock(gQueue);
}
