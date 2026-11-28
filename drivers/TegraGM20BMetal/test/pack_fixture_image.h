// SPDX-License-Identifier: MIT
// The pre-built segment fixture (pack_v34_fixture.py) read whole, and the kernel model's pack image set up from it
#pragma once
#include "kernel_model.h"
#include "../pack.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace pack_fixture {

constexpr const char *kPath = "drivers/TegraGM20BMetal/test/pack_v34_fixture.bin";

inline std::vector<uint8_t> readFileBytes(const char *path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.is_open()) << path;
    const std::streamsize size = in.tellg();
    if (size <= 0)
        return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    return bytes;
}

inline void configurePackImageFromBytes(const std::vector<uint8_t> &file, bool enabled)
{
    kernel_model::model = {};
    kernel_model::model.vaEnd = uint64_t(1) << 39;
    if (!enabled)
        return;
    gm20b::PackHeader h = {};
    std::memcpy(&h, file.data(), sizeof h);
    kernel_model::model.packImage.enabled = true;
    kernel_model::model.packImage.status = TegraGM20BPackStatusReady;
    kernel_model::model.packImage.gpuVa = TGM_PACK_WINDOW_VA;
    kernel_model::model.packImage.tier0HeapBytes = h.tier0[gm20b::PackHeap];
    kernel_model::model.packImage.heapBytes = h.section[gm20b::PackHeap].bytes;
    kernel_model::model.packImage.stack = h.stack;
    kernel_model::model.packImage.hostData = file.data() + h.section[gm20b::PackHeap].offset;
}

}  // namespace pack_fixture
