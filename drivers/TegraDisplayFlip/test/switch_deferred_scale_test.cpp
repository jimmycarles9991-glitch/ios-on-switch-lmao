// Host test of drivers/TegraDisplayFlip/SwitchDeferredScale.h against a model of surfaces, a decoder pool that writes a
// source only while nothing holds it, a scaler that defers or copies, and a display that scans one frame at a time: every
// read of a destination and every scanned frame shows the bytes the copy would have produced, and every hold is dropped
// exactly once.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "../SwitchDeferredScale.h"

using namespace SwitchDeferredScale;

namespace {

constexpr unsigned kSources = 4, kDests = 4;

uint64_t sourcePage(unsigned i)
{
    return 0x100000ull + i * 0x10000ull;
}

uint64_t destPage(unsigned i)
{
    return 0x900000ull + i * 0x10000ull;
}

struct Model {
    Table table;
    uint32_t sourceContent[kSources] = {};
    uint32_t destContent[kDests] = {};
    uint32_t expected[kDests] = {};  // what D logically holds: the scaled source as it was when the job ran
    std::map<uint64_t, int> holds;
    std::map<uint64_t, unsigned> sourceIndex;

    struct Scan {
        uint64_t dest, source;
        unsigned destIndex;
    };

    std::vector<Scan> onScreen;
    unsigned violations = 0;

    Model()
    {
        for (unsigned i = 0; i < kSources; ++i) {
            sourceContent[i] = 1000 * (i + 1);
            sourceIndex[sourcePage(i)] = i;
        }
    }

    static uint32_t scaled(uint32_t source)
    {
        return source * 7 + 3;
    }

    Job job(unsigned s, unsigned d) const
    {
        Job j;
        j.source = sourcePage(s);
        j.dest = destPage(d);
        j.transform = 4;
        return j;
    }

    void release(uint64_t source)
    {
        if (!source)
            return;
        if (--holds[source] < 0)
            ++violations;
    }

    void apply(const Action &a, unsigned d)
    {
        if (a.copy)
            destContent[d] = scaled(sourceContent[sourceIndex[a.job.source]]);
        release(a.releaseSource);
    }

    void scalerJob(unsigned s, unsigned d, bool qualifies)
    {
        uint64_t released = 0;
        if (qualifies && table.record(job(s, d), &released)) {
            ++holds[sourcePage(s)];
            release(released);
        } else {
            apply(table.access(destPage(d), Access::NewDestination), d);
            destContent[d] = scaled(sourceContent[s]);
        }
        expected[d] = scaled(sourceContent[s]);
    }

    void cpuRead(unsigned d, Access kind)
    {
        apply(table.access(destPage(d), kind), d);
        if (destContent[d] != expected[d])
            ++violations;
    }

    void displayFrame(unsigned d)
    {
        Job j;
        const uint64_t source = table.claim(destPage(d), &j);
        const uint32_t shown = source ? scaled(sourceContent[sourceIndex[source]]) : destContent[d];
        if (shown != expected[d])
            ++violations;
        for (const Scan &old : onScreen)
            release(table.retire(old.dest, old.source).releaseSource);
        onScreen.clear();
        if (source)
            onScreen.push_back({destPage(d), source, d});
    }

    void decoderWrite(unsigned s)
    {
        if (holds[sourcePage(s)] > 0)
            return;
        sourceContent[s] += 1;
        for (const Scan &scan : onScreen)
            if (scan.source == sourcePage(s))
                ++violations;
    }

    void freeDest(unsigned d)
    {
        apply(table.access(destPage(d), Access::Freed), d);
        destContent[d] = expected[d] = 0;
    }

    void drain()
    {
        for (const Scan &old : onScreen)
            release(table.retire(old.dest, old.source).releaseSource);
        onScreen.clear();
        for (unsigned d = 0; d < kDests; ++d)
            freeDest(d);
    }
};

}  // namespace

TEST(DeferredScale, ReadsSeeTheCopyAndHoldsBalance)
{
    std::mt19937 random(7);
    for (unsigned round = 0; round < 200; ++round) {
        Model m;
        for (unsigned step = 0; step < 2000; ++step) {
            const unsigned s = random() % kSources, d = random() % kDests;
            switch (random() % 8) {
            case 0:
            case 1:
                m.scalerJob(s, d, random() % 4 != 0);
                break;
            case 2:
                m.cpuRead(d, Access::CpuLock);
                break;
            case 3:
                m.cpuRead(d, random() % 2 ? Access::GpuImport : Access::ScalerSource);
                break;
            case 4:
            case 5:
                m.displayFrame(d);
                break;
            case 6:
                m.decoderWrite(s);
                break;
            case 7:
                if (random() % 8 == 0)
                    m.freeDest(d);
                break;
            }
            ASSERT_EQ(m.violations, 0u) << "round " << round << " step " << step;
        }
        m.drain();
        EXPECT_EQ(m.violations, 0u);
        EXPECT_EQ(m.table.holds(), 0u);
        for (const auto &[source, count] : m.holds)
            EXPECT_EQ(count, 0) << "source " << source;
    }
}

TEST(DeferredScale, ScannedSourceOutlivesItsDestination)
{
    Table t;
    uint64_t released = 1;
    Job j;
    j.source = 0x1000;
    j.dest = 0x9000;
    ASSERT_TRUE(t.record(j, &released));
    EXPECT_EQ(released, 0u);
    Job got;
    ASSERT_EQ(t.claim(0x9000, &got), 0x1000u);
    EXPECT_EQ(t.access(0x9000, Access::Freed).releaseSource, 0u) << "S is still on screen";
    EXPECT_FALSE(t.deferred(0x9000));
    EXPECT_EQ(t.retire(0x9000, 0x1000).releaseSource, 0x1000u);
    EXPECT_EQ(t.holds(), 0u);
}

TEST(DeferredScale, ADeferredDestinationIsNeverAJobsSource)
{
    Table t;
    uint64_t released = 0;
    Job j;
    j.source = 0x1000;
    j.dest = 0x9000;
    ASSERT_TRUE(t.record(j, &released));
    Job chained;
    chained.source = 0x9000;
    chained.dest = 0xa000;
    EXPECT_FALSE(t.record(chained, &released)) << "the caller copies: D's bytes are not real yet";
}
