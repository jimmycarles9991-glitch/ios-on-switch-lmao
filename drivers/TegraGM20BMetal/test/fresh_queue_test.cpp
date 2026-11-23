// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "../fresh_queue.h"

namespace {

struct Texture {
    std::string name;
};

struct Entry {
    std::weak_ptr<Texture> texture;
};

// The raw channel, which runs its submissions in order. A command is "fill:<texture>" or "write:<texture>".
struct Gpu {
    struct Submission {
        std::vector<std::string> commands;
    };

    std::vector<Submission> raw;

    void submitRaw(std::vector<std::string> commands)
    {
        raw.push_back({std::move(commands)});
    }

    // Whether the channel fills `texture` after a write of it
    bool fillMayFollowWrite(const std::string &texture) const
    {
        std::vector<std::string> order;
        for (const Submission &s : raw)
            order.insert(order.end(), s.commands.begin(), s.commands.end());
        const auto fill = std::find(order.begin(), order.end(), "fill:" + texture);
        const auto write = std::find(order.begin(), order.end(), "write:" + texture);
        return write != order.end() && fill != order.end() && write < fill;
    }
};

// The runtime's use of the queue: a raw submission takes every pending texture and writes it ahead of its own commands
// (FreshWrites)
struct Runtime {
    gm20b::FreshQueue<Entry> fresh;
    Gpu gpu;

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
};

}  // namespace

TEST(GM20BFreshQueue, TheFirstRawSubmissionWritesATextureAheadOfItsCommands)
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

// replaceRegion into a fresh private texture (gpu.upload.replace_region's private cases: boot 19 and 21 read R8's fill,
// 0xff, over 25492 of its 25641 uploaded texels): the transfer extracts it and writes its fill first, so no later raw
// submission fills it over the CPU's bytes
TEST(GM20BFreshQueue, ACpuTransferWritesItsTexturesFillFirst)
{
    Runtime rt;
    auto a = rt.make("a");
    auto b = rt.make("b");
    ASSERT_TRUE(rt.fresh.extract([&](const Entry &e) {
        return e.texture.lock() == a;
    }));
    rt.gpu.submitRaw({"fill:a"});
    rt.gpu.submitRaw({"write:a"});
    rt.raw({"draw"});
    EXPECT_FALSE(rt.gpu.fillMayFollowWrite("a"));
    EXPECT_EQ(rt.gpu.raw.back().commands, (std::vector<std::string>{"fill:b", "draw"})) << "b stays fresh";
    EXPECT_FALSE(rt.fresh.extract([&](const Entry &e) {
        return e.texture.lock() == a;
    })) << "extracted once";
}

// Without the extract the next raw submission fills the texture over what the transfer wrote
TEST(GM20BFreshQueue, WithoutTheExtractACpuTransferIsOverwritten)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.gpu.submitRaw({"write:a"});
    rt.raw({"draw"});
    EXPECT_TRUE(rt.gpu.fillMayFollowWrite("a"));
}

TEST(GM20BFreshQueue, AFailedSubmissionGivesItsTexturesBack)
{
    gm20b::FreshQueue<Entry> q;
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

TEST(GM20BFreshQueue, ATextureReleasedBeforeAnySubmissionIsNeverFilled)
{
    Runtime rt;
    rt.make("gone");
    rt.raw({"draw"});
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"draw"}));
}
