// SPDX-License-Identifier: MIT
// The units raw's command predication drives, as a model the host tests run its words on: the copy engine's copies and
// reducing semaphore releases, the compute class's render enable (two 64-bit operands, read when the mode is set), its
// inline writes and launches gated by it, and the host fetching a GP entry's segment from memory when it reaches it
// (NO_PREFETCH), so words the GPU wrote into a segment before are what it decodes.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_batch.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"

namespace predication_model {

constexpr uint64_t kBase = 0x0310000000ull;
constexpr uint32_t kCompute = 1, kCopy = 4;

// NVB0B5_LAUNCH_DMA's fields: DATA_TRANSFER_TYPE 1:0, SEMAPHORE_TYPE 4:3, SEMAPHORE_REDUCTION 17:14, its SIGN 18:18 and
// ENABLE 19:19
constexpr uint32_t bits(uint32_t v, uint32_t hi, uint32_t lo)
{
    return v >> lo & ((1u << (hi - lo + 1)) - 1);
}

struct Gpu {
    explicit Gpu(size_t bytes = 1 << 16) : memory(bytes)
    {}

    std::vector<uint8_t> memory;
    std::vector<uint32_t> launched;
    std::function<void(uint32_t)> onLaunch;  // what a launch's program does to memory
    // A device's memory instead of `memory` (a raw::Device over the kernel model), with only the compute class and the
    // copy engine modelled: other subchannels' methods (3D, 2D, host semaphores the stamps release) do nothing
    std::function<uint8_t *(uint64_t)> map;
    bool enabled = true;
    // false: the fetcher reads a segment's words as they were when the CPU wrote them (a GPU-written head unseen)
    bool seesHeads = true;
    uint64_t enableVa = 0, ceIn = 0, ceOut = 0, ceSemaphore = 0, i2mOut = 0;
    uint32_t ceLength = 0, cePayload = 0, i2mLength = 0, i2mAt = 0, pcas = 0;
    uint64_t decoded = 0;  // GP entries decoded past their first word

    uint8_t *at(uint64_t va)
    {
        if (map) {
            uint8_t *mapped = map(va);
            EXPECT_NE(mapped, nullptr) << std::hex << va;
            return mapped;
        }
        EXPECT_GE(va, kBase);
        EXPECT_LT(va - kBase, memory.size());
        return memory.data() + (va - kBase);
    }

    uint32_t word(uint64_t va)
    {
        uint32_t v;
        memcpy(&v, at(va), 4);
        return v;
    }

    uint64_t dword(uint64_t va)
    {
        return uint64_t(word(va)) | uint64_t(word(va + 4)) << 32;
    }

    void put(uint64_t va, uint32_t v)
    {
        memcpy(at(va), &v, 4);
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
        case NVB0B5_LAUNCH_DMA: {
            if (bits(v, 1, 0) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE)
                memmove(at(ceOut), at(ceIn), ceLength);
            if (bits(v, 4, 3) != NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE_NONE) {
                uint32_t next = cePayload;
                if (bits(v, 19, 19)) {
                    EXPECT_EQ(bits(v, 18, 18), uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_SIGN_UNSIGNED));
                    const uint32_t old = word(ceSemaphore);
                    next = bits(v, 17, 14) == NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN ? std::min(old, cePayload)
                                                                                         : std::max(old, cePayload);
                }
                put(ceSemaphore, next);
            }
            break;
        }
        default:
            break;
        }
    }

    void compute(uint32_t m, uint32_t v)
    {
        switch (m) {
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_A:
            enableVa = uint64_t(v) << 32 | (enableVa & 0xffffffff);
            break;
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_B:
            enableVa = (enableVa & ~0xffffffffull) | v;
            break;
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_C:
            if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE)
                enabled = true;
            else if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL)
                enabled = dword(enableVa) == dword(enableVa + 16);
            else if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL)
                enabled = dword(enableVa) != dword(enableVa + 16);
            else
                ADD_FAILURE() << "render enable mode " << v;
            break;
        case NVB1C0_LINE_LENGTH_IN:
            i2mLength = v;
            break;
        case NVB1C0_OFFSET_OUT_UPPER:
            i2mOut = uint64_t(v) << 32 | (i2mOut & 0xffffffff);
            break;
        case NVB1C0_OFFSET_OUT:
            i2mOut = (i2mOut & ~0xffffffffull) | v;
            break;
        case NVB1C0_LAUNCH_DMA:
            i2mAt = 0;
            break;
        case NVB1C0_LOAD_INLINE_DATA:
            EXPECT_LT(i2mAt, i2mLength);
            if (enabled)
                put(i2mOut + i2mAt, v);
            i2mAt += 4;
            break;
        case NVB1C0_SEND_PCAS_A:
            pcas = v;
            break;
        case NVB1C0_SEND_SIGNALING_PCAS_B:
            if (enabled) {
                launched.push_back(pcas);
                if (onLaunch)
                    onLaunch(pcas);
            }
            break;
        default:  // host methods (NOP, SET_REFERENCE, WFI) and the waits and cache operations the model needs nothing for
            break;
        }
    }

    void method(uint32_t subchannel, uint32_t m, uint32_t v)
    {
        if (subchannel == kCopy)
            copyEngine(m, v);
        else if (subchannel == kCompute)
            compute(m, v);
        else if (!map)
            ADD_FAILURE() << "subchannel " << subchannel;
    }

    // One segment of `count` words as the host decodes it: NV_FIFO_DMA_NOP (0) skipped, END_PB_SEGMENT ending it (a
    // fetcher that does not see the GPU's head writes, `seesHeads` false, takes a segment's first word as the CPU's NOP).
    // The words decoded.
    size_t run(const uint32_t *words, size_t count, bool segment = false)
    {
        for (size_t i = 0; i < count;) {
            const uint32_t hdr = words[i++];
            if (!hdr)
                continue;
            const uint32_t op = hdr >> 29, subc = hdr >> 13 & 7, mthd = (hdr & 0xfff) << 2, n = hdr >> 16 & 0x1fff;
            if (op == 7) {
                if (segment && i == 1 && !seesHeads)
                    continue;
                return i;
            }
            if (op == 4) {
                method(subc, mthd, n);
                continue;
            }
            EXPECT_TRUE(op == 1 || op == 3 || op == 5) << "header " << hdr;
            for (uint32_t k = 0; k < n && i < count; ++k) {
                const uint32_t step = op == 1 ? k : (op == 5 && k ? 1 : 0);
                method(subc, mthd + 4 * step, words[i++]);
            }
        }
        return count;
    }

    void run(const std::vector<uint32_t> &words)
    {
        run(words.data(), words.size());
    }

    // GP entries in order, each segment read from memory when its turn comes
    void runEntries(const raw::PushBatches &entries)
    {
        for (size_t e = 0; e < entries.size(); ++e) {
            std::vector<uint32_t> words(entries[e].dwords);
            memcpy(words.data(), at(entries[e].va), words.size() * 4);
            if (run(words.data(), words.size(), true) > 1)
                ++decoded;
        }
    }
};

}  // namespace predication_model
