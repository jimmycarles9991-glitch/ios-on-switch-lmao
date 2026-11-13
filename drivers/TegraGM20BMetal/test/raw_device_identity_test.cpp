// SPDX-License-Identifier: MIT
// raw/runtime/raw_device_answers.h: the identity's GPU subtype is the one the selected IPSW ships AGX binaries for, its
// QuartzCore's default.metallib slice beside the AIR one (24B119: 0xd2, J181's A13; 21H461: 0x61, J120's A10X), and its
// name is an Apple GPU's, which frameworks open that GPU's archives for.
#include "../raw/runtime/raw_archive.h"
#include "../raw/runtime/raw_device_answers.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

uint32_t bigEndian(const uint8_t *w)
{
    return uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3];
}

std::vector<uint32_t> appleGpuSubtypes(const char *path)
{
    std::vector<uint32_t> subtypes;
    FILE *f = fopen(path, "rb");
    if (!f)
        return subtypes;
    uint8_t head[8 + 20 * 16] = {};
    const size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    if (got < 8 || bigEndian(head) != 0xcafebabe)
        return subtypes;
    for (uint32_t i = 0; i < bigEndian(head + 4) && 8 + 20 * (i + 1) <= got; i++)
        if (bigEndian(head + 8 + 20 * i) == raw::kAppleGpuCpuType)
            subtypes.push_back(bigEndian(head + 8 + 20 * i + 4));
    return subtypes;
}

TEST(RawDeviceIdentity, TheGpuIsTheOneTheIpswShipsBinariesFor)
{
    const char *quartzCore = getenv("SWITCH_QUARTZCORE");
    ASSERT_NE(quartzCore, nullptr);
    const std::vector<uint32_t> shipped = appleGpuSubtypes(quartzCore);
    ASSERT_EQ(shipped.size(), 1u) << quartzCore;
    EXPECT_EQ(raw::answers::kThisKind.gpuSubtype, shipped[0]) << raw::answers::kThisKind.kind;
}

TEST(RawDeviceIdentity, TheNameIsAnAppleGpusAsMetalJoinsIt)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    EXPECT_EQ(k.gpuVendor, "Apple") << "CMImaging's _identifyGPU opens per-GPU archives only for an Apple name";
    EXPECT_FALSE(k.gpuFamily.empty());
    EXPECT_EQ(k.gpuProduct, "GPU");
}

}  // namespace
