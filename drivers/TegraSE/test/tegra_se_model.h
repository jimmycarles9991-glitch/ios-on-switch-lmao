// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE.cpp against a register model of both engines, for host tests. The model completes an operation inside the
// OPERATION write by calling the driver's filter, unless that engine is set to hang: then the operation never ends,
// and the memory its lists named is recorded, since a dead engine may still write it. Buffers from switch_dma_alloc
// get physical addresses below 4 GiB (ml_vtophys finds them); other memory is high, so it goes through a bounce buffer.
#pragma once
#include <gtest/gtest.h>

#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../TegraSE.cpp"

namespace se_model {
constexpr uintptr_t kBase[kEngines] = {0x70012000, 0x70412000};

struct Engine {
    std::map<uint32_t, uint32_t> regs;
    unsigned started = 0;
    unsigned hangAt = 0;         // the operation (counted from 1) that never ends; 0: none by count
    uint32_t hangFromBytes = 0;  // the first operation whose input list is at least this long never ends; 0: none
    unsigned hung = 0;           // the operation that never ended
    unsigned startedAtHang[kEngines] = {};  // every engine's count when it did
    std::vector<uint32_t> crypto;
    unsigned errorAt = 0;         // the operation (counted from 1) that ends with an error interrupt
    unsigned busyAfterError = 0;  // STATUS reads that still show it busy after that error: writes still queued
    unsigned busyLeft = 0;
    bool listsDoNotTake = false;  // IN_LL reads back 0
    uint8_t fillOut = 0;          // if non-zero, overwrite output DMA buffers with this byte on completion
};

struct Mapping {
    uint8_t *va;
    uint32_t pa;
    size_t size;
};

struct Alias {
    uintptr_t va;
    uint32_t pa;
    size_t size;
};

struct Range {
    uint32_t pa, size;
};

Engine engines[kEngines];
std::vector<std::string> logged;
std::vector<Mapping> mappings;
std::vector<Alias> aliases;
std::vector<Range> given;  // what a hung engine's lists named
unsigned reused;           // later lists or frees that touched `given`
uint32_t nextPa = 0x80000000;
IOService nubs[kEngines];
void (*longSleep)(unsigned ms);  // a debug seThread's 90 s and 300 s sleeps; null: they return at once

int engineAt(uintptr_t address)
{
    for (unsigned i = 0; i < kEngines; ++i) {
        if (address >= kBase[i] && address < kBase[i] + 0x2000)
            return static_cast<int>(i);
    }
    return -1;
}

uint8_t *bytesAt(uint32_t pa, uint32_t size)
{
    for (const Mapping &m : mappings) {
        if (pa >= m.pa && pa + size <= m.pa + m.size)
            return m.va + (pa - m.pa);
    }
    return nullptr;
}

const uint32_t *listAt(uint32_t pa)
{
    for (const Mapping &m : mappings) {
        if (pa >= m.pa && pa < m.pa + m.size)
            return reinterpret_cast<const uint32_t *>(m.va + (pa - m.pa));
    }
    return nullptr;
}

bool touchesGiven(uint32_t pa, uint32_t size)
{
    for (const Range &r : given) {
        if (pa < r.pa + r.size && r.pa < pa + size)
            return true;
    }
    return false;
}

// The {address, size} entries of the list at `pa` (none for 0).
std::vector<Range> entries(uint32_t pa)
{
    std::vector<Range> out;
    const uint32_t *list = pa ? listAt(pa) : nullptr;
    if (!list)
        return out;
    for (uint32_t i = 0; i <= list[0]; ++i)
        out.push_back({list[1 + 2 * i], list[2 + 2 * i]});
    return out;
}

uint32_t read(Engine &engine, uint32_t offset)
{
    switch (offset) {
    case SECURITY:
        return kSoftNonsecure;
    case IN_LL:
        return engine.listsDoNotTake ? 0 : engine.regs[offset];
    case PERKEY:
        return 0x3ffe;
    case STATUS:
        if (engine.busyLeft) {
            --engine.busyLeft;
            return kStatusBusy;
        }
        return 0;
    case ERR_STATUS:
        return 0;
    default:
        break;
    }
    if (offset > KEY_ACCESS && offset <= KEY_ACCESS + 4 * kLastSlot)
        return 0xff;
    return engine.regs[offset];
}

void start(unsigned index)
{
    Engine &engine = engines[index];
    ++engine.started;
    std::vector<Range> lists = entries(engine.regs[IN_LL]);
    const std::vector<Range> out = entries(engine.regs[OUT_LL]);
    uint64_t inBytes = 0;
    for (const Range &r : lists)
        inBytes += r.size;
    lists.insert(lists.end(), out.begin(), out.end());
    for (const Range &r : lists)
        reused += touchesGiven(r.pa, r.size) ? 1u : 0u;
    const bool hang = engine.started == engine.hangAt || (engine.hangFromBytes && inBytes >= engine.hangFromBytes);
    if (hang && !engine.hung) {
        engine.hung = engine.started;
        for (unsigned i = 0; i < kEngines; ++i)
            engine.startedAtHang[i] = engines[i].started;
        given.insert(given.end(), lists.begin(), lists.end());
        return;
    }
    if (engine.started == engine.errorAt) {
        engine.regs[INT_STATUS] |= kIntError;
        engine.busyLeft = engine.busyAfterError;
    } else {
        if (engine.fillOut) {
            for (const Range &r : out) {
                if (uint8_t *dst = bytesAt(r.pa, r.size))
                    memset(dst, engine.fillOut, r.size);
            }
        }
        engine.regs[INT_STATUS] |= kIntDone;
    }
    gEngines[index].nub->fire();
}

void write(unsigned index, uint32_t offset, uint32_t value)
{
    Engine &engine = engines[index];
    if (offset == INT_STATUS) {
        engine.regs[INT_STATUS] &= ~value;
        return;
    }
    if (offset == CRYPTO_CONFIG)
        engine.crypto.push_back(value);
    if (offset == OPERATION && (value & 1u)) {
        start(index);
        return;
    }
    engine.regs[offset] = value;
}

// Runs `body` on a thread of its own, as seThread runs; false if it was still running after 5 s.
template <class Body> bool finishes(Body body)
{
    std::promise<void> done;
    auto finished = done.get_future();
    std::thread worker([&] {
        body();
        done.set_value();
    });
    const bool ended = finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!ended) {
        gSleeping = true;
        if (IOLock *q = __atomic_load_n(&gQueue, __ATOMIC_ACQUIRE)) {
            IOLockLock(q);
            IOLockWakeup(q, &gQueueEvent, false);
            IOLockUnlock(q);
        }
    }
    worker.join();
    return ended;
}

std::string log()
{
    std::string all;
    for (const auto &line : logged)
        all += line;
    return all;
}

size_t count(const std::string &text)
{
    size_t n = 0;
    for (const auto &line : logged)
        n += line.find(text) != std::string::npos ? 1 : 0;
    return n;
}
}  // namespace se_model

// TegraSESmmu.cpp's, which the model does not build: the engines' memory clients untranslated, as at reset.
unsigned gSmmuAttached[kEngines];  // attaches asked for, per engine
unsigned gSmmuOwnedAdded[kEngines];

tegra_se_smmu_attach_result tegra_se_smmu_attach(unsigned engine, const switch_dma_buffer *const *, unsigned,
                                                 char *line, size_t size)
{
    ++gSmmuAttached[engine];
    snprintf(line, size, "no SMMU in the register model");
    return TEGRA_SE_SMMU_UNTRANSLATED;
}

bool tegra_se_smmu_add_owned(unsigned engine, const switch_dma_buffer *)
{
    if (engine < kEngines)
        ++gSmmuOwnedAdded[engine];
    return true;
}

// Holds taken less holds released, per engine (an operation's lists are counted once each way), and releases made
// while the engine still showed busy.
int gSmmuHolds[kEngines];
unsigned gReleasedBusy;

bool tegra_se_smmu_hold_lists(unsigned engine, const uint32_t *a, const uint32_t *b)
{
    if (a)
        ++gSmmuHolds[engine];
    if (b)
        ++gSmmuHolds[engine];
    return true;
}

void tegra_se_smmu_release_lists(unsigned engine, const uint32_t *a, const uint32_t *b)
{
    if (b) {
        --gSmmuHolds[engine];
        gReleasedBusy += se_model::engines[engine].busyLeft != 0 ? 1u : 0u;
    }
    if (a) {
        --gSmmuHolds[engine];
        gReleasedBusy += se_model::engines[engine].busyLeft != 0 ? 1u : 0u;
    }
}

bool tegra_se_smmu_hold(unsigned engine, const uint32_t *list)
{
    return tegra_se_smmu_hold_lists(engine, list, nullptr);
}

void tegra_se_smmu_release(unsigned engine, const uint32_t *list)
{
    tegra_se_smmu_release_lists(engine, list, nullptr);
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const int index = se_model::engineAt(address);
    return index < 0 ? 0 : se_model::read(se_model::engines[index], static_cast<uint32_t>(address & 0x1fff));
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const int index = se_model::engineAt(address);
    if (index >= 0)
        se_model::write(static_cast<unsigned>(index), static_cast<uint32_t>(address & 0x1fff), value);
}

uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return pa;
}

boolean_t ml_at_interrupt_context(void)
{
    return 0;
}

boolean_t ml_get_interrupts_enabled(void)
{
    return 1;
}

vm_offset_t ml_vtophys(vm_offset_t va)
{
    for (const se_model::Alias &a : se_model::aliases) {
        if (va >= a.va && va < a.va + a.size)
            return a.pa + (va - a.va);
    }
    for (const se_model::Mapping &m : se_model::mappings) {
        const auto base = reinterpret_cast<vm_offset_t>(m.va);
        if (va >= base && va < base + m.size)
            return m.pa + (va - base);
    }
    return va;
}

int get_preemption_level(void)
{
    return 0;
}

int write_random(void *, u_int)
{
    return 0;
}

kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *)
{
    return KERN_FAILURE;
}

void thread_deallocate(thread_t)
{}

int (*volatile switch_digest_compress_hook)(const struct ccdigest_info *, ccdigest_state_t, size_t, const void *);
const struct ccdigest_info *switch_digest_originals[4];

bool switch_car_clock_enabled(unsigned)
{
    return true;
}

bool switch_car_reset_asserted(unsigned)
{
    return false;
}

uint32_t switch_car_read(uint32_t)
{
    return 0x100;
}

void switch_refusal(const char *, const char *, const char *, ...)
{}
}

namespace {
thread_local int tSelf;
thread_local event_t tWaitingOn;
thread_local bool tWoken;
}  // namespace

thread_t current_thread()
{
    return reinterpret_cast<thread_t>(&tSelf);
}

void thread_terminate(thread_t)
{}

wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t, uint64_t)
{
    tWaitingOn = event;
    tWoken = false;
    return THREAD_AWAKENED;
}

void thread_wakeup(event_t event)
{
    tWoken = tWoken || event == tWaitingOn;
}

wait_result_t thread_block(thread_continue_t)
{
    const bool woken = tWoken;
    tWaitingOn = nullptr;
    tWoken = false;
    return woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

// Exits 3 after the panic line, which a fresh-process test can expect.
void panic(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("panic: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
    std::_Exit(3);
}

void IOLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    se_model::logged.emplace_back(line);
}

void IOSleep(unsigned ms)
{
    if (ms >= 90000) {
        if (se_model::longSleep)
            se_model::longSleep(ms);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

IOService *switch_interrupt_nub(uint32_t intid, bool)
{
    return &se_model::nubs[intid == gEngines[1].intid ? 1 : 0];
}

uint32_t switch_interrupt_count(uint32_t)
{
    return 0;
}

void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size)
{
    snprintf(buffer, size, "INTID %u", intid);
}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    const size_t rounded = (size + 4095) & ~static_cast<size_t>(4095);
    *out = {};
    out->va = static_cast<uint8_t *>(aligned_alloc(4096, rounded));
    if (!out->va)
        return false;
    memset(out->va, 0, rounded);
    out->pa = se_model::nextPa;
    out->size = size;
    se_model::mappings.push_back({out->va, se_model::nextPa, rounded});
    se_model::nextPa += static_cast<uint32_t>(rounded);
    return true;
}

void switch_dma_free(switch_dma_buffer *buffer)
{
    for (auto m = se_model::mappings.begin(); m != se_model::mappings.end(); ++m) {
        if (m->va != buffer->va)
            continue;
        se_model::reused += se_model::touchesGiven(m->pa, static_cast<uint32_t>(m->size)) ? 1u : 0u;
        se_model::mappings.erase(m);
        break;
    }
    free(buffer->va);
    *buffer = {};
}
