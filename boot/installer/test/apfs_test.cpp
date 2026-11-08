// The installer's container fit on a small synthetic container: 20 chunks, 4 per chunk-info block, so cutting it
// drops chunk-info blocks, shrinks the internal pool and hands the pool's tail back to chunk 0. Whether macOS accepts
// a fitted container is apfs_fit_test's question (real containers, fsck_apfs and the kernel).
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <vector>

extern "C" {
#include "boot/installer/apfs.h"
}

namespace {

constexpr uint32_t kBlock = SWITCH_APFS_BLOCK;
constexpr uint64_t kChunk = 32768;
constexpr uint64_t kBlocks = 20 * kChunk;
constexpr uint64_t kPool = 100;
constexpr uint64_t kPoolBlocks = 3 * (20 + 5);
constexpr uint64_t kPoolBitmap = 200;
constexpr uint64_t kMinimum = 3 * kChunk + 100;

using Block = std::vector<uint8_t>;

void put(Block &b, size_t at, uint64_t v, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) {
        b[at + i] = uint8_t(v >> (8 * i));
    }
}

uint64_t get(const Block &b, size_t at, size_t bytes)
{
    uint64_t v = 0;
    for (size_t i = bytes; i--;) {
        v = v << 8 | b[at + i];
    }
    return v;
}

void set_bits(Block &b, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; i++) {
        b[i / 8] |= uint8_t(1u << (i % 8));
    }
}

struct Container {
    std::map<uint64_t, Block> blocks;
    std::vector<uint8_t> work = std::vector<uint8_t>(SWITCH_APFS_WORK_BYTES);

    Block &at(uint64_t n)
    {
        return blocks.try_emplace(n, kBlock).first->second;
    }

    void object(uint64_t n, uint32_t type, uint64_t oid)
    {
        Block &b = at(n);
        put(b, 8, oid, 8);
        put(b, 16, 5, 8);
        put(b, 24, type, 4);
    }

    void seal(uint64_t n)
    {
        Block &b = at(n);
        put(b, 0, switch_apfs_checksum(b.data(), kBlock), 8);
    }

    void chunk(Block &cib, uint32_t i, uint64_t chunk_index, uint32_t free, uint64_t bitmap)
    {
        size_t ci = 40 + size_t(i) * 32;
        put(cib, ci, 5, 8);
        put(cib, ci + 8, chunk_index * kChunk, 8);
        put(cib, ci + 16, kChunk, 4);
        put(cib, ci + 20, free, 4);
        put(cib, ci + 24, bitmap, 8);
    }

    // A free-queue root leaf of one entry: `length` 1 is stored as a ghost.
    void queue(uint64_t n, uint64_t oid, uint64_t paddr, uint64_t length)
    {
        object(n, 0x40000002, oid);
        Block &b = at(n);
        put(b, 32, 7, 2);
        put(b, 36, 1, 4);
        put(b, 40, 0, 2);
        put(b, 42, 4, 2);
        put(b, 56, 0, 2);
        put(b, 58, length == 1 ? 0xFFFF : 8, 2);
        put(b, 60, 4, 8);
        put(b, 68, paddr, 8);
        if (length != 1) {
            put(b, kBlock - 40 - 8, length, 8);
        }
        seal(n);
    }

    void superblock(uint64_t n)
    {
        object(n, 0x80000001, 1);
        Block &b = at(n);
        std::memcpy(b.data() + 32, "NXSB", 4);
        put(b, 36, kBlock, 4);
        put(b, 40, kBlocks, 8);
        put(b, 104, 8, 4);
        put(b, 108, 8, 4);
        put(b, 112, 1, 8);
        put(b, 120, 9, 8);
        put(b, 136, 0, 4);
        put(b, 140, 2, 4);
        put(b, 152, 1024, 8);
        put(b, 180, 100, 4);
        put(b, 184, 1026, 8);
        seal(n);
    }

    Container()
    {
        object(1, 0x4000000C, 0);
        Block &map = at(1);
        put(map, 36, 3, 4);
        const uint64_t entries[3][4] = {{5, 0, 1024, 9}, {2, 9, 1027, 10}, {2, 9, 1029, 11}};
        for (size_t i = 0; i < 3; i++) {
            put(map, 40 + i * 40, entries[i][0], 4);
            put(map, 44 + i * 40, entries[i][1], 4);
            put(map, 48 + i * 40, kBlock, 4);
            put(map, 64 + i * 40, entries[i][2], 8);
            put(map, 72 + i * 40, entries[i][3], 8);
        }
        seal(1);
        Block &chunk0 = at(kPool + 5);
        set_bits(chunk0, 0, 17);
        set_bits(chunk0, kPool, kPool + kPoolBlocks);
        set_bits(chunk0, kPoolBitmap, kPoolBitmap + 16);
        set_bits(chunk0, 1000, 2000);
        set_bits(at(kPool + 6), 0, 100);
        uint64_t free = 0;
        for (uint32_t c = 0; c < 5; c++) {
            object(kPool + c, 0x40000007, 0);
            Block &cib = at(kPool + c);
            put(cib, 32, c, 4);
            put(cib, 36, 4, 4);
            for (uint32_t i = 0; i < 4; i++) {
                uint64_t index = uint64_t(c) * 4 + i;
                uint32_t used = index == 0 ? 17 + kPoolBlocks + 16 + 1000 : index == 3 ? 100 : 0;
                chunk(cib, i, index, uint32_t(kChunk - used), index == 0 ? kPool + 5 : index == 3 ? kPool + 6 : 0);
                free += kChunk - used;
            }
            seal(kPool + c);
        }
        set_bits(at(kPoolBitmap), 0, 7);
        object(9, 0x80000005, 1024);
        Block &sm = at(9);
        put(sm, 32, kBlock, 4);
        put(sm, 36, kChunk, 4);
        put(sm, 40, 4, 4);
        put(sm, 48, kBlocks, 8);
        put(sm, 56, 20, 8);
        put(sm, 64, 5, 4);
        put(sm, 72, free, 8);
        put(sm, 80, 2568, 4);
        put(sm, 128, 2568 + 5 * 8, 4);
        put(sm, 152, kPoolBlocks, 8);
        put(sm, 160, 1, 4);
        put(sm, 164, 16, 4);
        put(sm, 168, kPoolBitmap, 8);
        put(sm, 176, kPool, 8);
        put(sm, 200, 1, 8);
        put(sm, 208, 1027, 8);
        put(sm, 224, 1, 2);
        put(sm, 240, 10, 8);
        put(sm, 248, 1029, 8);
        put(sm, 264, 512, 2);
        put(sm, 328, 2528, 4);
        for (uint64_t c = 0; c < 5; c++) {
            put(sm, 2568 + c * 8, kPool + c, 8);
        }
        seal(9);
        queue(10, 1027, kPool + 7, 1);
        queue(11, 1029, 5000, 10);
        superblock(2);
        superblock(0);
    }

    switch_apfs_io io()
    {
        return {this,
                [](void *c, uint64_t n, uint8_t *buffer) {
                    auto *self = static_cast<Container *>(c);
                    auto found = self->blocks.find(n);
                    std::memset(buffer, 0, kBlock);
                    if (found != self->blocks.end()) {
                        std::memcpy(buffer, found->second.data(), kBlock);
                    }
                    return 0;
                },
                [](void *c, uint64_t n, const uint8_t *buffer) {
                    static_cast<Container *>(c)->at(n).assign(buffer, buffer + kBlock);
                    return 0;
                }};
    }

    int measure(uint64_t &size, uint64_t &minimum)
    {
        switch_apfs_io i = io();
        switch_apfs_layout layout{};
        int error = switch_apfs_measure(&i, work.data(), &layout);
        size = layout.blocks;
        minimum = layout.minimum;
        EXPECT_EQ(layout.pool_base, kPool);
        return error;
    }

    int fit(uint64_t blocks)
    {
        switch_apfs_io i = io();
        return switch_apfs_fit(&i, work.data(), blocks);
    }

    void reseal_queue(uint64_t paddr)
    {
        queue(11, 1029, paddr, 10);
    }
};

TEST(Fit, MeasuresWhatTheVolumesAndMetadataNeed)
{
    Container c;
    uint64_t size = 0;
    uint64_t minimum = 0;
    ASSERT_EQ(c.measure(size, minimum), 0);
    EXPECT_EQ(size, kBlocks);
    EXPECT_EQ(minimum, kMinimum);
}

TEST(Fit, CutsChunksChunkInfoBlocksAndThePool)
{
    Container c;
    const uint64_t target = 4 * kChunk + 1000;
    uint64_t free_before = get(c.at(9), 72, 8);
    ASSERT_EQ(c.fit(target), 0);
    for (uint64_t n : {uint64_t(0), uint64_t(2)}) {
        EXPECT_EQ(get(c.at(n), 40, 8), target);
        EXPECT_EQ(get(c.at(n), 180, 4), 2u);
    }
    const Block &sm = c.at(9);
    EXPECT_EQ(get(sm, 56, 8), 5u);
    EXPECT_EQ(get(sm, 64, 4), 2u);
    EXPECT_EQ(get(sm, 128, 4), 2568u + 2 * 8);
    EXPECT_EQ(get(sm, 2568 + 2 * 8, 8), 0u);
    EXPECT_EQ(get(sm, 152, 8), 3u * (5 + 2));
    EXPECT_EQ(get(sm, 224, 2), 1u);
    EXPECT_EQ(get(sm, 264, 2), 30u);
    uint64_t released = kPoolBlocks - 3 * (5 + 2);
    EXPECT_EQ(get(sm, 72, 8), free_before - (kBlocks - target) + released);
    const Block &cib1 = c.at(kPool + 1);
    EXPECT_EQ(get(cib1, 36, 4), 1u);
    EXPECT_EQ(get(cib1, 40 + 16, 4), 1000u);
    EXPECT_EQ(get(c.at(kPool), 40 + 20, 4), kChunk - (17 + kPoolBlocks + 16 + 1000) + released);
    EXPECT_EQ(get(c.at(kPoolBitmap), 0, 1), 0x63u);
    EXPECT_EQ(c.at(kPool + 5)[(kPool + 3 * 7) / 8] & (1u << ((kPool + 3 * 7) % 8)), 0);
    for (uint64_t n : {uint64_t(0), uint64_t(2), uint64_t(9), kPool, kPool + 1}) {
        EXPECT_EQ(get(c.at(n), 0, 8), switch_apfs_checksum(c.at(n).data(), kBlock)) << n;
    }
    uint64_t size = 0;
    uint64_t minimum = 0;
    ASSERT_EQ(c.measure(size, minimum), 0);
    EXPECT_EQ(size, target);
    EXPECT_EQ(minimum, kMinimum);
}

TEST(Fit, ItsOwnSizeChangesNothing)
{
    Container c;
    auto before = c.blocks;
    EXPECT_EQ(c.fit(kBlocks), 0);
    EXPECT_EQ(c.blocks, before);
}

TEST(Fit, RefusesWithoutWritingAnything)
{
    Container small;
    auto before = small.blocks;
    EXPECT_NE(small.fit(kMinimum - 1), 0);
    EXPECT_EQ(small.blocks, before);

    Container volume;
    put(volume.at(0), 184 + 2 * 8, 1030, 8);
    volume.seal(0);
    put(volume.at(2), 184 + 2 * 8, 1030, 8);
    volume.seal(2);
    before = volume.blocks;
    EXPECT_NE(volume.fit(4 * kChunk + 1000), 0) << "a volume past the smaller container's max_fs";
    EXPECT_EQ(volume.blocks, before);

    Container queued;
    queued.reseal_queue(kPool + 50);
    before = queued.blocks;
    EXPECT_NE(queued.fit(4 * kChunk + 1000), 0) << "a pending free inside the pool's released tail";
    EXPECT_EQ(queued.blocks, before);
}

TEST(Fit, AChunkWithABitmapPastTheCutRaisesTheMinimum)
{
    Container c;
    Block &cib = c.at(kPool + 2);
    put(cib, 40 + 2 * 32 + 24, kPool + 7, 8);
    put(cib, 40 + 2 * 32 + 20, kChunk - 1, 4);
    set_bits(c.at(kPool + 7), 5, 6);
    c.seal(kPool + 2);
    uint64_t size = 0;
    uint64_t minimum = 0;
    ASSERT_EQ(c.measure(size, minimum), 0);
    EXPECT_EQ(minimum, 10 * kChunk + 6);
    auto before = c.blocks;
    EXPECT_NE(c.fit(4 * kChunk + 1000), 0);
    EXPECT_EQ(c.blocks, before);
}

// What newfs_apfs on macOS 27 writes at each size (measured), and the rule's own steps around them.
TEST(QueueLimits, MainQueueSteps)
{
    const std::pair<uint64_t, uint16_t> sizes[] = {
        {4000, 1},      {4544, 1},      {4545, 3},      {9088, 3},        {13632, 3},    {13633, 4},
        {16384, 4},     {32768, 8},     {65536, 15},    {131776, 29},     {131777, 30},  {261888, 58},
        {262143, 58},   {262144, 116},  {262400, 116},  {393216, 174},    {524288, 231}, {786432, 347},
        {1048575, 462}, {1048576, 512}, {4194304, 512}, {268435456, 512},
    };
    for (auto [blocks, limit] : sizes) {
        EXPECT_EQ(switch_apfs_main_queue_limit(blocks), limit) << blocks << " blocks";
    }
}

TEST(QueueLimits, PoolQueueSteps)
{
    const std::pair<uint64_t, uint16_t> pools[] = {
        {6, 1},    {777, 1},  {1131, 1}, {1134, 1}, {1136, 1}, {1137, 3}, {1551, 3}, {2253, 3},
        {3099, 3}, {3408, 3}, {3409, 4}, {3411, 4}, {4536, 4}, {4560, 5}, {6195, 6}, {24774, 22},
    };
    for (auto [pool, limit] : pools) {
        EXPECT_EQ(switch_apfs_pool_queue_limit(pool), limit) << pool << " pool blocks";
    }
}

}  // namespace
