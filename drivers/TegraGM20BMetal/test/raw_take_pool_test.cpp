// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_take_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

struct Item {
    std::atomic<int> holders{0};
    uint64_t value = 0;
};

TEST(TakePool, AKeptItemIsTakenOnceAndItsNodeComesBack)
{
    raw::TakePool<Item, 4> pool;
    uint32_t node;
    EXPECT_EQ(pool.take(node), nullptr);
    Item *a = pool.room(node);
    ASSERT_NE(a, nullptr);
    a->value = 7;
    pool.keep(node);
    EXPECT_EQ(pool.kept(), 1u);
    uint32_t taken;
    Item *b = pool.take(taken);
    ASSERT_EQ(b, a);
    EXPECT_EQ(b->value, 7u);
    EXPECT_EQ(pool.take(node), nullptr);
    pool.giveBack(taken);
    EXPECT_EQ(pool.room(node), a) << "the node given back holds the next item";
}

TEST(TakePool, AFullPoolHasNoRoom)
{
    raw::TakePool<Item, 2> pool;
    uint32_t a, b, c;
    ASSERT_NE(pool.room(a), nullptr);
    ASSERT_NE(pool.room(b), nullptr);
    EXPECT_EQ(pool.room(c), nullptr);
    pool.keep(a);
    pool.keep(b);
    EXPECT_EQ(pool.room(c), nullptr);
}

// As command buffers give their recordings back on completion threads while others take them on encoding threads: no
// item is held by two threads at once, and none is lost
TEST(TakePool, ThreadsTakingAndKeepingNeverShareAnItem)
{
    raw::TakePool<Item, 16> pool;
    std::atomic<int> shared{0};
    std::atomic<uint64_t> moved{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&] {
            for (int i = 0; i < 100000; i++) {
                uint32_t node;
                Item *item = pool.take(node);
                bool taken = item != nullptr;
                if (!item)
                    item = pool.room(node);
                if (!item)
                    continue;
                if (item->holders.fetch_add(1) != 0)
                    shared++;
                item->value++;
                moved++;
                item->holders.fetch_sub(1);
                if (taken && i % 3 == 0)
                    pool.giveBack(node);
                else
                    pool.keep(node);
            }
        });
    for (std::thread &t : threads)
        t.join();
    EXPECT_EQ(shared.load(), 0);
    uint64_t values = 0, items = 0;
    uint32_t node;
    while (Item *item = pool.take(node)) {
        values += item->value;
        items++;
    }
    while (Item *item = pool.room(node)) {
        values += item->value;
        items++;
    }
    EXPECT_EQ(items, 16u) << "every node is kept or spare";
    EXPECT_EQ(values, moved.load()) << "each item kept its count across threads";
}

}  // namespace
