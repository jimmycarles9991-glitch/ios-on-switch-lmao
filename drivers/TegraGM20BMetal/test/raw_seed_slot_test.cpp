// SPDX-License-Identifier: MIT
// A seeded record's large programs run from the installed seed's pages (raw_cache.h findSeedSlot), on a client device over
// the kernel model.
#include "raw_kernel_model.h"
#include "empty_pack.h"
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_gpu.h"
#include "../raw/runtime/raw_program_pages.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedPackWriter.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kStack = 0x5eed;
constexpr size_t kShaderHeaderBytes = 80;

std::string bigEndian(uint64_t value)
{
    std::string bytes;
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes += char(value >> shift);
    return bytes;
}

raw::CapturedProgram program(const char *id, size_t bytes, uint8_t fill)
{
    raw::CapturedProgram p;
    p.id = id;
    p.image.assign(bytes, 0);
    for (size_t i = kShaderHeaderBytes; i < bytes; ++i)
        p.image[i] = uint8_t(fill + i);
    return p;
}

class SeedSlots : public testing::Test {
protected:
    std::string seed;
    raw::RecordCache cache;
    raw::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = 1ull << 40;
        const char *tmp = getenv("TEST_TMPDIR");
        seed = std::string(tmp ? tmp : "/tmp") + "/seed-slots-XXXXXX";
        ASSERT_NE(mkdtemp(seed.data()), nullptr);
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    void TearDown() override
    {
        dev.runSlotsFrom(nullptr);
    }

    // The seed holding `slot` as native_seed_manifest.h writeSlots writes it, attached and asked first for every slot
    void install(const std::vector<uint8_t> &slot)
    {
        std::string entry(slot.begin(), slot.end());
        entry.resize(raw::programSlotEntryBytes(slot.size()));
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        memcpy(identity, &kStack, sizeof kStack);
        std::string error;
        char dir[32];
        snprintf(dir, sizeof dir, "/raw-%016llx", (unsigned long long)kStack);
        ASSERT_TRUE(switch_seed_pack::write(
            seed + dir, 8, identity, {{bigEndian(raw::seedSlotName(slot.data(), slot.size())), 0, "", entry}}, error))
            << error;
        ASSERT_TRUE(cache.attachSeed(seed, kStack));
        raw::RecordCache *c = &cache;
        dev.runSlotsFrom([c](const uint8_t *s, uint32_t size, const uint8_t *&mapping, uint64_t &bytes) {
            return c->findSeedSlot(s, size, mapping, bytes);
        });
    }

    // Whether the model holds an import of host pages covering [at, at + bytes)
    static bool imported(const uint8_t *at, uint64_t bytes)
    {
        for (const auto &[handle, import] : kernel_model::model.imports) {
            (void)handle;
            if (import.first <= uintptr_t(at) && uintptr_t(at) + bytes <= import.first + import.second)
                return true;
        }
        return false;
    }
};

// The record's vertex program is a page and more: its slot is the seed's pages imported and bound in the program window
TEST_F(SeedSlots, ASeededProgramOfAPageOrMoreRunsFromTheSeedsPages)
{
    raw::RuntimeRecord r;
    r.keyString = "seeded state";
    r.programs[0] = program("vs", 0x5000, 0x11);
    r.programs[1] = program("fs", 0x100, 0x22);
    raw::PackProgram info;
    const std::vector<uint8_t> slot = raw::programSlot(r.programs[0], info);
    ASSERT_GE(slot.size(), raw::kProgramSlotPage);
    install(slot);
    const uint64_t inPlaceBefore = dev.heapInPlaceBytes();
    std::string why;
    dev.addRecord(r, false, why);
    EXPECT_GE(dev.heapInPlaceBytes(), inPlaceBefore + slot.size()) << why;
    const uint8_t *mapping = nullptr;
    uint64_t bytes = 0;
    const uint8_t *found = cache.findSeedSlot(slot.data(), slot.size(), mapping, bytes);
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(imported(found, slot.size() + raw::kProgramPrefetchBytes));
}

// A slot the seed does not hold byte for byte is copied
TEST_F(SeedSlots, AProgramTheSeedHoldsNoneOfIsCopied)
{
    raw::RuntimeRecord r;
    r.keyString = "unseeded state";
    r.programs[0] = program("vs", 0x5000, 0x33);
    r.programs[1] = program("fs", 0x100, 0x44);
    raw::PackProgram info;
    std::vector<uint8_t> other = raw::programSlot(r.programs[0], info);
    other[0x200] ^= 1;
    install(other);
    const uint64_t inPlaceBefore = dev.heapInPlaceBytes();
    std::string why;
    dev.addRecord(r, false, why);
    EXPECT_EQ(dev.heapInPlaceBytes(), inPlaceBefore) << why;
}

}  // namespace
