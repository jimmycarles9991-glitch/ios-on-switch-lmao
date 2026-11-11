// SPDX-License-Identifier: MIT
// A kernel's record with its program, as the raw path's compute work (raw_compute.h) places and dispatches one.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "raw_pack.h"

namespace raw {

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
};

}  // namespace raw
