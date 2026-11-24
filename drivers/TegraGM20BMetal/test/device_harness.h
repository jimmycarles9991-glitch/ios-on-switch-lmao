// SPDX-License-Identifier: MIT
// gm20b::Device on the host over the kernel model (kernel_model.h, whose IOKit calls replace the framework's: include
// this in one unit of a test), with a pack of no states. Nothing runs the pushes: the model completes jobs as they are
// submitted, or holds them until a kill, and the test reads what the device submitted.
#pragma once
#include "kernel_model.h"
#include "job_methods.h"
#include "../compute.h"
#include "../gpu.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gm20b {

// The device's own state a harness reads (Device declares it a friend in the harness builds, SWITCH_METAL_HARNESS_MARKS)
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

    static bool lockFree(Device &d)
    {
        std::unique_lock<gm20b::Mutex> l(d.lock_, std::try_to_lock);
        return l.owns_lock();
    }

    // A lease's hold on an arena (alias.cpp: a CPU access, or a placement made aliasable)
    static void holdArena(Device &d, uint32_t arena, bool hold)
    {
        auto &leases = d.arenas_[arena].leases;
        leases = hold ? leases + 1 : leases - 1;
    }

    static const Bo &zeroPage(const Device &d)
    {
        return d.zero_;
    }

    // An extended-range texture's stand-in of its shape (Device::standInAsk), whatever states the pack holds
    static bool standIn(Device &d, const Texture &t, Texture &s)
    {
        const TextureAsk ask = Device::standInAsk(t);
        return ask.kind == TextureAsk::Plain ? d.createTexture(t.width, t.height, kXrStandInFormat, s, false, true)
                                             : d.createLaidOut(ask, s, nullptr);
    }

    static std::array<uint32_t, 8> header(const Device &d, uint32_t tic)
    {
        std::array<uint32_t, 8> w;
        memcpy(w.data(), d.tic_.cpu + 32 * tic, sizeof w);
        return w;
    }

    static std::array<uint32_t, 8> tscHeader(const Device &d, uint32_t tsc)
    {
        std::array<uint32_t, 8> w;
        memcpy(w.data(), d.tsc_.cpu + 32 * tsc, sizeof w);
        return w;
    }

    // The header constexpr sampler `index` samples a texture of address map `map` with (0: none)
    static uint32_t constexprTsc(const Device &d, uint32_t index, uint8_t map)
    {
        return map ? d.constexprMapped_[index][map] : d.constexprTsc_[index];
    }

    // Where the encoder's last draw's set 0 lies
    static uint64_t preparedSet(const RenderEncoder &e)
    {
        return e.preparedSet_;
    }

    // The sizes of the compressed buffers made and not taken yet
    static std::vector<uint64_t> compReadyBytes(Device &d)
    {
        std::lock_guard<gm20b::Mutex> l(d.lock_);
        std::vector<uint64_t> out;
        for (const CompBo &b : d.comp_.ready)
            out.push_back(b.bo.bytes);
        return out;
    }

    // The GPU address of texture header `tic`
    static uint64_t ticVa(const Device &d, uint32_t tic)
    {
        return d.tic_.va + 32ull * tic;
    }

    // A full clear of a compressed target to the ZBC value `e` (Device::clearZbc's ask); true where the table holds it
    static bool zbcHeld(Device &d, const TegraGM20BAbiZbc &e)
    {
        return d.zbcHeld(e);
    }

    // Compressed buffers made off the render thread that no texture or backing took yet
    static size_t compReady(Device &d)
    {
        std::lock_guard<gm20b::Mutex> l(d.lock_);
        return d.comp_.ready.size();
    }

    // Plain private textures wanting compressed memory (private_comp.h)
    static size_t wanting(Device &d)
    {
        std::lock_guard<gm20b::Mutex> l(d.lock_);
        return d.privateWants_.size();
    }

    // Moved textures' plain memory not freed yet
    static size_t movedPlain(const Device &d)
    {
        return d.movedPlain_.size();
    }

    // The clears of the pass `e` ended: target, x0, y0, x1, y1
    static std::vector<std::array<uint32_t, 5>> clears(const RenderEncoder &e)
    {
        std::vector<std::array<uint32_t, 5>> out;
        for (const RenderEncoder::Clear &c : e.initialClears_)
            out.push_back({c.target, c.x0, c.y0, c.x1, c.y1});
        return out;
    }

    // What the root table holds at push word `word` after the command buffer's work encoded so far
    static uint32_t rootPush(const CommandBuffer &cb, uint32_t word)
    {
        return cb.rootPush_[word];
    }

    // The set members the command buffer named a use of so far (CommandBuffer::useMember)
    static size_t memberUses(const CommandBuffer &cb)
    {
        return cb.memberUses_.size();
    }

    // The state segments the command buffer bound so far (each bind a call of the state's words, not a skip of them)
    static uint32_t segmentBinds(const CommandBuffer &cb)
    {
        return cb.segmentBinds_;
    }

    // Where the encoder's last upload of stage `k`'s argument table lies (spill_table.h), 0: none
    static uint64_t spillTable(const RenderEncoder &e, uint32_t k)
    {
        return e.spillUploads_[k].va();
    }

    // The set-0 words a compute encoder made for its last dispatch, null: none
    static const uint32_t *kernelSet(const ComputeEncoder &e)
    {
        return e.descriptorWords_.empty() ? nullptr : e.descriptorWords_.data();
    }

    // The sampler header the device wrote at index `tsc`
    static const uint32_t *samplerHeader(const Device &d, uint32_t tsc)
    {
        return reinterpret_cast<const uint32_t *>(d.tsc_.cpu + 32 * tsc);
    }
};

}  // namespace gm20b

namespace harness {

// A pack of this build's version holding no states (pack.h), in the test's temporary directory
inline std::string emptyPack()
{
    std::vector<uint8_t> file(0x400, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
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
    gm20b::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }

    // The device and its threads go with this base, after a derived fixture's members (C++ destroys those first), and
    // GoogleTest runs TearDown before any of it: from here no hook a test put in the model runs into a fixture member.
    // A fixture that overrides TearDown calls this one last.
    void TearDown() override
    {
        kernel_model::clearHooks();
    }

    // One command buffer of `copies` copies of `bytes` from `from` to `to`: its seqno, 0 when it was not submitted
    uint32_t copy(const gm20b::Buffer &from, const gm20b::Buffer &to, uint32_t bytes, uint32_t copies = 1)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0;
        cb->use(from, false);
        cb->use(to, true, false);
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
