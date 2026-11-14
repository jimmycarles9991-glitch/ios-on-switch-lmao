// SwitchGicV2.h on the host: the barriers ArmCpu's acknowledge loop issues per interrupt class, an SGI send, and
// ArmGIC's run of an SPI's vector with the line masked or unmasked, counted and placed against a model GIC.
#include "drivers/ArmGIC/SwitchGicV2.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
std::vector<std::string> gTrace;
thread_local bool tTracing = true;

void note(const std::string &event)
{
    if (tTracing)
        gTrace.push_back(event);
}

std::map<std::string, unsigned> barriers()
{
    std::map<std::string, unsigned> counts;
    for (const std::string &event : gTrace) {
        if (event.rfind("dmb", 0) == 0 || event.rfind("dsb", 0) == 0 || event == "isb")
            ++counts[event];
    }
    return counts;
}

std::vector<std::string> run(std::vector<uint32_t> pending)
{
    struct Cpu {
        std::deque<uint32_t> pending;

        uint32_t acknowledge()
        {
            const uint32_t next = pending.empty() ? 1023 : pending.front();
            if (!pending.empty())
                pending.pop_front();
            note("ack " + std::to_string(next & 0x3ff));
            return next;
        }

        void end(uint32_t acknowledge)
        {
            note("eoi " + std::to_string(acknowledge));
        }

        void timer()
        {
            note("timer");
        }

        void ipi(uint32_t)
        {
            note("ipi");
        }

        void other(uint32_t intid)
        {
            note("spi " + std::to_string(intid));
        }
    } cpu{{pending.begin(), pending.end()}};

    gTrace.clear();
    switchgic::acknowledgeLoop(cpu);
    return gTrace;
}

using Trace = std::vector<std::string>;
using Counts = std::map<std::string, unsigned>;

constexpr uint32_t kSgiFromCpu2 = (2u << 10) | 0u;
constexpr uint32_t kDisplay = 32 + 73;

struct Vector {
    volatile char interruptActive = 0;
    volatile char interruptDisabledSoft = 0;
    volatile char interruptDisabledHard = 0;
};
}  // namespace

// Noted, and kept as a real fence: the threaded test below runs on the host's own weak memory model.
void switchgic::hostBarrier(const char *instruction)
{
    note(instruction);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// The final GICC_IAR read that ends every loop: nothing is read on its account and nothing is ended.
TEST(SwitchGicBarriers, SpuriousReadEndsTheLoopWithNoBarrier)
{
    EXPECT_EQ(run({}), (Trace{"ack 1023"}));
}

// SGI 0: the sender stored its IPI state, then a store barrier, then GICD_SGIR. The read barrier after the acknowledge
// pairs with it (Linux gic_handle_irq's smp_rmb), so the handler reads that state; no device write needs ordering
// before the EOI.
TEST(SwitchGicBarriers, SgiReadsItsStateAfterAReadBarrierAndEndsWithout)
{
    EXPECT_EQ(run({kSgiFromCpu2}), (Trace{"ack 0", "dmb ishld", "ipi", "eoi 2048", "ack 1023"}));
}

// PPI 27: XNU's rtclock_intr re-arms the timer with an MSR to CNTV_TVAL_EL0 and no ISB (ml_set_decrementer, both
// trees); the ISB makes that write take effect, the timer's level drop with it, before the EOI. A DSB does not.
TEST(SwitchGicBarriers, TimerPpiEndsAfterAnIsb)
{
    EXPECT_EQ(run({27}), (Trace{"ack 27", "timer", "isb", "eoi 27", "ack 1023"}));
}

// An SPI: the driver's status clear (or ArmGIC's mask) reaches its device before the EOI, so a level line is not taken
// again.
TEST(SwitchGicBarriers, SpiEndsAfterAFullDsb)
{
    EXPECT_EQ(run({kDisplay}), (Trace{"ack 105", "spi 105", "dsb sy", "eoi 105", "ack 1023"}));
}

// A display period as P.md counts it on the frame core and its neighbours: 9 SGIs, 4 timer PPIs, 2 frame SPIs, each loop
// ended by a spurious read.
TEST(SwitchGicBarriers, AFramesInterruptsCostOneBarrierEach)
{
    gTrace.clear();
    Trace all;
    for (int i = 0; i < 9; ++i) {
        const Trace t = run({kSgiFromCpu2});
        all.insert(all.end(), t.begin(), t.end());
    }
    for (int i = 0; i < 4; ++i) {
        const Trace t = run({27});
        all.insert(all.end(), t.begin(), t.end());
    }
    const Trace spis = run({kDisplay, 32 + 158});
    all.insert(all.end(), spis.begin(), spis.end());
    gTrace = all;
    EXPECT_EQ(barriers(), (Counts{{"dmb ishld", 9}, {"isb", 4}, {"dsb sy", 2}}));
}

// Linux gic_ipi_send_mask: one store barrier before GICD_SGIR and none after. A sender that then waits for the target's
// answer (XNU's cpu_xcall, the boot cross-IPI qualification) reads normal memory, which the posted SGIR write does not
// hold up.
TEST(SwitchGicBarriers, AnSgiIsOneStoreBarrierAndTheWrite)
{
    gTrace.clear();
    switchgic::sendSgi([] {
        note("sgir");
    });
    EXPECT_EQ(gTrace, (Trace{"dmb ishst", "sgir"}));
}

// interruptActive against IOInterruptController::disableInterrupt: a full barrier between setting it and reading
// interruptDisabledSoft (the other side stores the flag, DMBs, then reads interruptActive). The clear is a release
// store. A line the handler soft-disabled is masked with no barrier of its own: the SPI's DSB before the EOI covers it.
TEST(SwitchGicBarriers, AVectorRunIsOneFullBarrier)
{
    Vector vector;
    gTrace.clear();
    switchgic::runVector(
        vector,
        [] {
            note("handler");
        },
        [] {
            note("mask");
        });
    EXPECT_EQ(gTrace, (Trace{"dmb ish", "handler"}));
    EXPECT_EQ(vector.interruptActive, 0);

    gTrace.clear();
    switchgic::runVector(
        vector,
        [&vector] {
            note("handler");
            vector.interruptDisabledSoft = 1;
        },
        [] {
            note("mask");
        });
    EXPECT_EQ(gTrace, (Trace{"dmb ish", "handler", "mask"}));
    EXPECT_EQ(vector.interruptDisabledHard, 1);
}

// XNU's enableInterrupt issues its own DSB before it calls enableVector (the driver's device clear before the unmask);
// nothing waits for the unmask itself. Masking from a thread keeps its DSB: the vector is torn down after it.
TEST(SwitchGicBarriers, UnmaskHasNoBarrierAndAThreadMaskKeepsItsDsb)
{
    gTrace.clear();
    switchgic::unmask([] {
        note("isenabler");
    });
    EXPECT_EQ(gTrace, (Trace{"isenabler"}));
    gTrace.clear();
    switchgic::maskFromThread([] {
        note("icenabler");
    });
    EXPECT_EQ(gTrace, (Trace{"icenabler", "dsb sy"}));
}

// disableInterrupt on another core against the vector's runs: once the disabler has seen interruptActive clear after
// setting interruptDisabledSoft, no handler starts.
TEST(SwitchGicBarriers, NoHandlerStartsAfterDisableInterruptReturns)
{
    Vector vector;
    std::atomic<bool> stop{false};
    std::atomic<bool> disabled{false};
    std::atomic<unsigned> late{0};
    std::atomic<unsigned> runs{0};
    std::thread core([&] {
        tTracing = false;
        while (!stop.load(std::memory_order_relaxed)) {
            switchgic::runVector(
                vector,
                [&] {
                    if (disabled.load(std::memory_order_relaxed))
                        late.fetch_add(1, std::memory_order_relaxed);
                    runs.fetch_add(1, std::memory_order_relaxed);
                },
                [] {});
            __atomic_store_n(&vector.interruptDisabledHard, 0, __ATOMIC_RELAXED);
        }
    });
    tTracing = false;
    for (int round = 0; round < 2000; ++round) {
        __atomic_store_n(&vector.interruptDisabledSoft, 1, __ATOMIC_RELAXED);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (__atomic_load_n(&vector.interruptActive, __ATOMIC_ACQUIRE)) {
        }
        disabled.store(true, std::memory_order_relaxed);
        for (int spin = 0; spin < 200; ++spin)
            std::this_thread::yield();
        disabled.store(false, std::memory_order_relaxed);
        __atomic_store_n(&vector.interruptDisabledSoft, 0, __ATOMIC_RELAXED);
        const unsigned before = runs.load(std::memory_order_relaxed);
        for (int spin = 0; spin < 10000 && runs.load(std::memory_order_relaxed) == before; ++spin)
            std::this_thread::yield();
    }
    stop.store(true);
    core.join();
    tTracing = true;
    EXPECT_EQ(late.load(), 0u) << "handlers started after disableInterrupt saw the vector idle";
    EXPECT_GT(runs.load(), 0u);
}
