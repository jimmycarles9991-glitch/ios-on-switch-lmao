// SPDX-License-Identifier: MIT
#pragma once
#include "../runtime/raw_gpu.h"

extern "C" bool mock_iokit_seed_sequence(uint32_t channel, uint32_t sequence);
extern "C" void mock_iokit_pause(uint32_t channel, bool pause);

namespace raw {
struct DeviceHarness {
    template <class Expect> static void computeRetry(Device &device, Expect expect);
    template <class Expect> static void ranges(Device &device, Expect expect);

    template <class Expect> static void wrap(Device &device, Pipeline &pipeline, Expect expect)
    {
        expect(device.wait(device.submitted()), "pre-wrap work physically retires");
        expect(mock_iokit_seed_sequence(device.channel_, UINT32_MAX), "idle channel accepts deterministic wrap seed");
        device.lastSeqno_ = UINT32_MAX;
        device.push_.inflight.clear();
        device.upload_.inflight.clear();
        {
            std::lock_guard<std::mutex> lock(device.lock_);
            for (auto *backing : device.backings_)
                backing->submitted = false;
        }
        Texture transient;
        if (!device.createMemoryless(17, 13, 80, transient))
            std::abort();
        PassDesc pass;
        pass.count = 1;
        pass.color[0].texture = &transient;
        pass.color[0].load = ColorAttachment::Clear;
        pass.color[0].store = false;
        auto *cb = device.begin();
        auto *encoder = cb ? cb->render(pass) : nullptr;
        if (!encoder)
            std::abort();
        encoder->setPipeline(&pipeline);
        const char *why = nullptr;
        if (!encoder->draw(3, 0, 3, 1, 0, &why))
            std::abort();
        encoder->end();
        uint64_t uploadVa;
        *device.alloc(device.upload_, 64, 64, uploadVa) = 42;
        mock_iokit_pause(device.channel_, true);
        uint32_t sequence = UINT32_MAX;
        expect(cb->commit(sequence) && sequence == 0, "actual encoder publishes wrapped sequence zero");
        expect(!device.done(sequence), "zero does not retire while physical completion is pending");
        const auto hasZero = [](const Device::Ring &ring) {
            return std::any_of(ring.inflight.begin(), ring.inflight.end(), [](const auto &span) {
                return span.submitted && span.seqno == 0;
            });
        };
        expect(hasZero(device.push_) && hasZero(device.upload_), "both rings retain submitted zero spans");
        auto *abandoned = device.begin();
        if (!abandoned)
            std::abort();
        abandoned->abandon();
        expect(hasZero(device.push_) && hasZero(device.upload_), "abandon preserves the earlier zero submission");
        Device::Backing *held = nullptr;
        {
            std::lock_guard<std::mutex> lock(device.lock_);
            for (auto *backing : device.backings_)
                if (backing->texture.width == 17 && backing->texture.height == 13 && backing->submitted &&
                    backing->lastUse == 0)
                    held = backing;
            expect(held != nullptr, "transient backing records a valid zero last use");
            if (held)
                held->retired = true;
            device.trimBackings();
            expect(std::find(device.backings_.begin(), device.backings_.end(), held) != device.backings_.end(),
                   "idle trimming retains the backing before zero physically retires");
        }
        mock_iokit_pause(device.channel_, false);
        expect(device.wait(sequence) && device.done(sequence), "zero completes through the ordinary fence wait");
        {
            std::lock_guard<std::mutex> lock(device.lock_);
            device.trimBackings();
            expect(std::find(device.backings_.begin(), device.backings_.end(), held) == device.backings_.end(),
                   "retired transient backing releases after zero completion");
        }
        for (auto *ring : {&device.push_, &device.upload_}) {
            if (ring->inflight.empty())
                std::abort();
            ring->head = ring->since = ring->clear = ring->inflight.front().begin;
            uint64_t address;
            const auto before = ring->bo.handle;
            device.alloc(*ring, 4, 4, address);
            expect(!hasZero(*ring) && ring->bo.handle == before,
                   "completed zero ring space reuses the allocation without growth");
            ring->head = ring->since;
        }

        struct FenceState {
            TegraGM20BAbiFence physical;
            TegraGM20BAbiFenceSemantic semantic;
            TegraGM20BAbiFenceIdentity identity;
        } state{{UINT32_MAX, 0, 0, 0}, {2, 0, 0, TegraGM20BFenceSemanticPending}, {(uint64_t(5) << 48) | 19}};

        static_assert(offsetof(FenceState, semantic) == sizeof(TegraGM20BAbiFence));
        const auto *originalFence = device.fence_;
        auto &flags = const_cast<TegraGM20BAbiInfo &>(device.k_.info()).flags;
        const auto originalFlags = flags;
        device.fence_ = &state.physical.completed;
        expect(!device.done(0), "zero semantic state cannot bypass physical retirement");
        state.physical.completed = 0;
        expect(!device.done(0), "physically complete zero remains pending on semantic authority");
        state.semantic.flags = TegraGM20BFenceSemanticFailed;
        expect(device.done(0) && device.lost(0), "failed zero semantic authority is terminal and unsuccessful");
        state.semantic.flags = 0;
        expect(device.done(0) && !device.lost(0), "successful zero semantic authority completes normally");
        Buffer cached, wc;
        if (!device.createBuffer(256, cached, false) || !device.createBuffer(64, wc, true))
            std::abort();
        Buffer alias = cached;
        alias.cpu += 64;
        alias.va += 64;
        alias.length = 128;
        Buffer hidden = cached;
        hidden.hostVisible = false;
        const auto snapshot =
            device.captureCpuCompletion(0, device.channelGeneration(), {&alias, &cached, &wc, &hidden});
        expect(snapshot.valid && snapshot.channelUid == state.identity.channel_uid &&
                   snapshot.channel == device.channel_ && snapshot.seqno == 0,
               "accepted cached snapshot retains full kernel identity and valid zero prefix");
        expect(snapshot.ranges.size() == 1 && snapshot.ranges[0].bytes == cached.length,
               "cached buffer aliases coalesce while WC and private inputs are excluded");
        const auto subrange = device.captureCpuCompletion(0, device.channelGeneration(), {&alias});
        expect(subrange.valid && subrange.ranges.size() == 1 &&
                   subrange.ranges[0].handle == snapshot.ranges[0].handle &&
                   subrange.ranges[0].offset == snapshot.ranges[0].offset + 64 && subrange.ranges[0].bytes == 128,
               "cached alias snapshot retains exact BO offset and extent");
        state.identity.channel_uid += uint64_t(1) << 40;
        expect(snapshot.channelUid != state.identity.channel_uid,
               "queued snapshot cannot observe a later channel identity");
        expect(!device.captureCpuCompletion(0, device.channelGeneration() + 1, {&cached}).valid,
               "generation mismatch cannot acknowledge a replacement channel");
        Buffer oversized = cached;
        oversized.length = UINT64_MAX;
        expect(!device.captureCpuCompletion(0, device.channelGeneration(), {&oversized}).valid,
               "cached range outside its BO cannot reach completion maintenance");
        expect(device.captureCpuCompletion(0, device.channelGeneration(), {&wc, &hidden}).ranges.empty(),
               "GPU-only buffers need no CPU completion ranges");
        device.fence_ = originalFence;
        flags = originalFlags;
        device.destroyBuffer(cached);
        device.destroyBuffer(wc);
        device.destroyTexture(transient);
    }
};
}  // namespace raw
