// SPDX-License-Identifier: MIT
// Where the probe's bind-offset run (probe.mm PROBE_BIND_OFFSET, runtime/RawProbe.mm) binds each slot's payload, which names
// each buffer slot's SlotBindOffset residual push word by its value (pack/mkpack.py bind_offset and storage_code read it
// the same way). m2v's residual is the bind offset less its base alignment's multiple below it: 256 for a uniform buffer, 16
// for a storage buffer (metal2vulkan bind/instantiate.rs descriptor_range). So a uniform slot's word holds the offset itself
// (16 or more, each slot's own) and a storage slot's the offset's low four bits (1 to 15): a vertex slot's 4, 8 or 12, as
// a fixed-function vertex buffer's offset stays a multiple of Metal's largest vertex alignment (an odd one would make m2v
// pull the vertices: another program), a fragment slot's one of the twelve that are not multiples of 4. Two storage slots of
// one state with the same low bits name no word.
#pragma once
#include <cstdint>

namespace raw {

constexpr uint8_t kProbeFragmentCodes[12] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15};

// stage 1 vertex, 2 fragment; slot below 31: from 20 to 239
constexpr uint32_t probeBindOffset(uint32_t stage, uint32_t slot)
{
    return stage == 2 ? 16 * (12 + slot / 12) + kProbeFragmentCodes[slot % 12]
                      : 16 * (1 + slot / 3) + 4 * (1 + slot % 3);
}

// The residual a storage buffer bound at the slot's probe offset reads, which its push word holds in the run
constexpr uint32_t probeStorageCode(uint32_t stage, uint32_t slot)
{
    return probeBindOffset(stage, slot) & 15;
}

// A kernel's buffer slot's residual in probecompute.mm's bind-offset dispatch (pack/kernels.py residual_code): a kernel's
// buffers are storage buffers, and slots 15 apart share one, so a kernel binding both names no word for either
constexpr uint32_t probeComputeCode(uint32_t slot)
{
    return 1 + slot % 15;
}

}  // namespace raw
