// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_draw.h"
#include "apps/hwtest/bench/compiler_handles.h"
#include "drivers/TegraGM20BMetal/gm20b_gpu_internal.h"
#include "drivers/TegraGM20BMetal/cbuf.h"

namespace gm20b {
struct DeviceHarness {
    static void bind(Push &p, uint32_t group, uint32_t slot, uint64_t address, uint32_t bytes)
    {
        p.inc(CB_SELECTOR_A, {cbufBindBytes(bytes), uint32_t(address >> 32), uint32_t(address)});
        p.m(BIND_GROUP_CB + 32 * group, 1 | (slot << 4));
    }

    static bool vendorBindings(RenderEncoder &e, const Pipeline &pl, bench::compiler::Resources &r,
                               const bench::compiler::Input &in)
    {
        Device &d = *e.dev_;
        uint64_t va = 0;
        constexpr uint32_t size = 0x5800;
        auto *root = reinterpret_cast<uint32_t *>(d.alloc(d.upload_, size, 256, va));
        if (!root || !va || pl.entries.size() != in.kinds.size() || pl.entries.wide())
            return false;
        memset(root, 0, size);
        memcpy(root + 0x5600 / 4, e.cb_->rootPush_, sizeof e.cb_->rootPush_);
        memcpy(root + 0x4600 / 4, &r.tickets.va, 8);
        memcpy(root + 0x2a40 / 4, &r.metadata.va, 8);
        for (uint32_t sample = 0; sample < 4; ++sample) {
            root[4 * sample] = sample & 1;
            root[4 * sample + 1] = sample >> 1;
        }
        // R32 assigns bound samples first. RenderBox follows these with vertex SSBOs, storage images, then pixel SSBOs.
        const auto &words = e.descriptors_.last().words;
        uint32_t vsStorage = 0, fsStorage = 0, uniform = 2;
        const bool renderbox = in.key == "17d6ade58fef2152";
        bench::compiler::HandleWriter handles(root + 0x5700 / 4, in.kinds, renderbox);
        for (uint32_t i = 0; i < pl.entries.size(); ++i) {
            const auto &entry = pl.entries[i];
            const uint32_t *descriptor = words.data() + entry.offset / 4;
            switch (in.kinds[i]) {
            case 6:
                if (!uniformEntry(entry) || !descriptor[2] || (descriptor[0] & 255))
                    return false;
                bind(e.p_, entry.stage == 1 ? 0 : 4, uniform++, (uint64_t(descriptor[1]) << 32) | descriptor[0],
                     descriptor[2]);
                break;
            case 1:
            case 2:
                if (entry.kind != EntryTexture || !descriptor[0])
                    return false;
                handles.sampled(descriptor[0]);
                break;
            case 3:
                if (!descriptor[0] || descriptor[0] >= kPackHeapElements)
                    return false;
                handles.image(descriptor[0]);
                break;
            case 7:
                if (!bufferStorage(entry))
                    return false;
                if (renderbox) {
                    const uint32_t offset = entry.stage == 1 ? 0x5720 + 16 * vsStorage++ : 0x5750 + 16 * fsStorage++;
                    memcpy(root + offset / 4, descriptor, 16);
                } else if (entry.stage == 1 && entry.slot == 0 && vsStorage++ == 0) {
                    memcpy(root + 0x5710 / 4, descriptor, 16);
                } else {
                    return false;
                }
                break;
            default:
                return false;
            }
        }
        if (r.tickets.va)
            e.cb_->use(r.tickets, true);
        if (r.metadata.va)
            e.cb_->use(r.metadata, false);
        bind(e.p_, 0, 0, va, size);
        bind(e.p_, 4, 0, va, size);
        e.p_.m(BINDLESS_TEXTURE, 0);
        return true;
    }

    static bool single(RenderEncoder &e, Pipeline &pl, bench::compiler::Resources &r, const bench::compiler::Input &in,
                       bool vendor, std::string &why)
    {
        const char *reason = nullptr;
        e.called_ = true;
        e.readBits_ |= pl.fetchBits;
        if (!e.prepare(&pl, 3, 0, 0, 3, false, &reason)) {
            why = reason ? reason : "triangle skipped during preparation";
            return false;
        }
        if (e.p_.room() < 192)
            e.reserve();
        if (vendor && !vendorBindings(e, pl, r, in)) {
            why = "R32 descriptor layout disagrees with prepared Metal bindings";
            return false;
        }
        e.cb_->use(r.reports, true);
        counters::emit3d(e.p_, r.reports.va, counters::Point::VertexStart);
        e.wholeDraw(pl, false, 0, 3, 1, 0, 0);
        counters::emit3d(e.p_, r.reports.va + sizeof(counters::Record), counters::Point::FragmentEnd);
        if (vendor) {
            bind(e.p_, 0, 0, e.dev_->root_.va, kRootBytes);
            bind(e.p_, 4, 0, e.dev_->root_.va, kRootBytes);
        }
        e.cb_->draws++;
        e.finishDraw(3, 1);
        return true;
    }
};
}  // namespace gm20b

namespace bench::compiler {
bool draw(gm20b::RenderEncoder &e, gm20b::Pipeline &pipeline, Resources &r, const Input &input, bool vendor,
          std::string &why)
{
    return gm20b::DeviceHarness::single(e, pipeline, r, input, vendor, why);
}
}  // namespace bench::compiler
