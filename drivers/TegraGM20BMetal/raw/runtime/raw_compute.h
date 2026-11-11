// SPDX-License-Identifier: MIT
// The raw GM20B path's compute work: Metal kernels as MAXWELL_COMPUTE_B launches on the raw channel, beside its 3D and copy
// work. The pack's context init binds the class on subchannel 1 with the raw heap as its program region and the raw texture
// and sampler pools (raw_pack.h PackInit), so a dispatch is a QMD in the upload ring and SEND_PCAS: no compute state lives in
// the channel between dispatches.
//
// A kernel is NVK's work for one Metal kernel at one key (raw_compute_key.h: m2v compiles the threadgroup shape and memory
// lengths into the program, and picks its variant by the textures and samplers bound): NAK's program, the QMD NVK filled for
// it (nak_fill_qmd), which of its constant buffers are NVK's root table, set 0 and shader data, and where set 0 takes each
// Metal buffer, texture and sampler (raw_pack.h PackKernel). A dispatch writes those as NVK's nvk_cmd_upload_qmd does: its
// own root table (the cs group count, the set's address, the push words with m2v's DispatchRegion field) and set 0 in the
// upload ring, and a QMD of the template with the program's heap offset, the grid and the constant buffers' addresses.
//
// Order. A compute encoder is ordered after the work encoded before it and before the work after it on the GPU, as Metal orders
// a serial encoder: a wait for idle where it starts (the render and copy work before it has written), between its dispatches
// a shader-data flush that waits for the one before, and where it ends a wait for idle with the texture and constant caches
// invalidated (what the 3D and copy engines read next was written through L2).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "raw_compute_record.h"
#include "raw_gpu.h"

namespace raw {

constexpr uint32_t kQmdWords = 64;

// A kernel ready to dispatch: its record and where its program lies in the heap
struct ComputeState {
    const PackKernel *rec = nullptr;
    uint8_t cbufType[8] = {};
    uint32_t program = 0;  // QMD PROGRAM_OFFSET: from the program region (the heap's start)
    uint64_t data = 0;     // its shader data's address, 0: none
    PackKernel placed =
        {};  // a runtime record's, its samplers numbered among the device's (placeCompute): rec points here
};

// The pack's kernel of key `keyHash` (computeKeyHash), its program placed at its first use; null: not in the pack, or no
// room in the heap
const ComputeState *computeState(Device &dev, uint64_t keyHash);
// A record of no pack, its program placed at its first use (null: no room)
const ComputeState *placeCompute(Device &dev, const ComputeRecord &r);

class ComputeEncoder {
public:
    ComputeEncoder(CommandBuffer &cb, Device &dev);

    void setState(const ComputeState *k)
    {
        state_ = k;
    }

    // MTLDispatchTypeSerial: each dispatch after the encoder's first waits for the one before, its writes visible
    void setSerial(bool serial)
    {
        serial_ = serial;
    }

    void setBytes(uint32_t slot, const void *bytes, uint32_t length);
    void setBytesAt(uint32_t slot, uint64_t va, uint32_t length);  // bytes already where the GPU reads them
    void setBuffer(uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(uint32_t slot, uint64_t offset);
    void setTexture(uint32_t slot, const Texture *t);
    void setSampler(uint32_t slot, const Sampler *s);

    // dispatchThreadgroups: `groups` threadgroups of `threads` threads, the shape the kernel's record was made at. False:
    // refused (*why), nothing encoded
    bool dispatch(const uint32_t groups[3], const uint32_t threads[3], const char **why);
    // memoryBarrier...: the dispatches before have written, and their writes are visible to the ones after
    void barrier();
    void end();

private:
    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
    };

    CommandBuffer &cb_;
    Device &dev_;
    Push p_;
    uint32_t *start_ = nullptr;
    const ComputeState *state_ = nullptr;
    Bound bufs_[31];
    const Texture *textures_[31] = {};
    const Sampler *samplers_[16] = {};
    bool serial_ = false, dispatched_ = false;
    void room(uint32_t words);
};

}  // namespace raw
