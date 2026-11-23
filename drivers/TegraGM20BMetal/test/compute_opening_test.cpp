// SPDX-License-Identifier: MIT
// compute_opening.h: a compute encoder's opening, method by method, waiting only through the waiting form of
// INVALIDATE_SHADER_CACHES (no WAIT_FOR_IDLE before it).
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "drivers/TegraGM20BMetal/compute_opening.h"

namespace {

struct Method {
    uint32_t subc, mthd, value;

    bool operator==(const Method &o) const
    {
        return subc == o.subc && mthd == o.mthd && value == o.value;
    }
};

// gm20b::Push's methods: immediate (op 4) or one incrementing value (op 1)
std::vector<Method> methods(const uint32_t *at, const uint32_t *end)
{
    std::vector<Method> out;
    while (at < end) {
        const uint32_t h = *at++, op = h >> 29, subc = h >> 13 & 7, mthd = (h & 0x1FFF) << 2, count = h >> 16 & 0x1FFF;
        if (op == 4) {
            out.push_back({subc, mthd, count});
        } else {
            for (uint32_t k = 0; k < count; k++)
                out.push_back({subc, mthd + 4 * k, *at++});
        }
    }
    return out;
}

std::vector<Method> opening()
{
    std::array<uint32_t, 32> words{};
    gm20b::Push p{words.data(), words.data() + words.size()};
    gm20b::computeOpening(p);
    return methods(words.data(), p.at);
}

TEST(GM20BComputeOpening, FlushesAndInvalidatesThroughTheWaitingFormAlone)
{
    const uint32_t flush = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) |
                           NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                           NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
    const std::vector<Method> want = {
        {gm20b::SUBC_COMPUTE, NVB1C0_INVALIDATE_SHADER_CACHES, flush},
        {gm20b::SUBC_COMPUTE, NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0},
        {gm20b::SUBC_COMPUTE, NVB1C0_INVALIDATE_SKED_CACHES, 0},
        {gm20b::SUBC_COMPUTE, NVB1C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0},
        {gm20b::SUBC_COMPUTE, NVB1C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0},
    };
    EXPECT_EQ(opening(), want);
}

TEST(GM20BComputeOpening, WaitsForIdleOnNoSubchannel)
{
    for (const Method &m : opening())
        EXPECT_NE(m.mthd, uint32_t(NVB1C0_WAIT_FOR_IDLE)) << m.subc;
}

}  // namespace
