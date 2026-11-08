// kits/SwitchLinux's spinlock_t and rwlock_t on host threads: exclusion, Linux's preemption and interrupt-mask rules,
// reader nesting, and the panics for a retaken lock and one held past the spinlock timeout.
#include "kits/SwitchLinux/SwitchLinuxLock.h"
#include "switch_linux_lock_stubs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr int kThreads = 4;
constexpr int kRounds = 20000;

template <class Body> void onThreads(Body body)
{
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(body);
    for (std::thread &thread : threads)
        thread.join();
}

class SwitchLinuxLock : public ::testing::Test {
protected:
    void SetUp() override
    {
        switch_linux_model_set_irqs_on(1);
        switch_linux_model_tick_clock(0);
    }
};

using SwitchLinuxLockDeathTest = SwitchLinuxLock;

switch_linux_spinlock gStaticLock = SWITCH_LINUX_SPINLOCK_INIT;
}  // namespace

TEST_F(SwitchLinuxLock, ZeroedSpinlockIsUnlocked)
{
    EXPECT_FALSE(switch_linux_spin_is_locked(&gStaticLock));
    switch_linux_spin_lock(&gStaticLock);
    EXPECT_TRUE(switch_linux_spin_is_locked(&gStaticLock));
    switch_linux_spin_unlock(&gStaticLock);
    EXPECT_FALSE(switch_linux_spin_is_locked(&gStaticLock));
}

TEST_F(SwitchLinuxLock, SpinlockExcludesAcrossThreads)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    uint64_t count = 0;
    onThreads([&] {
        for (int i = 0; i < kRounds; ++i) {
            switch_linux_spin_lock(&lock);
            count = count + 1;
            switch_linux_spin_unlock(&lock);
        }
        EXPECT_EQ(switch_linux_model_preemption_level(), 0);
    });
    EXPECT_EQ(count, static_cast<uint64_t>(kThreads) * kRounds);
}

TEST_F(SwitchLinuxLock, SpinlockHoldsPreemptionOff)
{
    switch_linux_spinlock outer = SWITCH_LINUX_SPINLOCK_INIT;
    switch_linux_spinlock inner = SWITCH_LINUX_SPINLOCK_INIT;
    switch_linux_spin_lock(&outer);
    EXPECT_EQ(switch_linux_model_preemption_level(), 1);
    switch_linux_spin_lock(&inner);
    EXPECT_EQ(switch_linux_model_preemption_level(), 2);
    switch_linux_spin_unlock(&inner);
    switch_linux_spin_unlock(&outer);
    EXPECT_EQ(switch_linux_model_preemption_level(), 0);
    EXPECT_EQ(switch_linux_model_irqs_on(), 1);
}

TEST_F(SwitchLinuxLock, IrqsaveRestoresTheMaskItFound)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    for (int on : {1, 0}) {
        switch_linux_model_set_irqs_on(on);
        const unsigned long flags = switch_linux_spin_lock_irqsave(&lock);
        EXPECT_EQ(switch_linux_model_irqs_on(), 0);
        EXPECT_EQ(switch_linux_model_preemption_level(), 1);
        switch_linux_spin_unlock_irqrestore(&lock, flags);
        EXPECT_EQ(switch_linux_model_irqs_on(), on);
        EXPECT_EQ(switch_linux_model_preemption_level(), 0);
    }
}

// Linux's spin_unlock_irq enables interrupts whatever they were before spin_lock_irq.
TEST_F(SwitchLinuxLock, IrqFormsMaskThenUnmask)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    switch_linux_rwlock rw = SWITCH_LINUX_RWLOCK_INIT;
    switch_linux_model_set_irqs_on(0);
    switch_linux_spin_lock_irq(&lock);
    EXPECT_EQ(switch_linux_model_irqs_on(), 0);
    switch_linux_spin_unlock_irq(&lock);
    EXPECT_EQ(switch_linux_model_irqs_on(), 1);
    switch_linux_read_lock_irq(&rw);
    EXPECT_EQ(switch_linux_model_irqs_on(), 0);
    switch_linux_read_unlock_irq(&rw);
    EXPECT_EQ(switch_linux_model_irqs_on(), 1);
    switch_linux_write_lock_irq(&rw);
    EXPECT_EQ(switch_linux_model_irqs_on(), 0);
    switch_linux_write_unlock_irq(&rw);
    EXPECT_EQ(switch_linux_model_irqs_on(), 1);
    EXPECT_EQ(switch_linux_model_preemption_level(), 0);
}

TEST_F(SwitchLinuxLock, TrylockFailsWhileHeldElsewhere)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    switch_linux_spin_lock(&lock);
    std::thread other([&] {
        EXPECT_FALSE(switch_linux_spin_trylock(&lock));
        EXPECT_EQ(switch_linux_model_preemption_level(), 0);
    });
    other.join();
    switch_linux_spin_unlock(&lock);
    EXPECT_TRUE(switch_linux_spin_trylock(&lock));
    EXPECT_EQ(switch_linux_model_preemption_level(), 1);
    switch_linux_spin_unlock(&lock);
}

TEST_F(SwitchLinuxLockDeathTest, RetakingASpinlockPanics)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    EXPECT_DEATH(
        {
            switch_linux_spin_lock(&lock);
            switch_linux_spin_lock(&lock);
        },
        "panic: SwitchLinux: spinlock 0x[0-9a-f]+ taken again by its holder");
}

TEST_F(SwitchLinuxLockDeathTest, SpinlockHeldPastTheTimeoutPanics)
{
    switch_linux_spinlock lock = SWITCH_LINUX_SPINLOCK_INIT;
    EXPECT_DEATH(
        {
            switch_linux_model_tick_clock(1000000);
            switch_linux_spin_lock(&lock);
            std::thread([&] {
                switch_linux_spin_lock(&lock);
            }).join();
        },
        "panic: SwitchLinux: spinlock 0x[0-9a-f]+ held for over 250 ms by thread");
}

TEST_F(SwitchLinuxLock, ReadersNestAndShare)
{
    switch_linux_rwlock rw = SWITCH_LINUX_RWLOCK_INIT;
    switch_linux_read_lock(&rw);
    switch_linux_read_lock(&rw);
    std::thread([&] {
        switch_linux_read_lock(&rw);
        EXPECT_EQ(__atomic_load_n(&rw.state, __ATOMIC_RELAXED), 3u);
        switch_linux_read_unlock(&rw);
    }).join();
    switch_linux_read_unlock(&rw);
    switch_linux_read_unlock(&rw);
    EXPECT_EQ(rw.state, 0u);
    EXPECT_EQ(switch_linux_model_preemption_level(), 0);
}

TEST_F(SwitchLinuxLock, WriterWaitsForReadersToLeave)
{
    switch_linux_rwlock rw = SWITCH_LINUX_RWLOCK_INIT;
    std::atomic<bool> written{false};
    switch_linux_read_lock(&rw);
    std::thread writer([&] {
        switch_linux_write_lock(&rw);
        written = true;
        switch_linux_write_unlock(&rw);
    });
    for (int i = 0; i < 1000; ++i)
        std::this_thread::yield();
    EXPECT_FALSE(written.load());
    switch_linux_read_unlock(&rw);
    writer.join();
    EXPECT_TRUE(written.load());
}

TEST_F(SwitchLinuxLock, RwlockExcludesWritersFromEveryone)
{
    switch_linux_rwlock rw = SWITCH_LINUX_RWLOCK_INIT;
    uint64_t a = 0;
    uint64_t b = 0;
    std::atomic<int> torn{0};
    onThreads([&] {
        for (int i = 0; i < kRounds; ++i) {
            if (i % 4 == 0) {
                const unsigned long flags = switch_linux_write_lock_irqsave(&rw);
                a = a + 1;
                b = b + 1;
                switch_linux_write_unlock_irqrestore(&rw, flags);
            } else {
                const unsigned long flags = switch_linux_read_lock_irqsave(&rw);
                if (__atomic_load_n(&a, __ATOMIC_RELAXED) != __atomic_load_n(&b, __ATOMIC_RELAXED))
                    ++torn;
                switch_linux_read_unlock_irqrestore(&rw, flags);
            }
        }
        EXPECT_EQ(switch_linux_model_irqs_on(), 1);
    });
    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(a, static_cast<uint64_t>(kThreads) * (kRounds / 4));
}

TEST_F(SwitchLinuxLockDeathTest, WriterKeptOutPastTheTimeoutPanics)
{
    switch_linux_rwlock rw = SWITCH_LINUX_RWLOCK_INIT;
    EXPECT_DEATH(
        {
            switch_linux_model_tick_clock(1000000);
            switch_linux_read_lock(&rw);
            std::thread([&] {
                switch_linux_write_lock(&rw);
            }).join();
        },
        "panic: SwitchLinux: rwlock 0x[0-9a-f]+ busy for over 250 ms");
}
