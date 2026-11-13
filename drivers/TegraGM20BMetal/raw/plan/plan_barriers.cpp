// SPDX-License-Identifier: MIT
#include "plan_barriers.h"
#include "../runtime/raw_attachment_order.h"
#include <algorithm>
#include <array>
#include <vector>

namespace raw::plan {

namespace {

constexpr uint32_t kBits = 13;
constexpr Barriers kInvalidates =
    InvalidateShaderData | InvalidateConstants | InvalidateInstructions | InvalidateTextureData | SetReference;

struct Region {
    uint32_t memory = kNone;
    uint64_t offset = 0, bytes = 0;
    uint32_t slot = kNone;
    SubRange range;

    bool overlaps(const Region &o) const
    {
        if (memory != o.memory)
            return false;
        const bool bytesMeet = !bytes || !o.bytes || (offset < o.offset + o.bytes && o.offset < offset + bytes);
        return bytesMeet && (slot != o.slot || range.overlaps(o.range));
    }

    bool same(const Region &o) const
    {
        return memory == o.memory && offset == o.offset && bytes == o.bytes && slot == o.slot &&
               range.level == o.range.level && range.levels == o.range.levels && range.layer == o.range.layer &&
               range.layers == o.range.layers;
    }
};

struct Record {
    Region region;
    Access access;
    uint32_t node;
};

Region regionOf(const Recording &r, const Use &u)
{
    const ResourceFacts &f = r.resources[u.slot];
    Region g;
    g.memory = f.memory != kNone ? f.memory : (0x80000000u | u.slot);
    g.offset = f.offset;
    g.bytes = f.bytes;
    g.slot = u.slot;
    g.range = u.range;
    return g;
}

int bitIndex(Barrier b)
{
    return __builtin_ctz(uint32_t(b));
}

class Placer {
public:
    Placer(WorkGraph &g, const Recording &r, const Caps &caps) : g_(g), r_(r), caps_(caps)
    {
        last_.fill(-1);
    }

    void run(const JobContext &job);

private:
    WorkGraph &g_;
    const Recording &r_;
    const Caps &caps_;
    std::vector<Record> writes_, reads_;
    std::vector<uint32_t> drains_;
    std::array<int64_t, kBits> last_{};

    int64_t last(Barrier b) const
    {
        return last_[bitIndex(b)];
    }

    int64_t firstDrainAfter(uint32_t node) const
    {
        const auto at = std::upper_bound(drains_.begin(), drains_.end(), node);
        return at == drains_.end() ? -1 : int64_t(*at);
    }

    Barriers unsatisfied(Barriers b, uint32_t node) const;
    void note(Barriers b, uint32_t node, bool drained);
    Barriers crossNode(uint32_t head, uint32_t end, std::vector<uint32_t> &edges) const;
    void record(uint32_t head, uint32_t end);
    void inPass(uint32_t head, uint32_t end);
    Barriers flushOutstanding() const;
    Barriers entry(const JobContext &job) const;
};

Barriers Placer::unsatisfied(Barriers b, uint32_t node) const
{
    Barriers out = 0;
    const int64_t drain = firstDrainAfter(node);
    bool drained = drain >= 0;
    if ((b & TiledCacheFlush) && last(TiledCacheFlush) <= int64_t(node)) {
        out |= TiledCacheFlush;
        drained = false;
    }
    if ((b & TiledCacheFlush) && drained && drains_.back() < last(TiledCacheFlush))
        drained = false;
    if ((b & FlushShaderData) && last(FlushShaderData) <= int64_t(node)) {
        out |= FlushShaderData;
        drained = false;
    }
    if ((b & WaitForIdle) && !drained)
        out |= WaitForIdle;
    for (Barrier x :
         {InvalidateShaderData, InvalidateConstants, InvalidateInstructions, InvalidateTextureData, SetReference})
        if ((b & x) && !(drained && last(x) >= drain))
            out |= x;
    if ((b & PixelShaderBarrier) && last(PixelShaderBarrier) <= int64_t(node) &&
        !(drained && last(InvalidateTextureData) >= drain))
        out |= PixelShaderBarrier;
    if ((b & CopyNonPipelined) && last(CopyNonPipelined) <= int64_t(node) && !drained)
        out |= CopyNonPipelined;
    return out;
}

void Placer::note(Barriers b, uint32_t node, bool drained)
{
    for (uint32_t i = 0; i < kBits; ++i)
        if (b >> i & 1)
            last_[i] = node;
    if (drained && (drains_.empty() || drains_.back() != node))
        drains_.push_back(node);
}

Barriers Placer::crossNode(uint32_t head, uint32_t end, std::vector<uint32_t> &edges) const
{
    Barriers need = 0;
    for (uint32_t n = head; n < end; ++n) {
        const Node &node = g_.nodes[n];
        for (uint32_t i = 0; i < node.uses.count; ++i) {
            const Use &u = g_.uses[node.uses.first + i];
            const Region region = regionOf(r_, u);
            const Access later{u.path, u.write, u.stage, node.engine};
            const auto against = [&](const Record &earlier) {
                if (earlier.node >= head || !earlier.region.overlaps(region))
                    return;
                const Requirement req = hazard(earlier.access, later, Scope::AcrossNodes, caps_.tiledCache);
                if (req.none() && !req.clauses)
                    return;
                need |= unsatisfied(required(req, caps_), earlier.node);
                if (std::find(edges.begin(), edges.end(), earlier.node) == edges.end())
                    edges.push_back(earlier.node);
            };
            for (const Record &w : writes_)
                against(w);
            if (u.write)
                for (const Record &rd : reads_)
                    against(rd);
        }
    }
    if (need & FlushShaderData)
        need &= Barriers(~WaitForIdle);
    if (need & (WaitForIdle | FlushShaderData))
        need |= need & kInvalidates;
    return need;
}

void Placer::record(uint32_t head, uint32_t end)
{
    for (uint32_t n = head; n < end; ++n) {
        const Node &node = g_.nodes[n];
        for (uint32_t i = 0; i < node.uses.count; ++i) {
            const Use &u = g_.uses[node.uses.first + i];
            const Record rec{regionOf(r_, u), Access{u.path, u.write, u.stage, node.engine}, n};
            if (u.write) {
                writes_.erase(std::remove_if(writes_.begin(), writes_.end(),
                                             [&](const Record &w) {
                                                 return w.region.same(rec.region) && w.access.path == rec.access.path;
                                             }),
                              writes_.end());
                reads_.erase(std::remove_if(reads_.begin(), reads_.end(),
                                            [&](const Record &rd) {
                                                return rd.region.overlaps(rec.region);
                                            }),
                             reads_.end());
                writes_.push_back(rec);
            } else {
                reads_.push_back(rec);
            }
        }
    }
}

// Colour attachments between the runs of one pass (fused passes included): raw_attachment_order.h's rule, with no
// region test, since a region test needs the draw's vertices
void Placer::inPass(uint32_t head, uint32_t end)
{
    raw::AttachmentWrites writes;
    for (uint32_t n = head; n < end; ++n) {
        const Node &node = g_.nodes[n];
        for (uint32_t i = 0; i < node.runs.count; ++i) {
            DrawRun &run = g_.runs[node.runs.first + i];
            uint8_t ropRead = 0, ropWrite = 0, sampled = 0, storage = 0, storageWrite = 0;
            if (run.program != kNone) {
                const ProgramFacts &p = r_.programs[run.program];
                for (uint32_t c = 0; c < 8; ++c) {
                    const uint8_t bit = uint8_t(1u << c);
                    const bool fetched = p.fetchMask & bit;
                    const Rung rung = run.rung[c];
                    if (fetched && rung == Rung::Interlock) {
                        storage |= bit;
                        storageWrite |= p.writeMask & bit;
                    } else if (fetched && rung == Rung::Barrier) {
                        sampled |= bit;
                    } else if (fetched) {
                        ropRead |= bit;
                    }
                    if ((p.writeMask & bit) && !(storage & bit))
                        ropWrite |= bit;
                    if (p.readMask & bit)
                        ropRead |= bit;
                }
            } else {
                ropWrite = run.own == OwnState::StandInStore ? 3 : 1;
            }
            const raw::AttachmentAccess access{ropRead, ropWrite, sampled, storage};
            const raw::AttachmentBarrier needed = access.after(writes.rop, writes.storage);
            if (needed == raw::AttachmentBarrier::Shader)
                run.before |=
                    hazard({Path::Rop, true}, {Path::Storage, true}, Scope::InPass, caps_.tiledCache).barriers;
            else if (needed == raw::AttachmentBarrier::Pixel)
                run.before |= PixelShaderBarrier;
            if (run.before & (FlushShaderData | WaitForIdle))
                writes.synchronized(raw::AttachmentBarrier::Shader);
            else if (run.before & PixelShaderBarrier)
                writes.synchronized(raw::AttachmentBarrier::Pixel);
            else
                writes.synchronized(needed);
            if (run.before)
                note(run.before, n, run.before & (FlushShaderData | WaitForIdle));
            writes.rop |= ropWrite;
            writes.storage |= storageWrite;
        }
    }
}

Barriers Placer::flushOutstanding() const
{
    Barriers out = 0;
    for (const Record &w : writes_) {
        if (caps_.tiledCache && (w.access.path == Path::Rop || w.access.path == Path::Zrop) &&
            last(TiledCacheFlush) <= int64_t(w.node))
            out |= TiledCacheFlush;
        if (w.access.path == Path::Storage && last(FlushShaderData) <= int64_t(w.node))
            out |= FlushShaderData;
    }
    return out;
}

Barriers Placer::entry(const JobContext &job) const
{
    Barriers out = InvalidateTextureHeaders | InvalidateSamplers;
    for (const Use &u : g_.uses) {
        if (u.write)
            continue;
        if (u.path == Path::Texture)
            out |= InvalidateTextureData;
        if (u.path == Path::Storage)
            out |= InvalidateShaderData;
        if (u.path == Path::Constant)
            out |= InvalidateConstants | InvalidateShaderData;
    }
    if (job.heapPlaced)
        out |= InvalidateInstructions;
    // Until a probe shows every job ends drained with its storage writes flushed, the entry flushes and drains as
    // raw_gpu.cpp's submission prologue does
    if (!caps_.selectable(Rule::EntryNoDrain))
        out |= FlushShaderData | InvalidateShaderData | InvalidateTextureData;
    return out;
}

void Placer::run(const JobContext &job)
{
    g_.entry = entry(job);
    const bool trustSwitch = caps_.selectable(Rule::SwitchDrain);
    Engine previous = Engine::None;
    for (uint32_t head = 0; head < g_.nodes.size();) {
        uint32_t end = head + 1;
        while (end < g_.nodes.size() && g_.nodes[end].continues)
            ++end;
        Node &node = g_.nodes[head];
        std::vector<uint32_t> edges;
        Barriers need = 0;
        if (node.kind == NodeKind::Signal) {
            need = flushOutstanding();
        } else if (node.kind == NodeKind::Wait) {
            need = InvalidateTextureData | InvalidateShaderData | InvalidateConstants;
        } else {
            need = crossNode(head, end, edges);
        }
        for (const FenceEdge &f : g_.fences)
            if (f.waiter >= head && f.waiter < end) {
                need |= kFenceWait;
                if (std::find(edges.begin(), edges.end(), f.updater) == edges.end())
                    edges.push_back(f.updater);
            }
        const bool switched = switchDrains(previous, node.engine);
        if (switched && trustSwitch && (need & WaitForIdle) && !(need & FlushShaderData))
            need = Barriers((need & ~WaitForIdle) | EngineSwitch);
        else if (switched)
            need |= EngineSwitch;
        node.before = need;
        node.after = {uint32_t(g_.edges.size()), uint32_t(edges.size())};
        g_.edges.insert(g_.edges.end(), edges.begin(), edges.end());
        const bool drained = (need & (WaitForIdle | FlushShaderData)) || (trustSwitch && (need & EngineSwitch));
        note(need, head, drained || (node.kind == NodeKind::Signal && caps_.selectable(Rule::EntryNoDrain)));
        inPass(head, end);
        record(head, end);
        if (onChannel(g_.nodes[end - 1].engine))
            previous = g_.nodes[end - 1].engine;
        head = end;
    }
    g_.exit = flushOutstanding();
}

}  // namespace

void addBarriers(Counts &c, Barriers b)
{
    const Barriers parts = b & kShaderCaches;
    if (b & TiledCacheFlush)
        ++c.tiledFlushes;
    if ((b & FlushShaderData) || (parts && (b & WaitForIdle))) {
        ++c.shaderInval;
        ++c.drains;
    } else {
        if (parts)
            ++c.shaderInvalNoWfi;
        if (b & WaitForIdle) {
            ++c.wfi;
            ++c.drains;
        }
    }
    if ((b & EngineSwitch) && !(b & (WaitForIdle | FlushShaderData))) {
        ++c.switchDrains;
        ++c.drains;
    }
    if (b & InvalidateTextureData)
        ++c.texInval;
    if (b & PixelShaderBarrier) {
        ++c.psBarriers;
        ++c.texInval;
    }
    c.setReference += (b & SetReference) != 0;
    c.copyOrdered += (b & CopyNonPipelined) != 0;
    c.headerInval += (b & InvalidateTextureHeaders) != 0;
    c.samplerInval += (b & InvalidateSamplers) != 0;
}

Counts countGraph(const WorkGraph &g)
{
    Counts c;
    addBarriers(c, g.entry);
    for (const Node &n : g.nodes)
        addBarriers(c, n.before);
    for (const DrawRun &run : g.runs)
        addBarriers(c, run.before);
    addBarriers(c, g.exit);
    return c;
}

void placeBarriers(WorkGraph &g, const Recording &r, const Caps &caps, const JobContext &job)
{
    Placer(g, r, caps).run(job);
}

}  // namespace raw::plan
