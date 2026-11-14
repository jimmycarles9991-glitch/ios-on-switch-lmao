// SPDX-License-Identifier: GPL-2.0-or-later
// raw/runtime/raw_program_stores.h: a program writes memory a buffer binding names where its code holds a global or generic
// store, an atomic, a reduction or a surface write in an instruction slot; local and shared stores and the groups'
// scheduling words do not count.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_program_stores.h"

namespace {

// Two groups of four words, all zero but word `at`, whose bits 63-48 are `top`
bool writes(size_t at, uint16_t top)
{
    std::vector<uint8_t> code(64, 0);
    const uint64_t word = uint64_t(top) << 48;
    memcpy(code.data() + 8 * at, &word, sizeof word);
    return raw::programWritesMemory(code.data(), code.size());
}

TEST(ProgramStores, StoresAtomicsReductionsAndSurfaceWritesCount)
{
    EXPECT_FALSE(raw::programWritesMemory(std::vector<uint8_t>(64, 0).data(), 64));
    for (uint16_t top : {0xa000, 0xeed8, 0xed00, 0xeef0, 0xebf8, 0xeb20, 0xeb40, 0xea00})
        EXPECT_TRUE(writes(1, top)) << std::hex << top;
}

TEST(ProgramStores, LocalAndSharedStoresAndSchedulingWordsDoNot)
{
    EXPECT_FALSE(writes(1, 0xef50)) << "STL";
    EXPECT_FALSE(writes(1, 0xef58)) << "STS";
    EXPECT_FALSE(writes(1, 0xec00)) << "ATOMS";
    EXPECT_FALSE(writes(0, 0xeed8)) << "a scheduling word";
    EXPECT_FALSE(writes(4, 0xeed8)) << "the second group's scheduling word";
    EXPECT_TRUE(writes(5, 0xeed8));
}

}  // namespace
