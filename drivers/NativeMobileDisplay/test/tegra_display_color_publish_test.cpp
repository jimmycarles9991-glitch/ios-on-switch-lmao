// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraDisplayColorPublish.h with its writers on their own threads, against a model of DC A's CMU (LUT words that change
// the moment they are written, CMU_ENABLE latching at a frame start after GENERAL_ACT_REQ): the filter never reads a
// snapshot a setter is writing, two setter calls during one filter read leave the snapshot it reads alone, the enable
// never latches while the filter writes whole tables, no LUT word changes while the latched CMU scans, every
// generation's frame-end hold is released, and DC A ends on the newest tables. Built with ThreadSanitizer as well.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "../TegraDisplayColorPublish.h"

using namespace switch_color;

namespace {

std::atomic<int> gHolds{0};

struct Platform {
    static void hold()
    {
        ++gHolds;
    }

    static void release()
    {
        if (--gHolds < 0)
            throw std::runtime_error("a frame-end hold released twice");
    }

    static void deviceWritesDone()
    {}
};

using Unit = ColourPublication<Platform>;

// The writing thread's own hook before each register write (a pause, a frame start landing mid-write).
thread_local std::function<void(uint32_t)> tBeforeWrite;

// DC A's registers the colour unit touches. Writes are seen by the model's own thread-safe state.
struct DcModel {
    std::array<std::atomic<uint32_t>, 0x1000> words{};
    std::atomic<bool> assemblyEnable{false}, latchedEnable{false}, actRequest{false}, inBlank{true};
    std::atomic<unsigned> lutWritesScanned{0}, lutWrites{0}, enableWrites{0};

    struct Ref {
        DcModel *dc;
        uint32_t word;

        operator uint32_t() const
        {
            return dc->read(word);
        }

        Ref &operator=(uint32_t value)
        {
            dc->write(word, value);
            return *this;
        }
    };

    Ref operator[](uint32_t word)
    {
        return {this, word};
    }

    uint32_t read(uint32_t word)
    {
        if (word == kDcStateControl)
            return actRequest ? kGeneralActReq : 0u;
        if (word == kDcDispDbgTiming)
            return inBlank ? kDbgVBlank : 0u;
        return words[word];
    }

    void write(uint32_t word, uint32_t value)
    {
        if (tBeforeWrite)
            tBeforeWrite(word);
        if (word == kDcCmuLut1 || word == kDcCmuLut2) {
            ++lutWrites;
            if (latchedEnable)
                ++lutWritesScanned;
            const uint32_t at = (word == kDcCmuLut1 ? 0x2000u : 0x3000u) + (value & 0xfffu);
            luts[at & 0x3fffu] = value >> 16;
            return;
        }
        if (word == kDcDispColorControl) {
            ++enableWrites;
            assemblyEnable = (value & kCmuEnable) != 0u;
        } else if (word == kDcStateControl) {
            if (value & kGeneralActReq)
                actRequest = true;
            return;
        }
        words[word] = value;
    }

    // A frame start: a requested CMU_ENABLE latches.
    void boundary()
    {
        if (actRequest.exchange(false))
            latchedEnable = assemblyEnable.load();
    }

    // What DC A shows that `t` does not, or nothing.
    std::string differs(const Tables &t)
    {
        if (latchedEnable != t.enabled)
            return std::string("CMU_ENABLE latched ") + (latchedEnable ? "on" : "off");
        if (!t.enabled)
            return {};
        for (unsigned i = 0; i < kLut1Entries; ++i)
            if (luts[0x2000 + i] != t.lut1[i])
                return "LUT1 " + std::to_string(i);
        for (unsigned k = 0; k < kLut2Entries; ++k)
            if (luts[0x3000 + k] != t.lut2[k])
                return "LUT2 " + std::to_string(k);
        for (unsigned i = 0; i < kCscWords; ++i)
            if (words[kDcCmuCsc + i] != t.csc[i])
                return "CSC " + std::to_string(i);
        return {};
    }

    std::array<std::atomic<uint32_t>, 0x4000> luts{};
};

Inputs defaults()
{
    Inputs in;
    identityGamma(in.gamma);
    return in;
}

// A setter call as TegraDisplayColor makes one, under the setters' lock.
struct Setters {
    explicit Setters(Unit &publication) : unit(publication)
    {}

    Unit &unit;
    std::mutex lock;
    Inputs in = defaults();

    void set(const std::function<void(Inputs &)> &change, bool attached = true)
    {
        const std::lock_guard<std::mutex> hold(lock);
        change(in);
        Snapshot &next = unit.spare();
        build(in, next.tables);
        memset(next.palette, 0, sizeof(next.palette));
        unit.publish(false, false, attached);
    }
};

void inverted(Inputs &in)
{
    in.remapMode = in.remapMode == kRemapInverted ? kRemapNormal : kRemapInverted;
}

void warmer(Inputs &in)
{
    in.harmony.m[2][2] = in.harmony.m[2][2] == kOne ? kOne / 2 : kOne;
}

class ColourPublish : public testing::Test {
protected:
    Unit unit;
    DcModel dc;
    Setters setters{unit};

    void SetUp() override
    {
        gHolds = 0;
        build(defaults(), unit.initial().tables);
        unit.initialPublished();
        unit.reset(0);
    }

    void frame()
    {
        unit.frameEnd(dc);
        dc.boundary();
        unit.frameEndAction(dc, true);
    }

    void settle()
    {
        for (int i = 0; i < 12; ++i)
            frame();
    }
};

// The filter adopts a snapshot and, its CMU off, writes it whole; two setter calls land while it writes. Neither takes
// the snapshot it reads, and DC A gets exactly the tables it adopted.
TEST_F(ColourPublish, TwoSettersDuringOneFilterReadNeverReuseItsSnapshot)
{
    setters.set(warmer);
    const Tables adopted = unit.newest().tables;
    std::mutex m;
    std::condition_variable cv;
    bool writing = false, resume = false;
    const auto pauseAtLut = [&](uint32_t word) {
        if (word != kDcCmuLut1 || writing)
            return;
        std::unique_lock<std::mutex> hold(m);
        writing = true;
        cv.notify_all();
        cv.wait(hold, [&] {
            return resume;
        });
    };
    std::thread filter([&] {
        tBeforeWrite = pauseAtLut;
        unit.frameEnd(dc);
    });
    {
        std::unique_lock<std::mutex> hold(m);
        cv.wait(hold, [&] {
            return writing;
        });
    }
    const Snapshot *reading = &unit.newest();
    Snapshot copy;
    memcpy(&copy, reading, sizeof(copy));
    setters.set(inverted);
    EXPECT_NE(&unit.newest(), reading);
    setters.set(warmer);
    EXPECT_NE(&unit.newest(), reading);
    EXPECT_EQ(memcmp(&copy, reading, sizeof(copy)), 0) << "a setter wrote the snapshot the filter reads";
    {
        const std::lock_guard<std::mutex> hold(m);
        resume = true;
    }
    cv.notify_all();
    filter.join();
    for (unsigned i = 0; i < kLut1Entries; ++i)
        EXPECT_EQ(dc.luts[0x2000 + i].load(), adopted.lut1[i]) << "LUT1 " << i;
    for (unsigned k = 0; k < kLut2Entries; ++k)
        EXPECT_EQ(dc.luts[0x3000 + k].load(), adopted.lut2[k]) << "LUT2 " << k;
    settle();
    EXPECT_EQ(dc.differs(unit.newest().tables), "");
    EXPECT_EQ(gHolds.load(), 0);
}

// The action has decided to enable and checked the filter has not moved; the filter then adopts a LUT change. It must
// not write whole tables: the CMU is about to scan them.
TEST_F(ColourPublish, AnEnableUnderWayKeepsTheFilterOffTheLuts)
{
    setters.set(warmer);
    unit.frameEnd(dc);
    ASSERT_EQ(dc.enableWrites.load(), 0u);
    std::mutex m;
    std::condition_variable cv;
    bool enabling = false, resume = false;
    const auto pauseAtEnable = [&](uint32_t word) {
        if (word != kDcDispColorControl)
            return;
        std::unique_lock<std::mutex> hold(m);
        enabling = true;
        cv.notify_all();
        cv.wait(hold, [&] {
            return resume;
        });
    };
    std::thread action([&] {
        tBeforeWrite = pauseAtEnable;
        unit.frameEndAction(dc, true);
    });
    {
        std::unique_lock<std::mutex> hold(m);
        cv.wait(hold, [&] {
            return enabling;
        });
    }
    setters.set(inverted);
    const unsigned luts = dc.lutWrites;
    unit.frameEnd(dc);
    EXPECT_EQ(dc.lutWrites.load(), luts) << "whole tables written while the CMU was being enabled";
    {
        const std::lock_guard<std::mutex> hold(m);
        resume = true;
    }
    cv.notify_all();
    action.join();
    EXPECT_TRUE(unit.actionState().enableAsked);
    settle();
    EXPECT_EQ(dc.lutWritesScanned.load(), 0u);
    EXPECT_EQ(dc.differs(unit.newest().tables), "");
    EXPECT_EQ(gHolds.load(), 0);
}

// The filter writes whole tables with the CMU off; the action, meanwhile, does not enable it.
TEST_F(ColourPublish, TheFiltersWholeWriteKeepsTheActionFromEnabling)
{
    setters.set(warmer);
    std::mutex m;
    std::condition_variable cv;
    bool writing = false, resume = false;
    const auto pauseAtLut = [&](uint32_t word) {
        if (word != kDcCmuLut1 || writing)
            return;
        std::unique_lock<std::mutex> hold(m);
        writing = true;
        cv.notify_all();
        cv.wait(hold, [&] {
            return resume;
        });
    };
    std::thread filter([&] {
        tBeforeWrite = pauseAtLut;
        unit.frameEnd(dc);
    });
    {
        std::unique_lock<std::mutex> hold(m);
        cv.wait(hold, [&] {
            return writing;
        });
    }
    unit.frameEndAction(dc, true);
    EXPECT_EQ(dc.enableWrites.load(), 0u) << "enabled under the filter's whole write";
    {
        const std::lock_guard<std::mutex> hold(m);
        resume = true;
    }
    cv.notify_all();
    filter.join();
    settle();
    EXPECT_EQ(dc.enableWrites.load(), 1u);
    EXPECT_EQ(dc.differs(unit.newest().tables), "");
}

// The bypass's GENERAL_ACT_REQ is in DC A before the filter can read the bypass as latched: a LUT change with the CMU on
// writes no word until the frame start after the action's bypass.
TEST_F(ColourPublish, ALutChangeWaitsForTheBypassToLatch)
{
    setters.set(warmer);
    settle();
    ASSERT_TRUE(dc.latchedEnable);
    setters.set(inverted);
    for (int i = 0; i < 12; ++i) {
        frame();
        EXPECT_EQ(dc.lutWritesScanned.load(), 0u) << "frame " << i;
    }
    EXPECT_EQ(dc.differs(unit.newest().tables), "");
    EXPECT_EQ(gHolds.load(), 0);
}

// Setters on two threads, the filter and the action on theirs, frame starts landing anywhere in the filter's writes:
// no LUT word written while the latched CMU scans, every hold released, DC A on the newest tables once the setters stop.
TEST_F(ColourPublish, WritersOnTheirOwnThreads)
{
    std::atomic<bool> stop{false};
    std::atomic<unsigned> frames{0};
    std::thread filter([&] {
        std::mt19937 random(7);
        tBeforeWrite = [&](uint32_t) {
            if (random() % 512 == 0)
                dc.boundary();
        };
        while (!stop) {
            unit.frameEnd(dc);
            dc.boundary();
            ++frames;
            std::this_thread::yield();
        }
    });
    std::thread action([&] {
        std::mt19937 random(11);
        while (!stop) {
            unit.frameEndAction(dc, random() % 4 != 0);
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> setterThreads;
    for (int t = 0; t < 2; ++t) {
        setterThreads.emplace_back([&, t] {
            std::mt19937 random(static_cast<uint32_t>(100 + t));
            for (int i = 0; i < 300; ++i) {
                switch (random() % 4) {
                case 0:
                    setters.set(inverted);
                    break;
                case 1:
                    setters.set(warmer);
                    break;
                case 2:
                    setters.set([&](Inputs &in) {
                        in.contrast = random() % 2 ? kUnity16 : kUnity16 + kUnity16 / 4;
                    });
                    break;
                default:
                    setters.set([](Inputs &in) {
                        in = defaults();
                    });
                    break;
                }
                std::this_thread::yield();
            }
        });
    }
    for (auto &t : setterThreads)
        t.join();
    const unsigned at = frames;
    while (frames < at + 50)
        std::this_thread::yield();
    stop = true;
    filter.join();
    action.join();
    settle();
    EXPECT_EQ(dc.lutWritesScanned.load(), 0u);
    EXPECT_EQ(dc.differs(unit.newest().tables), "");
    EXPECT_EQ(unit.holdsOut(), 0u);
    EXPECT_EQ(gHolds.load(), 0);
}

// Detached (no frame end will come), a setter takes no hold; the stop releases every hold out.
TEST_F(ColourPublish, NoHoldOutlivesTheDisplay)
{
    setters.set(warmer, false);
    EXPECT_EQ(gHolds.load(), 0);
    setters.set(inverted);
    setters.set(warmer);
    EXPECT_EQ(gHolds.load(), 2);
    unit.stop(dc);
    EXPECT_EQ(gHolds.load(), 0);
}

}  // namespace
