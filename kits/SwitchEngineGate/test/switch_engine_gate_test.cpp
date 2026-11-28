// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../SwitchEngineGate.h"

namespace {

// One thread: the lock is a flag the gate must never take twice, sleep runs what the other thread would do meanwhile.
struct Model {
    bool start(void (*fire)(void *), void *context)
    {
        fire_ = fire;
        context_ = context;
        return true;
    }

    void lock()
    {
        EXPECT_FALSE(locked);
        locked = true;
        locks++;
    }

    void unlock()
    {
        locked = false;
    }

    void sleep(void *)
    {
        locked = false;
        meanwhile();
        locked = true;
    }

    void wake(void *)
    {
        wakes++;
    }

    void arm(uint32_t ms)
    {
        armedMs = ms;
        arms++;
    }

    uint64_t nowUs()
    {
        return now;
    }

    void log(const char *line)
    {
        lines.emplace_back(line);
    }

    void advanceUs(uint64_t deltaUs)
    {
        now += deltaUs;
    }

    void fire()
    {
        now += static_cast<uint64_t>(armedMs) * 1000;
        armedMs = 0;
        fire_(context_);
    }

    void (*fire_)(void *) = nullptr;
    void *context_ = nullptr;
    bool locked = false;
    uint32_t armedMs = 0, wakes = 0, locks = 0, arms = 0;
    uint64_t now = 1000000;
    std::function<void()> meanwhile = [] {};
    std::vector<std::string> lines;
};

struct Engine {
    int onResult = 0, offResult = SWITCH_ENGINE_GATE_OFF;
    uint32_t ons = 0, offs = 0, drains = 0;
    uint64_t *clock = nullptr;
    std::function<void()> duringOff = [] {};

    static int on(void *self)
    {
        auto *e = static_cast<Engine *>(self);
        e->ons++;
        if (e->clock)
            *e->clock += 250;
        return e->onResult;
    }

    static int off(void *self, bool drain)
    {
        auto *e = static_cast<Engine *>(self);
        e->offs++;
        e->drains += drain;
        e->duringOff();
        return drain ? SWITCH_ENGINE_GATE_OFF : e->offResult;
    }
};

struct Fixture {
    Engine engine;
    SwitchEngineGate<Model> gate;

    Fixture()
    {
        engine.clock = &gate.platform().now;
        EXPECT_TRUE(gate.start({"TegraTest", &engine, Engine::on, Engine::off}));
    }

    Model &model()
    {
        return gate.platform();
    }
};
}  // namespace

TEST(SwitchEngineGate, AnIdleEngineIsPoweredOffWhenTheArmedTimerFires)
{
    Fixture f;
    EXPECT_EQ(f.model().armedMs, 500u);
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 1u);
    EXPECT_FALSE(f.gate.powered());
    EXPECT_EQ(f.gate.stats().gates, 1u);
    ASSERT_EQ(f.model().lines.size(), 1u);
    EXPECT_EQ(f.model().lines[0], "TegraTest: idle power off: gates=1 ungates=0 resume=0us max=0us");
}

TEST(SwitchEngineGate, TheNextUsePowersItOnAndItsLeavingArmsTheTimer)
{
    Fixture f;
    f.model().fire();
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 1u);
    EXPECT_TRUE(f.gate.powered());
    EXPECT_EQ(f.gate.stats().ungates, 1u);
    EXPECT_EQ(f.gate.stats().lastResumeUs, 250u);
    EXPECT_EQ(f.model().armedMs, 0u);
    f.gate.done();
    EXPECT_EQ(f.model().armedMs, 500u);
    f.model().fire();
    EXPECT_EQ(f.model().lines.back(), "TegraTest: idle power off: gates=2 ungates=1 resume=250us max=250us");
}

TEST(SwitchEngineGate, APoweredEngineIsNotPoweredOnAgain)
{
    Fixture f;
    ASSERT_EQ(f.gate.use(), 0);
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 0u);
    f.gate.done();
    EXPECT_EQ(f.model().armedMs, 500u);
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 0u) << "a user still holds it";
    f.gate.done();
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 1u);
}

TEST(SwitchEngineGate, QueuedWorkKeepsItOnUntilTheQueueDrains)
{
    Fixture f;
    f.engine.offResult = SWITCH_ENGINE_GATE_BUSY;
    f.model().fire();
    EXPECT_TRUE(f.gate.powered());
    EXPECT_TRUE(f.model().lines.empty());
    EXPECT_EQ(f.model().armedMs, 0u) << "a busy engine is not polled";
    f.gate.idle();
    EXPECT_EQ(f.model().armedMs, 500u);
    f.engine.offResult = SWITCH_ENGINE_GATE_OFF;
    f.model().fire();
    EXPECT_FALSE(f.gate.powered());
}

// A failed power-on fails that use and holds nothing: every later use runs the normal power-on, with no wait, count or
// refusal for having failed before.
TEST(SwitchEngineGate, AFailedPowerOnFailsThatUseAndTheNextUsePowersOnAsUsual)
{
    Fixture f;
    f.model().fire();
    f.engine.onResult = -19;
    EXPECT_EQ(f.gate.use(), -19);
    EXPECT_FALSE(f.gate.powered());
    EXPECT_EQ(f.gate.use(), -19);
    EXPECT_EQ(f.engine.ons, 2u) << "the next use powered on at once, as any use of an engine that is off";
    f.engine.onResult = 0;
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 3u);
    EXPECT_TRUE(f.gate.powered());
    EXPECT_TRUE(f.model().lines.empty() || f.model().lines.back().find("again") == std::string::npos)
        << "no recovery line: the engine's on logs its own failures";
    f.gate.done();
    f.model().fire();
    EXPECT_FALSE(f.gate.powered()) << "gating goes on as before";
}

TEST(SwitchEngineGate, AnEngineItsDriverLostIsPoweredOnByTheNextUse)
{
    Fixture f;
    ASSERT_EQ(f.gate.use(), 0);
    f.gate.lost();
    EXPECT_FALSE(f.gate.powered());
    f.gate.done();
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 0u) << "a lost engine is already off";
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 1u);
    EXPECT_TRUE(f.gate.powered());
}

// Each refused power-off is its own line; the engine is parked unpowered, the next use powers it on and the next idle
// period powers it off as usual. Nothing counts the refusals.
TEST(SwitchEngineGate, ARefusedPowerOffIsOneLineAndTheNextIdlePeriodPowersOffAsUsual)
{
    Fixture f;
    f.engine.offResult = -5;
    f.model().fire();
    EXPECT_FALSE(f.gate.powered());
    ASSERT_EQ(f.model().lines.size(), 1u);
    EXPECT_EQ(f.model().lines[0],
              "TegraTest: idle power-off refused by the hardware (-5); parked unpowered, the next use powers it on");
    for (uint32_t i = 2; i <= 3; i++) {
        ASSERT_EQ(f.gate.use(), 0);
        EXPECT_EQ(f.engine.ons, i - 1);
        f.gate.done();
        f.model().fire();
        EXPECT_EQ(f.engine.offs, i) << "each idle period powers off as usual";
    }
    EXPECT_EQ(f.model().lines.size(), 3u) << "one line for each refusal";
    f.engine.offResult = SWITCH_ENGINE_GATE_OFF;
    ASSERT_EQ(f.gate.use(), 0);
    f.gate.done();
    f.model().fire();
    EXPECT_FALSE(f.gate.powered());
    EXPECT_EQ(f.model().lines.back().find("again"), std::string::npos);
}

TEST(SwitchEngineGate, SleepDrainsAndPowersOffAndResumeLeavesItOffWithoutAUser)
{
    Fixture f;
    f.gate.suspend();
    EXPECT_EQ(f.engine.drains, 1u);
    EXPECT_FALSE(f.gate.powered());
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 1u) << "nothing to do while asleep";
    f.gate.resume();
    EXPECT_EQ(f.engine.ons, 0u);
    EXPECT_EQ(f.model().wakes, 1u);
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 1u);
}

TEST(SwitchEngineGate, AUserHeldAcrossSleepGetsTheEngineBackAtResume)
{
    Fixture f;
    ASSERT_EQ(f.gate.use(), 0);
    f.gate.suspend();
    f.gate.resume();
    EXPECT_EQ(f.engine.ons, 1u);
    EXPECT_TRUE(f.gate.powered());
}

TEST(SwitchEngineGate, AUseDuringSleepWaitsForResume)
{
    Fixture f;
    f.gate.suspend();
    f.model().meanwhile = [&f] {
        f.gate.resume();
    };
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 1u);
    f.gate.suspend();
    f.model().meanwhile = [&f] {
        f.gate.resume();
    };
    f.gate.done();
    f.engine.onResult = -19;
    EXPECT_EQ(f.gate.use(), -19) << "an engine that did not come back from sleep";
    EXPECT_EQ(f.engine.ons, 2u) << "its use runs the normal power-on, which restores what the resume could not";
    f.engine.onResult = 0;
    ASSERT_EQ(f.gate.use(), 0);
    EXPECT_EQ(f.engine.ons, 3u) << "and the next one again, with no wait";
}

TEST(SwitchEngineGate, AHeldEngineIsNotPoweredOffWhenIdleUntilItsRelease)
{
    Fixture f;
    f.model().lock();
    f.gate.holdLocked();
    f.model().unlock();
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 0u);
    EXPECT_EQ(f.engine.ons, 0u) << "a hold powers nothing on";
    f.gate.release();
    EXPECT_EQ(f.model().armedMs, SwitchEngineGate<Model>::kIdleMs);
    f.model().fire();
    EXPECT_EQ(f.engine.offs, 1u);
}

TEST(SwitchEngineGate, SleepWaitsForAHoldsReleaseBeforeItPowersOff)
{
    Fixture f;
    f.model().lock();
    f.gate.holdLocked();
    f.model().unlock();
    uint32_t offsBeforeRelease = 99;
    f.model().meanwhile = [&f, &offsBeforeRelease] {
        offsBeforeRelease = f.engine.offs;
        f.gate.release();
    };
    f.gate.suspend();
    EXPECT_EQ(offsBeforeRelease, 0u);
    EXPECT_EQ(f.engine.drains, 1u);
    EXPECT_FALSE(f.gate.powered());
}

TEST(SwitchEngineGate, PoweredHotPathTakesNoLockAndDoesNotRearmTimer)
{
    Fixture f;
    const uint32_t locksBefore = f.model().locks;
    const uint32_t armsBefore = f.model().arms;
    for (int i = 0; i < 120; ++i) {
        f.model().advanceUs(16000);
        ASSERT_EQ(f.gate.use(), 0);
        f.gate.done();
        f.gate.idle();
    }
    EXPECT_EQ(f.model().locks, locksBefore) << "powered use/done/idle take no power lock";
    EXPECT_EQ(f.model().arms, armsBefore) << "timer already armed is not re-armed per frame";
    EXPECT_EQ(f.engine.ons, 0u);
    EXPECT_EQ(f.engine.offs, 0u);
    EXPECT_TRUE(f.gate.powered());
}

TEST(SwitchEngineGate, IdleTimerChecksLastActivityLazilyAndRearmsForRemainingTime)
{
    Fixture f;
    EXPECT_EQ(f.model().armedMs, 500u);
    const uint32_t armsAtStart = f.model().arms;

    f.model().advanceUs(300000);
    ASSERT_EQ(f.gate.use(), 0);
    f.gate.done();
    EXPECT_EQ(f.model().arms, armsAtStart);

    f.model().advanceUs(200000);
    f.model().armedMs = 0;
    f.gate.expire();
    EXPECT_TRUE(f.gate.powered());
    EXPECT_EQ(f.engine.offs, 0u);
    EXPECT_EQ(f.model().armedMs, 300u);
    EXPECT_EQ(f.model().arms, armsAtStart + 1);

    f.model().fire();
    EXPECT_FALSE(f.gate.powered());
    EXPECT_EQ(f.engine.offs, 1u);
}

TEST(SwitchEngineGate, IdleRacingWithBusyPowerOffArmsFreshTimer)
{
    Fixture f;
    f.engine.offResult = SWITCH_ENGINE_GATE_BUSY;
    f.engine.duringOff = [&f] {
        f.gate.idle();
    };
    f.model().fire();
    EXPECT_TRUE(f.gate.powered());
    EXPECT_EQ(f.model().armedMs, SwitchEngineGate<Model>::kIdleMs);
    f.engine.duringOff = [] {};
    f.engine.offResult = SWITCH_ENGINE_GATE_OFF;
    f.model().fire();
    EXPECT_FALSE(f.gate.powered());
}

namespace {
struct FrozenClockModel : Model {
    uint64_t fixed = 0;

    uint64_t nowUs()
    {
        return fixed;
    }

    void fireFrozen()
    {
        armedMs = 0;
        fire_(context_);
    }
};

struct ThreadedModel {
    bool start(void (*fire)(void *), void *context)
    {
        fire_ = fire;
        context_ = context;
        return true;
    }

    void lock() __attribute__((no_thread_safety_analysis))
    {
        mu_.lock();
    }

    void unlock() __attribute__((no_thread_safety_analysis))
    {
        mu_.unlock();
    }

    void sleep(void *) __attribute__((no_thread_safety_analysis))
    {
        std::unique_lock<std::mutex> lk(mu_, std::adopt_lock);
        cv_.wait(lk);
        lk.release();
    }

    void wake(void *)
    {
        cv_.notify_all();
    }

    void arm(uint32_t ms)
    {
        armedCount.fetch_add(1, std::memory_order_relaxed);
        lastArmedMs.store(ms, std::memory_order_relaxed);
    }

    uint64_t nowUs()
    {
        return clockUs.fetch_add(1000, std::memory_order_relaxed);
    }

    void log(const char *)
    {}

    void (*fire_)(void *) = nullptr;
    void *context_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<uint64_t> clockUs{1000000};
    std::atomic<uint32_t> armedCount{0}, lastArmedMs{0};
};
}  // namespace

TEST(SwitchEngineGate, FrozenClockModelsPowerOffWhenExpireRunsDirectly)
{
    for (uint64_t fixed : {uint64_t(0), uint64_t(1000000)}) {
        Engine engine;
        SwitchEngineGate<FrozenClockModel> gate;
        gate.platform().fixed = fixed;
        ASSERT_TRUE(gate.start({"TegraFrozen", &engine, Engine::on, Engine::off}));
        ASSERT_EQ(gate.use(), 0);
        gate.done();
        gate.platform().fireFrozen();
        EXPECT_FALSE(gate.powered()) << "fixed clock " << fixed;
        EXPECT_EQ(engine.offs, 1u);
    }
}

TEST(SwitchEngineGate, ConcurrentHotPathExpireAndSleepStayConsistent)
{
    Engine engine;
    SwitchEngineGate<ThreadedModel> gate;
    ASSERT_TRUE(gate.start({"TegraMT", &engine, Engine::on, Engine::off}));
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    for (int t = 0; t < 3; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 400; ++i) {
                ASSERT_EQ(gate.use(), 0);
                gate.done();
                if ((i & 3) == 0)
                    gate.idle();
            }
        });
    }
    std::thread timer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            gate.platform().clockUs.fetch_add(600000, std::memory_order_relaxed);
            gate.expire();
        }
    });
    std::thread sleeper([&] {
        for (int i = 0; i < 20; ++i) {
            gate.suspend();
            gate.resume();
        }
    });

    for (auto &w : workers)
        w.join();
    sleeper.join();
    stop.store(true, std::memory_order_release);
    timer.join();

    gate.platform().clockUs.fetch_add(1000000, std::memory_order_relaxed);
    gate.expire();
    EXPECT_FALSE(gate.powered());
}
