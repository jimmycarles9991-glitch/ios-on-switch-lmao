// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_windows.h with raw_kernel_shadow.h's planner: a buffer bound off 16 bytes is reached through
// one window per buffer, address and length, so a dispatch of the same bytes reached in place, or through a window at
// another offset, is another resource over them: the window written is copied back before it, and filled again before
// its next use after it.
#include <gtest/gtest.h>

#include <vector>

#include "../raw/runtime/raw_kernel_windows.h"

namespace {

int gBuffer;

raw::KernelUse windowUse(uint64_t va, uint64_t bytes, bool writes, int8_t slot = 0)
{
    return {&gBuffer, {va, bytes}, true, writes, slot};
}

TEST(KernelWindows, OneWindowABufferAddressAndLength)
{
    raw::KernelWindows windows;
    uint64_t next = 0x10000;
    auto transient = [&](uint64_t bytes) {
        const uint64_t va = next;
        next += (bytes + 255) & ~uint64_t(255);
        return va;
    };
    std::vector<raw::KernelUse> uses = {windowUse(0x1004, 252, true),
                                        windowUse(0x1004, 252, false, 1),
                                        windowUse(0x1008, 248, false, 2),
                                        {&gBuffer, {0x1000, 256}, false, false}};
    windows.name(uses, transient);
    const auto *a = windows.find(uses[0].object), *b = windows.find(uses[1].object), *c = windows.find(uses[2].object);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(a, b) << "the same bytes of the same buffer share a window";
    EXPECT_NE(a, c);
    EXPECT_EQ(a->va & 15, 0u);
    EXPECT_EQ(a->span.va, 0x1004u);
    EXPECT_EQ(a->span.bytes, 252u);
    EXPECT_EQ(uses[3].object, &gBuffer) << "a use in place keeps the buffer";
    EXPECT_EQ(windows.find(&gBuffer), nullptr);
}

TEST(KernelWindows, AWindowWrittenIsCopiedBackBeforeItsBytesAreReachedInPlace)
{
    raw::KernelWindows windows;
    auto transient = [](uint64_t) {
        return uint64_t(0x20000);
    };
    std::vector<raw::KernelUse> first = {windowUse(0x1004, 252, true)};
    std::vector<raw::KernelUse> inPlace = {{&gBuffer, {0x1000, 256}, false, true}};
    std::vector<raw::KernelUse> again = {windowUse(0x1004, 252, false)};
    windows.name(first, transient);
    windows.name(again, transient);
    std::vector<raw::KernelUse> all = first;
    all.insert(all.end(), inPlace.begin(), inPlace.end());
    raw::KernelShadows shadows;
    shadows.begin(all.data(), all.size());
    EXPECT_TRUE(shadows.before(first.data(), first.size()).empty());
    shadows.after(first.data(), first.size());
    const auto &back = shadows.before(inPlace.data(), inPlace.size());
    ASSERT_EQ(back.copyBack.size(), 1u);
    EXPECT_EQ(back.copyBack[0], first[0].object);
    shadows.after(inPlace.data(), inPlace.size());
    const auto &refill = shadows.before(again.data(), again.size());
    ASSERT_EQ(refill.fill.size(), 1u) << "the in-place write is filled into the window again";
    EXPECT_EQ(refill.fill[0], again[0].object);
    EXPECT_TRUE(shadows.end().empty());
}

}  // namespace
