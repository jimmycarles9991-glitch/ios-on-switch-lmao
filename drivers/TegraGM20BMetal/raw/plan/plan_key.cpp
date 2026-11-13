// SPDX-License-Identifier: MIT
#include "plan_key.h"
#include <unordered_map>
#include <vector>

namespace raw::plan {

namespace {

class Hash {
public:
    void add(uint64_t v)
    {
        h_ = mix(h_ ^ (v + 0x9e3779b97f4a7c15ull + (h_ << 6) + (h_ >> 2)));
    }

    void add(const SubRange &r)
    {
        add(uint64_t(r.level) | uint64_t(r.levels) << 16 | uint64_t(r.layer) << 32 | uint64_t(r.layers) << 48);
    }

    uint64_t value() const
    {
        return h_;
    }

private:
    static uint64_t mix(uint64_t z)
    {
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    uint64_t h_ = 0x5357495443480000ull;
};

void resources(Hash &h, const Recording &r)
{
    struct Memory {
        uint32_t canonical;
        std::vector<uint32_t> slots;
    };

    std::unordered_map<uint32_t, Memory> sharing;
    for (uint32_t i = 0; i < r.resources.size(); ++i) {
        const ResourceFacts &f = r.resources[i];
        std::vector<uint32_t> *earlier = nullptr;
        uint32_t memory = kNone;
        if (f.memory != kNone) {
            Memory &m = sharing.try_emplace(f.memory, Memory{uint32_t(sharing.size()), {}}).first->second;
            earlier = &m.slots;
            memory = m.canonical;
        }
        h.add(uint64_t(f.kind) | uint64_t(f.layout) << 8 | uint64_t(f.samples) << 16 | uint64_t(f.format) << 32);
        h.add(uint64_t(f.flags) | uint64_t(memory) << 32);
        h.add(uint64_t(f.levels) | uint64_t(f.layers) << 16 | uint64_t(f.width) << 32);
        h.add(uint64_t(f.height) | uint64_t(f.shadow) << 32);
        h.add(uint64_t(f.standIn) | uint64_t(f.luma) << 32);
        h.add(f.chroma);
        if (!earlier)
            continue;
        for (uint32_t j : *earlier) {
            const ResourceFacts &o = r.resources[j];
            const bool meet = !f.bytes || !o.bytes || (f.offset < o.offset + o.bytes && o.offset < f.offset + f.bytes);
            h.add(uint64_t(j) << 1 | meet);
        }
        earlier->push_back(i);
    }
}

void programs(Hash &h, const Recording &r)
{
    for (const ProgramFacts &p : r.programs) {
        h.add(p.key);
        for (uint16_t f : p.colorFormats)
            h.add(f);
        h.add(uint64_t(p.depthFormat) | uint64_t(p.stencilFormat) << 16 | uint64_t(p.samples) << 32 |
              uint64_t(p.forms) << 40);
        h.add(uint64_t(p.writeMask) | uint64_t(p.readMask) << 8 | uint64_t(p.fetchMask) << 16 |
              uint64_t(p.storageMask) << 24 | uint64_t(p.storageWriteMask) << 32 | uint64_t(p.primitiveId) << 40 |
              uint64_t(p.discards) << 41 | uint64_t(p.compute) << 42);
        for (uint8_t rung : p.fetchRung)
            h.add(rung);
    }
}

void attachment(Hash &h, const Attachment &a)
{
    h.add(uint64_t(a.texture) | uint64_t(a.level) << 32 | uint64_t(a.slice) << 48);
    h.add(uint64_t(a.load) | uint64_t(a.store) << 8 | uint64_t(a.depthPlane) << 16 | uint64_t(a.resolve) << 32);
    h.add(uint64_t(a.resolveLevel) | uint64_t(a.resolveSlice) << 16);
}

void passes(Hash &h, const Recording &r)
{
    for (const PassDesc &p : r.passes) {
        for (const Attachment &a : p.color)
            attachment(h, a);
        attachment(h, p.depth);
        attachment(h, p.stencil);
        h.add(uint64_t(p.width) | uint64_t(p.height) << 32);
        h.add(uint64_t(p.layers) | uint64_t(p.rateMap) << 16 | uint64_t(p.samplePositions) << 17);
    }
}

void bindings(Hash &h, const Recording &r, const Span &s)
{
    h.add(s.count);
    for (uint32_t i = 0; i < s.count; ++i) {
        const Binding &b = r.bindings[s.first + i];
        h.add(uint64_t(b.resource) | uint64_t(b.stage) << 32 | uint64_t(b.index) << 40 | uint64_t(b.path) << 48 |
              uint64_t(b.write) << 56);
    }
}

void op(Hash &h, const Recording &r, const Op &o)
{
    h.add(uint64_t(o.kind));
    switch (o.kind) {
    case OpKind::Draw: {
        const Draw &d = r.draws[o.payload];
        const bool misaligned = d.indexSize && d.indexOffset % d.indexSize;
        h.add(uint64_t(d.state) | uint64_t(d.primitive) << 32 | uint64_t(d.indexSize) << 40 |
              uint64_t(d.indirect) << 48 | uint64_t(misaligned) << 49);
        h.add(uint64_t(d.indices) | uint64_t(d.arguments) << 32);
        break;
    }
    case OpKind::Dispatch: {
        const Dispatch &d = r.dispatches[o.payload];
        bool ragged = false;
        for (uint32_t i = 0; i < 3; ++i)
            ragged |= d.threads[i] && d.grid[i] % d.threads[i];
        h.add(uint64_t(d.program) | uint64_t(d.perThread) << 32 | uint64_t(ragged) << 33);
        h.add(d.imageShadowSlots);
        bindings(h, r, d.bindings);
        break;
    }
    case OpKind::Transfer: {
        const Transfer &t = r.transfers[o.payload];
        const auto past = [&](uint32_t slot, uint64_t offset) {
            return slot != kNone && r.resources[slot].bytes && t.bytes && offset + t.bytes > r.resources[slot].bytes;
        };
        const bool outside = past(t.source, t.sourceOffset) || past(t.destination, t.destinationOffset);
        h.add(uint64_t(t.kind) | uint64_t(t.aspect) << 8 | uint64_t(outside) << 16 | uint64_t(t.source) << 32);
        h.add(t.destination);
        h.add(t.sourceRange);
        h.add(t.destinationRange);
        break;
    }
    case OpKind::Control: {
        const Control &c = r.controls[o.payload];
        h.add(uint64_t(c.kind) | uint64_t(c.scope) << 8 | uint64_t(c.after) << 16 | uint64_t(c.before) << 24 |
              uint64_t(c.resource) << 32);
        h.add(c.write);
        break;
    }
    }
}

}  // namespace

uint64_t structuralKey(const Recording &r, const Caps &caps)
{
    Hash h;
    h.add(uint64_t(caps.proven) | uint64_t(caps.tiledCache) << 32);
    resources(h, r);
    programs(h, r);
    passes(h, r);
    for (const DrawState &s : r.states) {
        h.add(uint64_t(s.program) | uint64_t(s.depthWrite) << 32 | uint64_t(s.stencilWrite) << 33 |
              uint64_t(s.depthTest) << 34 | uint64_t(s.visibility) << 35);
        bindings(h, r, s.bindings);
    }
    for (const Encoder &e : r.encoders) {
        h.add(uint64_t(e.kind) | uint64_t(e.serial) << 8 | uint64_t(e.pass) << 32);
        h.add(e.ops.count);
        for (uint32_t i = 0; i < e.ops.count; ++i)
            op(h, r, r.ops[e.ops.first + i]);
    }
    for (const Event &e : r.events)
        h.add(uint64_t(e.event) | uint64_t(e.signal) << 32 | uint64_t(e.shared) << 33);
    for (const Boundary &b : r.boundaries)
        h.add(uint64_t(b.kind) | uint64_t(b.afterEncoder) << 8 | uint64_t(b.payload) << 32);
    return h.value();
}

void nodeKeys(WorkGraph &g, const Recording &r)
{
    for (Node &n : g.nodes) {
        Hash h;
        h.add(uint64_t(n.kind) | uint64_t(n.engine) << 8 | uint64_t(n.rule) << 16 | uint64_t(n.continues) << 24 |
              uint64_t(n.before) << 32);
        h.add(uint64_t(n.pass) | uint64_t(n.from) << 32);
        h.add(n.to);
        h.add(n.fromRange);
        h.add(n.toRange);
        for (uint32_t i = 0; i < n.uses.count; ++i) {
            const Use &u = g.uses[n.uses.first + i];
            h.add(uint64_t(u.slot) | uint64_t(u.path) << 32 | uint64_t(u.stage) << 40 | uint64_t(u.write) << 48);
            h.add(u.range);
        }
        if (n.pass != kNone) {
            const PassDesc &p = r.passes[n.pass];
            for (const Attachment &a : p.color)
                attachment(h, a);
            attachment(h, p.depth);
            attachment(h, p.stencil);
        }
        for (uint32_t i = 0; i < n.runs.count; ++i) {
            const DrawRun &run = g.runs[n.runs.first + i];
            h.add(run.program != kNone ? r.programs[run.program].key : uint64_t(run.own));
            h.add(uint64_t(run.variant) | uint64_t(run.before) << 8 | uint64_t(run.draws.count) << 32);
            for (Rung rung : run.rung)
                h.add(uint64_t(rung));
            if (run.own != OwnState::None)
                continue;
            bindings(h, r, r.states[run.state].bindings);
            for (uint32_t d = 0; d < run.draws.count; ++d) {
                const Draw &draw = r.draws[run.draws.first + d];
                h.add(uint64_t(draw.primitive) | uint64_t(draw.indexSize) << 8 | uint64_t(draw.indirect) << 16 |
                      uint64_t(draw.indices) << 32);
                h.add(draw.arguments);
            }
        }
        n.key = h.value();
    }
}

}  // namespace raw::plan
