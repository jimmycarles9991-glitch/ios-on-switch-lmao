// SPDX-License-Identifier: MIT
// The raw path's device-wide handle tables and heap sets (raw_heap_tables.h), read as m2v's programs read them: an ID is
// searched by m2v's lookup_host (memory/handles.rs), its element indexes a set-1 array.
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "../raw/runtime/raw_heap_tables.h"

namespace {

constexpr uint32_t kTextures = 8192, kSamplers = 1024;
constexpr uint32_t kSampled2d = 0x5, kSampled2dArray = 0x25, kSamplerClass = 0x2;  // metal2vulkan heap_class_code

uint64_t entryId(const raw::HandleEntry &e)
{
    return uint64_t(e.idHi) << 32 | e.idLo;
}

// m2v's lookup_host: SEARCH_STEPS halvings over a table sorted by id whose first entry is the null entry
uint32_t lookupHost(const uint8_t *bytes, uint32_t count, uint64_t id)
{
    const auto *table = reinterpret_cast<const raw::HandleEntry *>(bytes);
    uint64_t base = 0, len = count;
    for (int step = 0; step < 16; step++) {
        const uint64_t half = len >> 1, mid = base + half;
        if (mid < count && entryId(table[mid]) <= id)
            base = mid;
        len -= half;
    }
    return base < count && entryId(table[base]) == id ? table[base].element : 0;
}

// An image class holds texture facts of type 2 (MTLTextureType2D); the array class, type 3
bool holds(uint32_t heapClass, const raw::HeapTextureFacts *facts)
{
    return (heapClass == kSampled2d && facts->textureType == 2) ||
           (heapClass == kSampled2dArray && facts->textureType == 3);
}

class HeapTablesTest : public testing::Test {
protected:
    std::vector<uint8_t> tables = std::vector<uint8_t>(raw::HeapTables::tablesBytes(kTextures, kSamplers));
    std::vector<uint8_t> roots = std::vector<uint8_t>(raw::HeapTables::kRootsBytes);
    std::vector<uint8_t> setBytes = std::vector<uint8_t>(0x10000, 0xEE);
    raw::HeapTables heap;

    void SetUp() override
    {
        heap.attach({tables.data(), 0x100000, tables.size()}, {roots.data(), 0x200000, roots.size()}, kTextures,
                    kSamplers, 0x1000, 1ull << 39, holds);
    }

    const uint8_t *samplerTable() const
    {
        return tables.data() + heap.textureTableBytes();
    }

    static raw::HeapTexture texture(uint32_t sampleTic, uint32_t type, uint32_t flags = 0)
    {
        raw::HeapTexture t;
        t.sampleTic = sampleTic;
        t.flags = flags;
        t.facts.textureType = type;
        return t;
    }

    static raw::PackPipeline record()
    {
        raw::PackPipeline rec = {};
        rec.heapArrays = 3;
        rec.heap[0] = {kSampled2d, 0};
        rec.heap[1] = {kSampled2dArray, 0x8000};
        rec.heap[2] = {kSamplerClass, 0xC000};
        rec.heapBytes = 0x10000;
        return rec;
    }

    uint32_t element(const raw::HeapSet &set, uint32_t array, uint32_t i) const
    {
        uint32_t word;
        memcpy(&word, set.memory.cpu + set.array[array].offset + 4 * i, 4);
        return word;
    }
};

TEST_F(HeapTablesTest, AnIdResolvesToItsHeaderOnlyWhileItsObjectIsNamed)
{
    EXPECT_EQ(heap.textureTableBytes(), kTextures * 16);
    EXPECT_EQ(heap.samplerTableVa(), 0x100000 + kTextures * 16);
    for (uint32_t tic : {1u, 5u, 4095u, 8191u}) {
        const uint64_t id = raw::textureResourceId(tic);
        EXPECT_EQ(lookupHost(tables.data(), kTextures, id), 0u) << tic;
        const raw::HeapTexture t = texture(tic + 1, 2, 0x20002);
        heap.texture(tic, &t);
        EXPECT_EQ(lookupHost(tables.data(), kTextures, id), tic);
        EXPECT_EQ(reinterpret_cast<const raw::HandleEntry *>(tables.data())[tic].flags, 0x20002u);
        heap.texture(tic, nullptr);
        EXPECT_EQ(lookupHost(tables.data(), kTextures, id), 0u);
    }
    EXPECT_EQ(lookupHost(tables.data(), kTextures, 0), 0u);
    EXPECT_EQ(lookupHost(tables.data(), kTextures, raw::textureResourceId(kTextures)), 0u);
    EXPECT_EQ(lookupHost(tables.data(), kTextures, 5), 0u);
    const raw::HeapSampler sampler = {7, 0x1234};
    heap.sampler(7, &sampler);
    EXPECT_EQ(lookupHost(samplerTable(), kSamplers, raw::samplerResourceId(7)), 7u);
    EXPECT_EQ(reinterpret_cast<const raw::HandleEntry *>(samplerTable())[7].flags, sampler.word);
    EXPECT_EQ(lookupHost(samplerTable(), kSamplers, raw::samplerResourceId(8)), 0u);
    heap.sampler(7, nullptr);
    EXPECT_EQ(lookupHost(samplerTable(), kSamplers, raw::samplerResourceId(7)), 0u);
}

TEST_F(HeapTablesTest, ASetHoldsEveryNamedObjectWhereItsClassFitsAndFollowsThemAfter)
{
    const raw::HeapTexture flat = texture(40, 2), layered = texture(41, 3);
    heap.texture(9, &flat);
    const raw::HeapSampler pixels = {12, raw::kSamplerWordPixelCoordinates};
    heap.sampler(3, &pixels);
    const raw::PackPipeline rec = record();
    EXPECT_EQ(heap.find(raw::heapLayout(rec)), nullptr);
    const raw::HeapSet *set = heap.add(raw::heapLayout(rec), {setBytes.data(), 0x300000, setBytes.size()});
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(heap.find(raw::heapLayout(rec)), set);
    EXPECT_EQ(element(*set, 0, 9), 40u);
    EXPECT_EQ(element(*set, 1, 9), 0u);
    EXPECT_EQ(element(*set, 2, 3), 12u << 20);
    EXPECT_EQ(element(*set, 0, 10), 0u);
    heap.texture(10, &layered);
    EXPECT_EQ(element(*set, 0, 10), 0u);
    EXPECT_EQ(element(*set, 1, 10), 41u);
    heap.texture(9, nullptr);
    heap.sampler(3, nullptr);
    EXPECT_EQ(element(*set, 0, 9), 0u);
    EXPECT_EQ(element(*set, 2, 3), 0u);
    raw::PackPipeline other = rec;
    other.heap[1].offset = 0x4000;
    EXPECT_EQ(heap.find(raw::heapLayout(other)), nullptr);
}

TEST_F(HeapTablesTest, TheRootsAreNullThenTheSinkThenTheIdentityOverTheConnectionsVas)
{
    const auto *root = reinterpret_cast<const raw::RootEntry *>(roots.data());
    EXPECT_EQ(root[0].host | root[0].guest | root[0].size | root[0].bindOffset, 0u);
    EXPECT_EQ(root[1].host, heap.sinkVa());
    EXPECT_EQ(heap.rootTableVa() + 32 * root[1].guest, heap.identityVa());
    EXPECT_EQ(root[1].size, 1u);
    EXPECT_EQ(root[2].host, 0x1000u);
    EXPECT_EQ(root[2].guest, 0x1000u);
    EXPECT_EQ(root[2].size, (1ull << 39) - 0x1000);
    EXPECT_EQ(heap.statusVa(), heap.sinkVa() + raw::kRootSinkBytes);
    EXPECT_LE(heap.statusVa() + raw::kExecutionStatusBytes - 0x200000, raw::HeapTables::kRootsBytes);
}

// m2v's resident_fields: a set below the table is 0 - its distance in entries, a set above it the distance itself
TEST(DrawRoots, R0IsM2vsOnEitherSideOfTheTable)
{
    const uint64_t identity = 0x200040;
    EXPECT_EQ(raw::residentFirst(identity - 0x4000, identity), 0x4000u / 32);
    EXPECT_EQ(raw::residentFirst(identity + 0x40, identity), 0 - uint64_t(2));
    EXPECT_EQ(raw::residentFirst(identity + 0x10000000, identity), 0 - uint64_t(0x10000000 / 32));
    for (uint64_t table : {identity - 0x4000, identity + 0x40, identity + 0x10000000})
        EXPECT_EQ(table + 32 * raw::residentFirst(table, identity), identity) << table;
}

// A draw's table: null, the sink over the resident set, an entry per slot it holds up to its last, rounded to 16 bytes,
// with the bind offset; a slot outside the mask or bound to nothing is null
TEST(DrawRoots, AnEntryPerHeldSlotUpToTheLast)
{
    const uint32_t slots = 1u << 1 | 1u << 3;
    ASSERT_EQ(raw::drawRootEntries(slots), 6u);
    raw::RootSlot slot[4] = {{0x5000, 64, 0}, {0x6000, 30, 32}, {0x7000, 64, 0}, {0, 0, 0}};
    std::vector<raw::RootEntry> table(6, raw::RootEntry{1, 1, 1, 1});
    const uint64_t va = 0x9000000, identity = 0x200040, sink = 0x200060;
    raw::writeDrawRoots(table.data(), va, slots, slot, sink, identity);
    EXPECT_EQ(table[0].host | table[0].guest | table[0].size | table[0].bindOffset, 0u);
    EXPECT_EQ(table[1].host, sink);
    EXPECT_EQ(va + 32 * table[1].guest, identity);
    EXPECT_EQ(table[1].size, 1u);
    EXPECT_EQ(table[2].host | table[2].size, 0u);  // slot 0: not held
    EXPECT_EQ(table[3].host, 0x6000u);
    EXPECT_EQ(table[3].guest, 0x6000u);
    EXPECT_EQ(table[3].size, 32u);
    EXPECT_EQ(table[3].bindOffset, 32u);
    EXPECT_EQ(table[4].host | table[4].size, 0u);  // slot 2: not held
    EXPECT_EQ(table[5].host | table[5].size, 0u);  // slot 3: held, bound to nothing
}

}  // namespace
