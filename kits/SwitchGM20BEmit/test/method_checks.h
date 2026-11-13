// SPDX-License-Identifier: MIT
// What GM20B's classes take, for the host tests' pushes and their runs of the Fermi MME: a method its class lacks
// (SwitchGM20BMethods.h) or a shadow scratch word past the MME's 128 fails the test, as the GPU kills the channel with
// ILLEGAL_MTHD for either.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ios>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20BMethods.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h"

namespace switch_gm20b_test {

constexpr uint32_t kMaxwellB = 0xb197, kScratchMethod = 0x3400, kMacroMethod = 0x3800;

// The class a method on `subchannel` reaches, as both drivers bind them (Mesa's nv_push.h): 3D, compute,
// inline-to-memory, 2D, copy, and the host below method 0x100 on every subchannel
inline uint32_t subchannelClass(uint32_t subchannel, uint32_t method)
{
    static const uint32_t classes[] = {0xb197, 0xb1c0, 0xa140, 0x902d, 0xb0b5};
    if (method < 0x100)
        return 0xb06f;
    return subchannel < sizeof classes / sizeof classes[0] ? classes[subchannel] : 0;
}

inline void expectMethod(uint32_t cls, uint32_t method)
{
    EXPECT_TRUE(switch_gm20b_method_legal(cls, method))
        << "class 0x" << std::hex << cls << " has no method 0x" << method;
}

// A method a 3D macro writes, or the state it reads
inline void expectMaxwellB(uint32_t method)
{
    expectMethod(kMaxwellB, method);
}

inline bool isScratch(uint32_t method)
{
    return method >= kScratchMethod && method < kMacroMethod;
}

// The MME's shadow scratch: the words SET_MME_SHADOW_SCRATCH writes and a macro's state reads
struct Scratch {
    uint32_t words[SWITCH_GM20B_SCRATCH_WORDS] = {};
    uint32_t past = 0;

    uint32_t &operator[](uint32_t word)
    {
        EXPECT_LT(word, uint32_t(SWITCH_GM20B_SCRATCH_WORDS)) << "the Fermi MME has no scratch word " << word;
        return word < SWITCH_GM20B_SCRATCH_WORDS ? words[word] : past;
    }

    uint32_t &method(uint32_t m)
    {
        expectMaxwellB(m);
        return (*this)[(m - kScratchMethod) / 4];
    }
};

// Every method of push segments (a job's GP entries' words, back to back) checked against its subchannel's class, as
// the PBDMA reads them (clb06f.h DMA): a method's data runs on into the next segment, word 0 is NV_FIFO_DMA_NOP, and
// END_PB_SEGMENT ends its segment (raw_predication.h's loop heads). False on any other header than those and the
// method forms 1 INC, 3 NON_INC, 4 IMMD and 5 ONE_INC.
inline bool expectSegmentMethods(const uint32_t *words, const uint32_t *segmentWords, size_t segments)
{
    uint32_t op = 0, subchannel = 0, method = 0, left = 0;
    for (size_t s = 0; s < segments; s++) {
        const uint32_t *w = words, *end = words + segmentWords[s];
        words = end;
        while (w < end) {
            if (left) {
                expectMethod(subchannelClass(subchannel, method), method);
                w++;
                left--;
                if (op != 3)
                    method += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            const uint32_t header = *w++, form = header >> 29;
            if (header == 0)
                continue;
            if (form == 7)
                break;
            subchannel = header >> 13 & 7;
            method = (header & 0xfff) << 2;
            if (form == 4) {
                expectMethod(subchannelClass(subchannel, method), method);
                continue;
            }
            if (form != 1 && form != 3 && form != 5)
                return false;
            op = form;
            left = header >> 16 & 0x1fff;
        }
    }
    return true;
}

inline bool expectPushMethods(const uint32_t *words, size_t count)
{
    const uint32_t segment = uint32_t(count);
    return expectSegmentMethods(words, &segment, 1);
}

}  // namespace switch_gm20b_test
