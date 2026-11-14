// SPDX-License-Identifier: MIT
// raw/runtime/raw_grow_list.h: a list that grows while another thread reads what it was handed. Built twice: under
// AddressSanitizer (a read of storage a growth freed faults) and ThreadSanitizer (raw_grow_list_tsan_test: a read not
// ordered after the write it reads is a report).
#include "../raw/runtime/raw_grow_list.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Wide {
    uint64_t index, check[5];
};

Wide wide(uint64_t i)
{
    return {i, {i * 3, i * 5, i * 7, i * 11, i ^ 0x5a5a5a5a}};
}

TEST(RawGrowList, AppendsIndexesAndKeepsItsCapacityAcrossAClear)
{
    raw::GrowList<uint32_t> list;
    for (uint32_t i = 0; i < 1000; i++)
        list.push_back(i * 3);
    ASSERT_EQ(list.size(), 1000u);
    for (uint32_t i = 0; i < 1000; i++)
        EXPECT_EQ(list[i], i * 3);
    const uint32_t more[3] = {7, 8, 9};
    list.append(more, more + 3);
    EXPECT_EQ(list.back(), 9u);
    list.resize(1001);
    EXPECT_EQ(list.size(), 1001u);
    const size_t capacity = list.capacity();
    list.clear();
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.capacity(), capacity);
    list.emplace_back(5u);
    EXPECT_EQ(list.own(0), 5u);
}

TEST(RawGrowList, MovesAndSwapsCarryTheirStorage)
{
    raw::GrowList<uint64_t> a, b;
    for (uint64_t i = 0; i < 100; i++)
        a.push_back(i);
    b.push_back(42);
    a.swap(b);
    EXPECT_EQ(a.size(), 1u);
    EXPECT_EQ(b.size(), 100u);
    EXPECT_EQ(b[99], 99u);
    raw::GrowList<uint64_t> c(std::move(b));
    EXPECT_EQ(c[50], 50u);
    EXPECT_TRUE(b.empty());
}

// The writer appends wide elements from an empty list, so it grows many times while the reader reads every element below
// each length the writer published, from whichever storage it finds
TEST(RawGrowList, AReaderOfWhatWasPublishedMeetsEveryGrowth)
{
    for (int round = 0; round < 20; round++) {
        raw::GrowList<Wide> list;
        std::atomic<size_t> published{0};
        std::atomic<bool> done{false};
        std::atomic<uint64_t> checked{0};
        std::thread reader([&] {
            size_t seen = 0;
            while (!done.load(std::memory_order_acquire) || seen < published.load(std::memory_order_acquire)) {
                const size_t upTo = published.load(std::memory_order_acquire);
                for (size_t i = 0; i < upTo; i += 1 + i / 64) {
                    const Wide &w = list[i];
                    if (w.index != i || w.check[4] != (i ^ 0x5a5a5a5a))
                        ADD_FAILURE() << "element " << i << " read as " << w.index;
                }
                checked.fetch_add(upTo, std::memory_order_relaxed);
                seen = upTo;
            }
        });
        for (uint64_t i = 0; i < 20000; i++) {
            list.push_back(wide(i));
            if (i % 37 == 0) {
                published.store(list.size(), std::memory_order_release);
                // a growth after the length the reader holds: it reads through the new storage before the next one
                if (i % 2048 == 0) {
                    list.reserve(list.capacity() * 2);
                    std::this_thread::yield();
                }
            }
        }
        published.store(list.size(), std::memory_order_release);
        done.store(true, std::memory_order_release);
        reader.join();
        EXPECT_GT(checked.load(), 0u);
    }
}

// One growth after the reader took the length, ordered with the reader by nothing but the list itself (the hand-shakes
// are relaxed): the reader's reads through the new storage are ordered after the growth's copy by the storage's load
TEST(RawGrowList, AGrowthAfterTheHandOverIsOrderedByTheList)
{
    raw::GrowList<Wide> list;
    for (uint64_t i = 0; i < 100; i++)
        list.push_back(wide(i));
    std::atomic<size_t> published{0};
    std::atomic<bool> ready{false}, grown{false};
    published.store(list.size(), std::memory_order_release);
    std::thread reader([&] {
        const size_t upTo = published.load(std::memory_order_acquire);
        const Wide *before = std::as_const(list).data();
        ready.store(true, std::memory_order_relaxed);
        while (!grown.load(std::memory_order_relaxed) || std::as_const(list).data() == before)
            std::this_thread::yield();
        for (size_t i = 0; i < upTo; i++)
            EXPECT_EQ(list[i].index, i);
    });
    while (!ready.load(std::memory_order_relaxed))
        std::this_thread::yield();
    list.reserve(list.capacity() * 4);
    grown.store(true, std::memory_order_relaxed);
    reader.join();
}

}  // namespace
