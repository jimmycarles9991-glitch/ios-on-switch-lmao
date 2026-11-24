// drivers/NativeSwitchBlock/SwitchSdIo.h: latencies land in half-octave buckets whose bounds the percentiles report, a
// line shows only what happened since the one before, the queue's depth is its length integrated over time, and the
// line and the totals fit what the driver gives them.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../SwitchSdIo.h"

namespace {
constexpr uint64_t kUs = 1000, kMs = 1000000;

TEST(SwitchSdIo, BucketsAreHalfOctavesFrom64Us)
{
    EXPECT_EQ(switchSdBucket(0), 0u);
    EXPECT_EQ(switchSdBucket(64 * kUs - 1), 0u);
    EXPECT_EQ(switchSdBucket(64 * kUs), 1u);
    EXPECT_EQ(switchSdBucket(90 * kUs), 1u);
    EXPECT_EQ(switchSdBucket(91 * kUs), 2u);
    EXPECT_EQ(switchSdBucket(128 * kUs), 3u);
    EXPECT_EQ(switchSdBucket(1 * kMs), 8u);  // 724 us to 1.024 ms
    EXPECT_EQ(switchSdBucket(10 * kMs), 15u);
    EXPECT_EQ(switchSdBucket(3000 * kMs), kSwitchSdBuckets - 1);
    // Every value lies inside its bucket, and buckets only grow.
    for (uint64_t ns = 1; ns < 5000 * kMs; ns = ns * 9 / 8 + 1) {
        const unsigned b = switchSdBucket(ns);
        ASSERT_LT(b, kSwitchSdBuckets);
        if (b)
            EXPECT_GE(ns, switchSdBucketLow(b)) << ns;
        if (b + 1 < kSwitchSdBuckets)
            EXPECT_LT(ns, switchSdBucketTop(b)) << ns;
        EXPECT_GE(b, switchSdBucket(ns * 8 / 9)) << ns;
    }
}

TEST(SwitchSdIo, PercentilesReportTheBucketTop)
{
    SwitchSdHistogram h = {};
    EXPECT_EQ(h.percentileNs(500), 0u);
    for (int i = 0; i < 90; ++i)
        h.add(500 * kUs);
    for (int i = 0; i < 9; ++i)
        h.add(5 * kMs);
    h.add(80 * kMs);
    EXPECT_EQ(h.count, 100u);
    EXPECT_EQ(h.sumNs, 90 * 500 * kUs + 9 * 5 * kMs + 80 * kMs);
    const uint64_t p50 = h.percentileNs(500), p99 = h.percentileNs(990), p100 = h.percentileNs(1000);
    EXPECT_GT(p50, 500 * kUs);
    EXPECT_LE(p50, 500 * kUs * 1415 / 1000);
    EXPECT_EQ(h.percentileNs(900), p50);  // the 90th value is still 500 us
    EXPECT_GT(p99, 5 * kMs);
    EXPECT_LE(p99, 5 * kMs * 1415 / 1000);
    EXPECT_GT(p100, 80 * kMs);
}

TEST(SwitchSdIo, LineShowsOnlyWhatHappenedSinceTheLastOne)
{
    auto io = std::make_unique<SwitchSdIo>();
    for (int i = 0; i < 50; ++i)
        io->wait[0].add(30 * kMs);
    auto before = std::make_unique<SwitchSdIo>();
    io->copyTo(*before);
    for (int i = 0; i < 7; ++i) {
        io->wait[0].add(2 * kMs);
        io->total[0].add(3 * kMs);
    }
    io->command[0][0].add(400 * kUs);
    io->command[1][3].add(9 * kMs);
    char out[768];
    const size_t n = switchSdFormatIo(*io, *before, 10000 * kMs, out, sizeof(out));
    const std::string line(out, n);
    EXPECT_EQ(line.rfind("SwitchSD io:", 0), 0u) << line;
    // Seven waits of 2 ms (the 50 of 30 ms are the last line's): every percentile is 2 ms's bucket top (1.45-2.05 ms);
    // 3 ms lies in 2.9-4.1 ms.
    EXPECT_NE(line.find("hi wait 7 2.0/2.0/2.0 all 7 4.0/4.0/4.0"), std::string::npos) << line;
    EXPECT_EQ(line.find("def"), std::string::npos) << line;  // a class with nothing is left out
    EXPECT_NE(line.find("cmd rd 1 0.5/0.5/0.5 - - -"), std::string::npos) << line;
    EXPECT_NE(line.find("cmd wr - - - 1 "), std::string::npos) << line;
    EXPECT_EQ(line.find("cq task"), std::string::npos) << line;
}

TEST(SwitchSdIo, DepthIsTheQueueLengthOverTime)
{
    auto io = std::make_unique<SwitchSdIo>();
    // 0 for 1 s, 4 for 2 s, 2 for 2 s: 12 request-seconds over 5 s.
    io->depthChanging(1000 * kMs, 0);
    io->depthChanging(2000 * kMs, 0);
    io->depthChanging(4000 * kMs, 4);
    io->depthChanging(6000 * kMs, 2);
    io->depthChanging(5000 * kMs, 1);  // a submitter's older clock read adds nothing
    EXPECT_EQ(io->depthNs, 4 * 2000 * kMs + 2 * 2000 * kMs);
    EXPECT_EQ(io->depthMax, 4u);
    const SwitchSdIo none = {};
    char out[768];
    const std::string line(out, switchSdFormatIo(*io, none, 5000 * kMs, out, sizeof(out)));
    EXPECT_NE(line.find("queue 2.4 max 4"), std::string::npos) << line;
}

// kern.switch_sd: every token "key=value", a histogram "count,sum_us,b0..b31"; two readings subtract.
TEST(SwitchSdIo, TotalsParse)
{
    auto io = std::make_unique<SwitchSdIo>();
    io->wait[0].add(1 * kMs);
    io->wait[0].add(100 * kMs);
    io->total[3].add(7 * kMs);
    io->command[1][2].add(3 * kMs);
    io->asked[1] = 5;
    io->chosen = 2;
    io->bounceBytes = 4096;
    std::string text(16384, '\0');
    text.resize(switchSdFormatTotals(*io, text.data(), text.size()));
    std::map<std::string, std::vector<uint64_t>> tokens;
    std::istringstream in(text);
    for (std::string token; in >> token;) {
        const size_t eq = token.find('=');
        ASSERT_NE(eq, std::string::npos) << token;
        std::vector<uint64_t> values;
        std::istringstream list(token.substr(eq + 1));
        for (std::string v; std::getline(list, v, ',');)
            values.push_back(std::strtoull(v.c_str(), nullptr, 10));
        tokens[token.substr(0, eq)] = values;
    }
    ASSERT_EQ(tokens["wait0"].size(), 2 + kSwitchSdBuckets);
    EXPECT_EQ(tokens["wait0"][0], 2u);
    EXPECT_EQ(tokens["wait0"][1], 101000u);
    EXPECT_EQ(tokens["wait0"][2 + switchSdBucket(1 * kMs)], 1u);
    EXPECT_EQ(tokens["wait0"][2 + switchSdBucket(100 * kMs)], 1u);
    EXPECT_EQ(tokens["all3"][0], 1u);
    EXPECT_EQ(tokens["wr2"][0], 1u);
    EXPECT_EQ(tokens["rd2"][0], 0u);
    EXPECT_EQ(tokens["asked1"], std::vector<uint64_t>{5});
    EXPECT_EQ(tokens["chosen"], std::vector<uint64_t>{2});
    EXPECT_EQ(tokens["bounce"], std::vector<uint64_t>{4096});
    EXPECT_EQ(tokens.find("cq0"), tokens.end());
    ASSERT_EQ(tokens["rt0"].size(), 2 + kSwitchSdBuckets);
    ASSERT_EQ(tokens["rt4"].size(), 2 + kSwitchSdBuckets);
    EXPECT_EQ(tokens.size(), 2 * kSwitchSdClasses + 2 * kSwitchSdSizes + kSwitchSdRtPhases + kSwitchSdClasses + 3);
}

TEST(SwitchSdIo, FineBucketsAreHalfOctavesFrom1Us)
{
    EXPECT_EQ(switchSdBucket(0, kSwitchSdFineFirstNs), 0u);
    EXPECT_EQ(switchSdBucket(999, kSwitchSdFineFirstNs), 0u);
    EXPECT_EQ(switchSdBucket(1 * kUs, kSwitchSdFineFirstNs), 1u);
    EXPECT_EQ(switchSdBucket(14 * kUs, kSwitchSdFineFirstNs), 8u);
    EXPECT_EQ(switchSdBucket(28 * kUs, kSwitchSdFineFirstNs), 10u);
    EXPECT_EQ(switchSdBucket(55 * kUs, kSwitchSdFineFirstNs), 12u);
    EXPECT_EQ(switchSdBucket(64 * kMs, kSwitchSdFineFirstNs), kSwitchSdBuckets - 1);
    for (uint64_t ns = 1; ns < 100 * kMs; ns = ns * 9 / 8 + 1) {
        const unsigned b = switchSdBucket(ns, kSwitchSdFineFirstNs);
        ASSERT_LT(b, kSwitchSdBuckets);
        if (b)
            EXPECT_GE(ns, switchSdBucketLow(b, kSwitchSdFineFirstNs)) << ns;
        if (b + 1 < kSwitchSdBuckets)
            EXPECT_LT(ns, switchSdBucketTop(b, kSwitchSdFineFirstNs)) << ns;
        EXPECT_GE(b, switchSdBucket(ns * 8 / 9, kSwitchSdFineFirstNs)) << ns;
    }
}

TEST(SwitchSdIo, RoundTripSplitInLineAndTotals)
{
    auto io = std::make_unique<SwitchSdIo>();
    io->roundTrip[kSwitchSdRtGap].add(500 * kUs);
    auto before = std::make_unique<SwitchSdIo>();
    io->copyTo(*before);

    io->roundTrip[kSwitchSdRtSubmit].add(25 * kUs);
    io->roundTrip[kSwitchSdRtPrep].add(12 * kUs);
    io->roundTrip[kSwitchSdRtWire].add(420 * kUs);
    io->roundTrip[kSwitchSdRtWake].add(18 * kUs);
    io->roundTrip[kSwitchSdRtGap].add(52 * kUs);

    char out[896];
    const size_t n = switchSdFormatIo(*io, *before, 10000 * kMs, out, sizeof(out));
    const std::string line(out, n);
    EXPECT_NE(line.find("| rt us sub 32/32/32 prep 16/16/16 wire 512/512/512 wake 22/22/22 gap 64/64/64"),
              std::string::npos)
        << line;

    std::string text(16384, '\0');
    text.resize(switchSdFormatTotals(*io, text.data(), text.size()));
    std::map<std::string, std::vector<uint64_t>> tokens;
    std::istringstream in(text);
    for (std::string token; in >> token;) {
        const size_t eq = token.find('=');
        ASSERT_NE(eq, std::string::npos) << token;
        std::vector<uint64_t> values;
        std::istringstream list(token.substr(eq + 1));
        for (std::string v; std::getline(list, v, ',');)
            values.push_back(std::strtoull(v.c_str(), nullptr, 10));
        tokens[token.substr(0, eq)] = values;
    }
    EXPECT_EQ(tokens["rt0"][0], 1u);
    EXPECT_EQ(tokens["rt0"][1], 25u);
    EXPECT_EQ(tokens["rt0"][2 + switchSdBucket(25 * kUs, kSwitchSdFineFirstNs)], 1u);
    EXPECT_EQ(tokens["rt1"][1], 12u);
    EXPECT_EQ(tokens["rt2"][1], 420u);
    EXPECT_EQ(tokens["rt3"][1], 18u);
    EXPECT_EQ(tokens["rt4"][0], 2u);
    EXPECT_EQ(tokens["rt4"][1], 552u);
}

// The driver's buffers: a 10 s line with more of everything than a card can do (320,000 commands of each kind) fits its
// 896 bytes, and totals after years of that fit the sysctl's 16 KiB.
TEST(SwitchSdIo, FitsTheDriversBuffers)
{
    const auto fill = [](SwitchSdIo &io, uint64_t perBucket) {
        const auto each = [&](auto &h) {
            for (unsigned b = 0; b < kSwitchSdBuckets; ++b)
                h.n[b] = perBucket;
            h.count = perBucket * kSwitchSdBuckets;
            h.sumNs = h.count * kMs;
        };
        for (unsigned c = 0; c < kSwitchSdClasses; ++c) {
            each(io.wait[c]);
            each(io.total[c]);
            io.asked[c] = perBucket * kSwitchSdBuckets;
        }
        for (auto &dir : io.command) {
            for (auto &h : dir)
                each(h);
        }
        for (auto &h : io.roundTrip)
            each(h);
        io.chosen = io.depthMax = perBucket * kSwitchSdBuckets;
        io.depthNs = perBucket * kMs;
        io.bounceBytes = perBucket << 20;
    };
    auto busy = std::make_unique<SwitchSdIo>();
    fill(*busy, 10000);
    const SwitchSdIo none = {};
    char line[896];
    const size_t n = switchSdFormatIo(*busy, none, 10000 * kMs, line, sizeof(line));
    EXPECT_LT(n, sizeof(line) - 1);
    EXPECT_EQ(std::strlen(line), n);
    EXPECT_NE(std::string(line).find("bounce"), std::string::npos) << line;
    auto years = std::make_unique<SwitchSdIo>();
    fill(*years, 100000000000ULL);
    std::vector<char> totals(16384);
    const size_t t = switchSdFormatTotals(*years, totals.data(), totals.size());
    EXPECT_LT(t, totals.size() - 256);  // room for the setup and cryptex tokens around it
}
}  // namespace
