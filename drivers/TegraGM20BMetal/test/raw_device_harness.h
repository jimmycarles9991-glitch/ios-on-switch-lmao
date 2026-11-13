// SPDX-License-Identifier: MIT
// raw::Device on the host over the kernel model (raw_kernel_model.h, whose IOKit calls replace the framework's: include
// this in one unit of a test), with a pack of no states. Nothing runs the pushes: the model completes jobs as they are
// submitted, or holds them until a kill, and the test reads what the device submitted.
#pragma once
#include "raw_kernel_model.h"
#include "raw_job_methods.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_gpu.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace raw {

// The device's own state a harness reads (Device declares it a friend in the harness builds, SWITCH_RAW_HARNESS_MARKS)
struct DeviceHarness {
    // What the GPU's root table holds at push word `word` after everything submitted
    static uint32_t rootPush(const Device &d, uint32_t word)
    {
        return d.rootPush_[word];
    }

    // The CPU's view of `bytes` at `va` in the upload ring, null outside it
    static const uint8_t *upload(const Device &d, uint64_t va, uint64_t bytes)
    {
        const Bo &bo = d.upload_.bo;
        return va >= bo.va && va - bo.va + bytes <= bo.bytes ? bo.cpu + (va - bo.va) : nullptr;
    }

    static const HeapTables &tables(const Device &d)
    {
        return d.heapTables_;
    }

    static const Bo &handleTables(const Device &d)
    {
        return d.handleTables_;
    }

    static const Bo &arena(const Device &d, uint32_t arena)
    {
        return d.arenas_[arena].bo;
    }

    static bool arenaWriteCombined(const Device &d, uint32_t arena)
    {
        return d.arenas_[arena].wc;
    }

    static const uint8_t *arenaPages(const Device &d, uint32_t arena)
    {
        return d.arenas_[arena].cpu;
    }

    // The mappings of an arena's BO besides its own: its pitch view (a private heap's), then the views of other kinds
    static size_t arenaViews(const Device &d, uint32_t arena)
    {
        const auto &a = d.arenas_[arena];
        return (a.bufferVa != a.va) + a.views.size();
    }

    // Whether [va, va + bytes) lies in a member of the device's resource set, as a submitting thread sees it now
    static bool setHolds(const Device &d, uint64_t va, uint64_t bytes = 1)
    {
        SetView v;
        d.set_.view(v);
        return v.holds(va, bytes);
    }

    // An m2v view's hold on an arena (raw_alias.cpp's lease), which the harness's own connection never makes
    static void holdArena(Device &d, uint32_t arena, bool hold)
    {
        auto &leases = d.arenas_[arena].leases;
        leases = hold ? leases + 1 : leases - 1;
    }

    static std::array<uint32_t, 8> header(const Device &d, uint32_t tic)
    {
        std::array<uint32_t, 8> w;
        memcpy(w.data(), d.tic_.cpu + 32 * tic, sizeof w);
        return w;
    }

    // What the root table holds at push word `word` after the command buffer's work encoded so far
    static uint32_t rootPush(const CommandBuffer &cb, uint32_t word)
    {
        return cb.rootPush_[word];
    }

    // Where the encoder's last upload of stage `k`'s argument table lies (raw_spill_table.h), 0: none
    static uint64_t spillTable(const RenderEncoder &e, uint32_t k)
    {
        return e.spillUploads_[k].va();
    }

    // The set-0 words a compute encoder made for its last dispatch, null: none
    static const uint32_t *kernelSet(const ComputeEncoder &e)
    {
        return e.descriptorWords_.empty() ? nullptr : e.descriptorWords_.data();
    }
};

}  // namespace raw

namespace harness {

// A pack of this build's version holding no states (raw_pack.h), in the test's temporary directory
inline std::string emptyPack()
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/empty-pack.bin";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

class DeviceTest : public testing::Test {
protected:
    raw::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    // One command buffer of `copies` copies of `bytes` from `from` to `to`: its seqno, 0 when it was not submitted
    uint32_t copy(const raw::Buffer &from, const raw::Buffer &to, uint32_t bytes, uint32_t copies = 1)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0;
        for (uint32_t i = 0; i < copies; i++)
            e->copyBytes(from.va, to.va, bytes);
        e->end();
        uint32_t seqno = 0;
        return cb->commit(seqno) ? seqno : 0;
    }

    kernel_model::Slot *liveSlot()
    {
        for (kernel_model::Slot &s : kernel_model::model.slots)
            if (s.uid && !s.foreign)
                return &s;
        return nullptr;
    }

    uint32_t liveChannel()
    {
        kernel_model::Slot *s = liveSlot();
        return s ? uint32_t(s - kernel_model::model.slots) + 1 : 0;
    }

    bool freed(uint64_t handle) const
    {
        const auto &f = kernel_model::model.freed;
        return std::find(f.begin(), f.end(), handle) != f.end();
    }
};

}  // namespace harness
