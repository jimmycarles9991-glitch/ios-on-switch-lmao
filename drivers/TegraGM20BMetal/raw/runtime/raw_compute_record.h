// SPDX-License-Identifier: MIT
// A kernel's record with its program, as the raw path's compute work (raw_compute.h) places and dispatches one.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "raw_pack.h"

namespace raw {

struct ComputeResourceSlots {
    uint32_t buffers = 0, textures = 0;

    void add(const PackKernel &record)
    {
        for (uint32_t i = 0; i < record.entries; ++i) {
            const auto &entry = record.entry[i];
            if (entry.slot >= 31)
                continue;
            if (entry.kind == KernelBuffer)
                buffers |= 1u << entry.slot;
            else if (entry.kind == KernelTexture)
                textures |= 1u << entry.slot;
        }
    }

    template <typename Texture, typename Buffer> bool aliases(Texture texture, Buffer buffer) const
    {
        for (uint32_t slots = textures; slots; slots &= slots - 1)
            if (texture(uint32_t(__builtin_ctz(slots))))
                return true;
        for (uint32_t slots = buffers; slots; slots &= slots - 1)
            if (buffer(uint32_t(__builtin_ctz(slots))))
                return true;
        return false;
    }
};

// A kernel's record with its program (pack/mkpack.py's from a probe dispatch, or one the runtime made): placeCompute puts the
// program in the heap
struct ComputeRecord {
    PackKernel kernel = {};
    std::vector<uint8_t> code, data;  // NAK's program, and its shader data (uploaded 256-aligned after the code)
    uint8_t cbufType[8] = {};         // its constant buffer slots' PackCbufType (CbufRoot, CbufSet, ...)
    uint8_t cbufs = 0;
    std::string keyString;
    // the sampler headers its sampler entries name (slot 0x80 | index), placed among the device's at placeCompute
    std::vector<std::array<uint32_t, 8>> samplers;
    std::vector<uint32_t> spill;  // its argument table's words (PackKernel::spillWords): in a pack, its blob section's
};

}  // namespace raw
