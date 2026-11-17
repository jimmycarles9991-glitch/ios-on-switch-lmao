// SPDX-License-Identifier: MIT
// The units raw's visibility results drive, as a model the host tests run its words on: the 3D engine's samples-passed
// counter and its reports and releases (four words, or one word plain or reduced into memory), and the copy engine's
// copies and reducing semaphore releases, each launch gated by the copy engine's render enable (two 64-bit operands).
// A word nothing wrote reads 0xdeadbeef, as a reused report slot holds a query's before.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace visibility_model {

constexpr uint32_t k3D = 0, kCopy = 4;

constexpr uint32_t bits(uint32_t v, uint32_t hi, uint32_t lo)
{
    return v >> lo & ((1u << (hi - lo + 1)) - 1);
}

inline uint32_t reduce(uint32_t op, uint32_t old, uint32_t operand, bool copyEngine)
{
    if (copyEngine) {
        switch (op) {
        case NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN:
            return std::min(old, operand);
        case NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMAX:
            return std::max(old, operand);
        case NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IOR:
            return old | operand;
        case NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IADD:
            return old + operand;
        }
    } else {
        switch (op) {
        case NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_OP_RED_ADD:
            return old + operand;
        case NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_OP_RED_MIN:
            return std::min(old, operand);
        case NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_OP_RED_MAX:
            return std::max(old, operand);
        case NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_OP_RED_OR:
            return old | operand;
        }
    }
    ADD_FAILURE() << "reduction " << op << (copyEngine ? " (copy engine)" : " (3D)");
    return old;
}

struct Gpu {
    std::map<uint64_t, uint32_t> words;
    uint64_t counter = 0, stamp = 0x5157000000000000ull;
    bool counting = false, enabled = true;
    uint64_t reportVa = 0, enableVa = 0, ceIn = 0, ceOut = 0, ceSemaphore = 0;
    uint32_t reportPayload = 0, ceLength = 0, cePayload = 0;

    uint32_t word(uint64_t va) const
    {
        EXPECT_EQ(va & 3, 0u);
        const auto it = words.find(va);
        return it == words.end() ? 0xdeadbeefu : it->second;
    }

    uint64_t dword(uint64_t va) const
    {
        return uint64_t(word(va)) | uint64_t(word(va + 4)) << 32;
    }

    void put(uint64_t va, uint32_t v)
    {
        EXPECT_EQ(va & 3, 0u);
        words[va] = v;
    }

    void putDword(uint64_t va, uint64_t v)
    {
        put(va, uint32_t(v));
        put(va + 4, uint32_t(v >> 32));
    }

    // The draws while the counter counts pass `samples`
    void draw(uint64_t samples)
    {
        if (counting)
            counter += samples;
    }

    void report(uint32_t d)
    {
        const uint32_t operation = bits(d, 1, 0), report = bits(d, 27, 23);
        const bool oneWord = bits(d, 28, 28) == NVB197_SET_REPORT_SEMAPHORE_D_STRUCTURE_SIZE_ONE_WORD;
        if (operation == NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_RELEASE) {
            EXPECT_TRUE(oneWord);
            EXPECT_EQ(bits(d, 3, 3), 0u) << "the releases here write";
            put(reportVa, reportPayload);
            return;
        }
        ASSERT_EQ(operation, uint32_t(NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_REPORT_ONLY));
        if (!oneWord) {
            ASSERT_EQ(report, uint32_t(NVB197_SET_REPORT_SEMAPHORE_D_REPORT_ZPASS_PIXEL_CNT64));
            putDword(reportVa, counter);
            putDword(reportVa + 8, ++stamp);
            return;
        }
        ASSERT_EQ(report, uint32_t(NVB197_SET_REPORT_SEMAPHORE_D_REPORT_ZPASS_PIXEL_CNT));
        const uint32_t count = uint32_t(counter);
        if (!bits(d, 3, 3)) {
            put(reportVa, count);
            return;
        }
        EXPECT_EQ(bits(d, 18, 17), uint32_t(NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_FORMAT_UNSIGNED_32));
        put(reportVa, reduce(bits(d, 11, 9), word(reportVa), count, false));
    }

    void threeD(uint32_t m, uint32_t v)
    {
        switch (m) {
        case NVB197_CLEAR_REPORT_VALUE:
            EXPECT_EQ(v, uint32_t(NVB197_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT));
            counter = 0;
            break;
        case NVB197_SET_ZPASS_PIXEL_COUNT:
            counting = v != 0;
            break;
        case NVB197_SET_REPORT_SEMAPHORE_A:
            reportVa = uint64_t(v) << 32 | (reportVa & 0xffffffff);
            break;
        case NVB197_SET_REPORT_SEMAPHORE_B:
            reportVa = (reportVa & ~0xffffffffull) | v;
            break;
        case NVB197_SET_REPORT_SEMAPHORE_C:
            reportPayload = v;
            break;
        case NVB197_SET_REPORT_SEMAPHORE_D:
            report(v);
            break;
        case NVB197_WAIT_FOR_IDLE:
            break;
        default:
            ADD_FAILURE() << "3D method " << std::hex << m;
        }
    }

    void copyEngine(uint32_t m, uint32_t v)
    {
        switch (m) {
        case NVB0B5_OFFSET_IN_UPPER:
            ceIn = uint64_t(v) << 32 | (ceIn & 0xffffffff);
            break;
        case NVB0B5_OFFSET_IN_LOWER:
            ceIn = (ceIn & ~0xffffffffull) | v;
            break;
        case NVB0B5_OFFSET_OUT_UPPER:
            ceOut = uint64_t(v) << 32 | (ceOut & 0xffffffff);
            break;
        case NVB0B5_OFFSET_OUT_LOWER:
            ceOut = (ceOut & ~0xffffffffull) | v;
            break;
        case NVB0B5_LINE_LENGTH_IN:
            ceLength = v;
            break;
        case NVB0B5_SET_SEMAPHORE_A:
            ceSemaphore = uint64_t(v) << 32 | (ceSemaphore & 0xffffffff);
            break;
        case NVB0B5_SET_SEMAPHORE_B:
            ceSemaphore = (ceSemaphore & ~0xffffffffull) | v;
            break;
        case NVB0B5_SET_SEMAPHORE_PAYLOAD:
            cePayload = v;
            break;
        case NVB0B5_SET_RENDER_ENABLE_A:
            enableVa = uint64_t(v) << 32 | (enableVa & 0xffffffff);
            break;
        case NVB0B5_SET_RENDER_ENABLE_B:
            enableVa = (enableVa & ~0xffffffffull) | v;
            break;
        case NVB0B5_SET_RENDER_ENABLE_C:
            if (v == NVB0B5_SET_RENDER_ENABLE_C_MODE_TRUE)
                enabled = true;
            else if (v == NVB0B5_SET_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL)
                enabled = dword(enableVa) != dword(enableVa + 16);
            else if (v == NVB0B5_SET_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL)
                enabled = dword(enableVa) == dword(enableVa + 16);
            else
                ADD_FAILURE() << "copy render enable mode " << v;
            break;
        case NVB0B5_LAUNCH_DMA: {
            if (!enabled)
                break;
            if (bits(v, 1, 0) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE)
                for (uint32_t i = 0; i < ceLength; i += 4)
                    put(ceOut + i, word(ceIn + i));
            if (bits(v, 4, 3) != NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE_NONE) {
                uint32_t next = cePayload;
                if (bits(v, 19, 19)) {
                    EXPECT_EQ(bits(v, 18, 18), uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_SIGN_UNSIGNED));
                    next = reduce(bits(v, 17, 14), word(ceSemaphore), cePayload, true);
                }
                put(ceSemaphore, next);
            }
            break;
        }
        default:  // pitches, line counts, remap
            break;
        }
    }

    void run(const std::vector<uint32_t> &push)
    {
        std::vector<switch_gm20b_method> decoded(push.size() * 2 + 1);
        const long n = switch_gm20b_push_decode(push.data(), push.size(), decoded.data(), decoded.size());
        ASSERT_GE(n, 0);
        for (long i = 0; i < n; i++) {
            const switch_gm20b_method &m = decoded[size_t(i)];
            if (m.subchannel == k3D)
                threeD(m.method, m.value);
            else if (m.subchannel == kCopy)
                copyEngine(m.method, m.value);
            else
                ADD_FAILURE() << "subchannel " << unsigned(m.subchannel);
        }
    }
};

}  // namespace visibility_model
