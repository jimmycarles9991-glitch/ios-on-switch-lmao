// Host test of drivers/NativeMobileDisplay/SwitchFrameInfo.h: the keys selector 74 answers, the entries a present
// queues, decoded the way 21H461's IOMobileFramebuffer.framework decodes them, and the client and pending-swap tables.
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../SwitchFrameInfo.h"

namespace {

using namespace switch_frame_info;

std::vector<std::string> keyList()
{
    std::vector<std::string> keys;
    uint32_t count = 0, flags = 0;
    char key[kKeyBytes];
    for (uint32_t i = 0; supported(i, &count, key, &flags) == kOk; ++i)
        keys.emplace_back(key);
    return keys;
}

// PackAndDispatchFrameInfoNotifictionData: entry[0] is the mask, entry[1] the swap id, then one value per set bit
// (two 32-bit halves each if bit 31 is set), named by the key at that bit's index.
std::map<std::string, uint64_t> decode(const uint64_t *entry, unsigned words, uint32_t &swapId)
{
    const std::vector<std::string> keys = keyList();
    std::map<std::string, uint64_t> out;
    const uint64_t mask = entry[0];
    swapId = (uint32_t)entry[1];
    unsigned next = 2;
    for (unsigned bit = 0; bit < 31; ++bit) {
        if (!(mask & (1ull << bit)))
            continue;
        EXPECT_FALSE(mask & (1ull << 31));
        EXPECT_LT(bit, keys.size());
        EXPECT_LT(next, words);
        out[keys[bit]] = entry[next++];
    }
    EXPECT_EQ(next, words);
    return out;
}

uint32_t maskOf(std::initializer_list<const char *> names)
{
    const std::vector<std::string> keys = keyList();
    uint32_t mask = 0;
    for (const char *name : names)
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == name)
                mask |= 1u << i;
    return mask;
}

}  // namespace

TEST(SwitchFrameInfo, SupportedAnswersEveryKeyAndTheSharedQueue)
{
    uint32_t count = 0, flags = 0;
    char key[kKeyBytes];
    memset(key, 0xaa, sizeof(key));
    ASSERT_EQ(supported(0, &count, key, &flags), kOk);
    EXPECT_EQ(count, (uint32_t)kKeys);
    EXPECT_EQ(flags, 1u);
    EXPECT_STREQ(key, "Presentation_time");
    for (size_t i = strlen(key); i < kKeyBytes; ++i)
        EXPECT_EQ(key[i], 0);
    EXPECT_EQ(keyList(), (std::vector<std::string>{"Presentation_time", "Presentation_delta", "Requested_presentation",
                                                   "Thermal_throttle"}));
    count = 7;
    EXPECT_EQ(supported(kKeys, &count, key, &flags), kBadArgument);
    EXPECT_EQ(count, 7u);
}

// Core Animation's AccelServer enables its eight keys one at a time; the ones J120 has that DC A can answer come back.
TEST(SwitchFrameInfo, CoreAnimationsKeysThatExist)
{
    const std::vector<std::string> keys = keyList();
    const char *ca[] = {"Presentation_time", "Presentation_delta", "Requested_presentation", "Performance_headroom",
                        "Thermal_throttle",  "Min_FrameTime",      "Max_FrameTime",          "Vbl_FrameTime"};
    unsigned found = 0;
    for (const char *name : ca)
        for (const std::string &k : keys)
            found += k == name;
    EXPECT_EQ(found, 4u);
}

TEST(SwitchFrameInfo, EntryDecodesToTheFrame)
{
    const uint32_t mask =
        maskOf({"Presentation_time", "Presentation_delta", "Requested_presentation", "Thermal_throttle"});
    uint64_t last = 0, words[kEntryWords];
    const Frame first = {1000000, 999500, 41};
    unsigned n = entry(mask, first, last, words);
    uint32_t id = 0;
    auto d = decode(words, n, id);
    EXPECT_EQ(id, 41u);
    EXPECT_EQ(d["Presentation_time"], 1000000u);
    EXPECT_EQ(d["Presentation_delta"], 1000000u);  // from 0, as J120's pipe keeps it
    EXPECT_EQ(d["Requested_presentation"], 999500u);
    EXPECT_EQ(d["Thermal_throttle"], 0u);
    EXPECT_EQ(last, 1000000u);

    n = entry(mask, {1400000, 1400000, 42}, last, words);
    d = decode(words, n, id);
    EXPECT_EQ(id, 42u);
    EXPECT_EQ(d["Presentation_delta"], 400000u);
    EXPECT_EQ(last, 1400000u);
}

TEST(SwitchFrameInfo, EntryCarriesOnlyTheClientsKeysInBitOrder)
{
    uint64_t last = 500, words[kEntryWords];
    const unsigned n =
        entry(maskOf({"Requested_presentation", "Presentation_time"}) | 0xff00u, {900, 800, 7}, last, words);
    ASSERT_EQ(n, 4u);
    EXPECT_EQ(words[0], (uint64_t)maskOf({"Presentation_time", "Requested_presentation"}));
    EXPECT_EQ(words[2], 900u);
    EXPECT_EQ(words[3], 800u);
    EXPECT_EQ(last, 900u);  // the delta's base moves with every entry, as J120's
    EXPECT_EQ(entry(0, {1000, 0, 8}, last, words), 2u);
    EXPECT_EQ(last, 1000u);
}

TEST(SwitchFrameInfo, ClientsByRecord)
{
    Clients c;
    int records[Clients::kMax + 1];
    EXPECT_FALSE(c.any());
    EXPECT_EQ(c.change(&records[0], 0), kOk);
    EXPECT_FALSE(c.any());
    for (unsigned i = 0; i < Clients::kMax; ++i)
        EXPECT_EQ(c.change(&records[i], 1u << (i % kKeys)), kOk);
    EXPECT_EQ(c.change(&records[Clients::kMax], 1), kNoResources);
    EXPECT_EQ(c.change(&records[3], 0xffffffffu), kOk);
    ASSERT_NE(c.find(&records[3]), nullptr);
    EXPECT_EQ(c.find(&records[3])->mask, kAllKeys);
    c.find(&records[3])->lastPresented = 77;
    EXPECT_EQ(c.change(&records[3], 1), kOk);
    EXPECT_EQ(c.find(&records[3])->lastPresented, 77u);
    EXPECT_EQ(c.change(&records[3], 0), kOk);
    EXPECT_EQ(c.find(&records[3]), nullptr);
    EXPECT_EQ(c.change(&records[Clients::kMax], 1), kOk);
    EXPECT_EQ(c.find(&records[Clients::kMax])->lastPresented, 0u);
    EXPECT_EQ(c.find(nullptr), nullptr);
}

TEST(SwitchFrameInfo, PendingSendsTheShownSwapAndDropsReplacedOnes)
{
    Pending p;
    Frame f = {};
    p.take(1, 101, 1000);
    p.take(2, 102, 2000);
    p.take(3, 103, 3000);
    ASSERT_TRUE(p.shown(2, 2100, f));
    EXPECT_EQ(f.swapId, 102u);
    EXPECT_EQ(f.requested, 2000u);
    EXPECT_EQ(f.presented, 2100u);
    EXPECT_FALSE(p.shown(1, 2200, f));  // replaced by 2 before any frame showed it
    ASSERT_TRUE(p.shown(3, 3100, f));
    EXPECT_EQ(f.swapId, 103u);
    EXPECT_FALSE(p.shown(3, 3200, f));
    p.take(0, 1, 1);
    EXPECT_FALSE(p.shown(0, 1, f));
}

TEST(SwitchFrameInfo, PendingEvictsTheOldestWhenFull)
{
    Pending p;
    Frame f = {};
    for (uint64_t seq = 1; seq <= Pending::kMax + 1; ++seq)
        p.take(seq, (uint32_t)(100 + seq), seq * 10);
    ASSERT_TRUE(p.shown(2, 1, f));
    EXPECT_EQ(f.swapId, 102u);
    EXPECT_FALSE(p.shown(1, 1, f));
    ASSERT_TRUE(p.shown(Pending::kMax + 1, 1, f));
    EXPECT_EQ(f.swapId, 100u + Pending::kMax + 1);
}

// A present stamped in mach_absolute_time reaches the framework in continuous time, which it turns back with the same
// offset (kern_egest_timestamp): after a sleep of 5 s worth of ticks the two clocks differ by that much.
TEST(SwitchFrameInfo, PresentsInContinuousTime)
{
    const uint64_t sleptTicks = 5 * 24000000ull, nowAbsolute = 900000000, nowContinuous = nowAbsolute + sleptTicks;
    const uint64_t present = nowAbsolute - 16000;
    const uint64_t sent = continuousTime(present, nowAbsolute, nowContinuous);
    EXPECT_EQ(sent, present + sleptTicks);
    EXPECT_EQ(sent - nowContinuous + nowAbsolute, present);  // egest: value - continuous + absolute
    EXPECT_EQ(continuousTime(present, nowAbsolute, nowAbsolute), present);
}
