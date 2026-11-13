// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "../raw/runtime/raw_fresh_queue.h"

namespace {

struct Texture {
    std::string name;
};

struct Entry {
    std::weak_ptr<Texture> texture;
};

// Two channels of one GPU as the raw path orders them (RawMetalSubmit.mm): each runs its submissions in order; an m2v
// submission made after raw work first acquires the raw channel's last seqno (the kernel's Order), and nothing else orders the
// two. A command is "fill:<texture>" or "write:<texture>".
struct Gpu {
    struct Submission {
        bool raw;
        uint32_t acquires;  // raw seqno an m2v submission waits for (0: none)
        std::vector<std::string> commands;
    };

    std::vector<Submission> raw, m2v;
    bool rawSinceM2v = false;

    uint32_t submitRaw(std::vector<std::string> commands)
    {
        raw.push_back({true, 0, std::move(commands)});
        rawSinceM2v = true;
        return uint32_t(raw.size());
    }

    void submitM2v(std::vector<std::string> commands)
    {
        m2v.push_back({false, rawSinceM2v ? uint32_t(raw.size()) : 0u, std::move(commands)});
        rawSinceM2v = false;
    }

    // Whether some order the GPU may run both channels in fills `texture` after a write of it: the m2v channel run as early
    // as its acquires allow, before the raw channel's later submissions
    bool fillMayFollowWrite(const std::string &texture) const
    {
        std::vector<std::string> order;
        size_t r = 0;
        for (const Submission &s : m2v) {
            while (r < s.acquires)
                order.insert(order.end(), raw[r].commands.begin(), raw[r].commands.end()), r++;
            order.insert(order.end(), s.commands.begin(), s.commands.end());
        }
        for (; r < raw.size(); r++)
            order.insert(order.end(), raw[r].commands.begin(), raw[r].commands.end());
        const auto fill = std::find(order.begin(), order.end(), "fill:" + texture);
        const auto write = std::find(order.begin(), order.end(), "write:" + texture);
        return write != order.end() && fill != order.end() && write < fill;
    }
};

// The runtime's use of the queue: a raw submission takes every pending texture and writes it ahead of its own commands
// (FreshWrites); an m2v submission is preceded by FreshQueue::writeAhead (writeFreshAhead), whose raw submission goes
// through where `rawSubmits`, under GPU order where `gpuOrder`; `ahead` false is the runtime before it
struct Runtime {
    raw::FreshQueue<Entry> fresh;
    Gpu gpu;
    bool ahead = true, gpuOrder = true, rawSubmits = true;

    std::shared_ptr<Texture> make(const std::string &name)
    {
        auto t = std::make_shared<Texture>(Texture{name});
        fresh.add({t});
        return t;
    }

    static std::vector<std::string> fills(std::vector<Entry> &entries)
    {
        std::vector<std::string> out;
        for (Entry &e : entries)
            if (auto t = e.texture.lock())
                out.push_back("fill:" + t->name);
        return out;
    }

    std::vector<std::string> takeFills()
    {
        std::vector<Entry> taken = fresh.take();
        return fills(taken);
    }

    void raw(std::vector<std::string> commands)
    {
        std::vector<std::string> all = takeFills();
        all.insert(all.end(), commands.begin(), commands.end());
        gpu.submitRaw(std::move(all));
    }

    void m2v(std::vector<std::string> commands)
    {
        if (ahead)
            fresh.writeAhead(gpuOrder, [&](std::vector<Entry> &taken) {
                if (rawSubmits)
                    gpu.submitRaw(fills(taken));
                return rawSubmits;
            });
        gpu.submitM2v(std::move(commands));
    }
};

}  // namespace

TEST(RawFreshQueue, TheFirstRawSubmissionWritesATextureAheadOfItsCommands)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.raw({"write:a"});
    ASSERT_EQ(rt.gpu.raw.size(), 1u);
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"fill:a", "write:a"}));
    EXPECT_FALSE(rt.fresh.any());
    rt.raw({"write:a"});
    EXPECT_EQ(rt.gpu.raw[1].commands, (std::vector<std::string>{"write:a"}));
}

TEST(RawFreshQueue, AnM2vSubmissionGoesAfterTheRawFillOfItsTextures)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.m2v({"write:a"});
    ASSERT_EQ(rt.gpu.raw.size(), 1u);
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"fill:a"}));
    EXPECT_EQ(rt.gpu.m2v[0].acquires, 1u);
    EXPECT_FALSE(rt.gpu.fillMayFollowWrite("a"));
    rt.raw({"write:a"});
    EXPECT_FALSE(rt.gpu.fillMayFollowWrite("a"));
}

// Without the raw submission ahead, the next raw submission fills the texture after m2v wrote it, on another channel
TEST(RawFreshQueue, WithoutTheFillAheadAnM2vWriteIsOverwritten)
{
    Runtime rt;
    rt.ahead = false;
    auto a = rt.make("a");
    rt.m2v({"write:a"});
    rt.raw({"draw"});
    EXPECT_TRUE(rt.gpu.fillMayFollowWrite("a"));
}

// Where the raw submission ahead cannot be ordered before m2v's, or fails, the texture keeps what its memory held: never a
// fill after m2v wrote it
TEST(RawFreshQueue, AFillThatCannotGoAheadIsDropped)
{
    for (const bool gpuOrder : {false, true}) {
        Runtime rt;
        rt.gpuOrder = gpuOrder;
        rt.rawSubmits = !gpuOrder;
        auto a = rt.make("a");
        rt.m2v({"write:a"});
        rt.raw({"draw"});
        EXPECT_FALSE(rt.gpu.fillMayFollowWrite("a"));
        for (const auto &s : rt.gpu.raw)
            EXPECT_EQ(std::count(s.commands.begin(), s.commands.end(), "fill:a"), 0);
    }
}

TEST(RawFreshQueue, AnM2vSubmissionAfterRawWorkWaitsForIt)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.raw({"draw"});
    auto b = rt.make("b");
    rt.m2v({"write:b", "write:a"});
    EXPECT_FALSE(rt.gpu.fillMayFollowWrite("a"));
    EXPECT_FALSE(rt.gpu.fillMayFollowWrite("b"));
    EXPECT_EQ(rt.gpu.raw.size(), 2u);
}

TEST(RawFreshQueue, AFailedSubmissionGivesItsTexturesBack)
{
    raw::FreshQueue<Entry> q;
    auto a = std::make_shared<Texture>(Texture{"a"});
    auto b = std::make_shared<Texture>(Texture{"b"});
    q.add({a});
    auto taken = q.take();
    q.add({b});
    q.settle(std::move(taken), false);
    auto again = q.take();
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].texture.lock(), a);
    EXPECT_EQ(again[1].texture.lock(), b);
    q.settle(std::move(again), true);
    EXPECT_FALSE(q.any());
    EXPECT_TRUE(q.take().empty());
}

TEST(RawFreshQueue, ATextureReleasedBeforeAnySubmissionIsNeverFilled)
{
    Runtime rt;
    rt.make("gone");
    rt.raw({"draw"});
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"draw"}));
}
