// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <functional>
#include <string>
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
    }

    uint64_t nowUs()
    {
        return now += 250;
    }

    void log(const char *line)
    {
        lines.emplace_back(line);
    }

    void fire()
    {
        armedMs = 0;
        fire_(context_);
    }

    void (*fire_)(void *) = nullptr;
    void *context_ = nullptr;
    bool locked = false;
    uint32_t armedMs = 0, wakes = 0;
    uint64_t now = 0;
    std::function<void()> meanwhile = [] {};
    std::vector<std::string> lines;
};

struct Engine {
    int onResult = 0, offResult = SWITCH_ENGINE_GATE_OFF;
    uint32_t ons = 0, offs = 0, drains = 0;

    static int on(void *self)
    {
        auto *e = static_cast<Engine *>(self);
        e->ons++;
        return e->onResult;
    }

    static int off(void *self, bool drain)
    {
        auto *e = static_cast<Engine *>(self);
        e->offs++;
        e->drains += drain;
        return drain ? SWITCH_ENGINE_GATE_OFF : e->offResult;
    }
};

struct Fixture {
    Engine engine;
    SwitchEngineGate<Model> gate;

    Fixture()
    {
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
