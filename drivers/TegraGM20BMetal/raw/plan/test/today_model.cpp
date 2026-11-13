// SPDX-License-Identifier: MIT
#include "today_model.h"
#include "../../runtime/raw_attachment_order.h"
#include <vector>

namespace raw::plan::test {

namespace {

constexpr Barriers kSubmissionPrologue = FlushShaderData | InvalidateShaderData | InvalidateTextureData;

// RenderEncoder::end's switch_gm20b_pass_barrier: WAIT_FOR_IDLE, then INVALIDATE_SHADER_CACHES (which waits again)
void passBarrier(Counts &c, bool tiled)
{
    c.tiledFlushes += tiled;
    ++c.wfi;
    ++c.shaderInval;
    ++c.texInval;
    ++c.drains;
}

bool threeD(const Node &n)
{
    return n.engine == Engine::ThreeD && n.kind != NodeKind::Dispatch;
}

void inPass(Counts &c, const WorkGraph &g, const Recording &r, uint32_t head, uint32_t end, bool tiledCache,
            bool &storageWritten)
{
    raw::AttachmentWrites writes;
    for (uint32_t n = head; n < end; ++n) {
        const Node &node = g.nodes[n];
        for (uint32_t i = 0; i < node.runs.count; ++i) {
            const DrawRun &run = g.runs[node.runs.first + i];
            if (run.requested)
                passBarrier(c, tiledCache);
            if (run.program == kNone)
                continue;
            const ProgramFacts &p = r.programs[run.program];
            const uint8_t storage = p.storageMask, ropWrite = p.writeMask & ~p.storageMask;
            const raw::AttachmentAccess access{p.readMask, ropWrite, uint8_t(p.fetchMask & ~p.storageMask), storage};
            const raw::AttachmentBarrier needed = access.after(writes.rop, writes.storage);
            if (needed == raw::AttachmentBarrier::Shader) {
                c.tiledFlushes += tiledCache;
                ++c.shaderInval;
                ++c.texInval;
                ++c.drains;
            } else if (needed == raw::AttachmentBarrier::Pixel) {
                ++c.psBarriers;
                ++c.texInval;
            }
            writes.synchronized(needed);
            writes.rop |= p.writeMask;
            writes.storage |= p.storageWriteMask;
            storageWritten |= p.storageWriteMask != 0;
            for (uint32_t d = 0; d < run.draws.count; ++d)
                if (run.draws.first + d < r.draws.size() && r.draws[run.draws.first + d].indirect) {
                    ++c.wfi;
                    ++c.shaderInval;
                    ++c.drains;
                }
        }
    }
}

std::vector<uint32_t> transferEncoders(const Recording &r)
{
    std::vector<uint32_t> out(r.transfers.size(), kNone);
    for (uint32_t e = 0; e < r.encoders.size(); ++e)
        for (uint32_t i = 0; i < r.encoders[e].ops.count; ++i) {
            const Op &op = r.ops[r.encoders[e].ops.first + i];
            if (op.kind == OpKind::Transfer)
                out[op.payload] = e;
        }
    return out;
}

}  // namespace

Counts todayCounts(const WorkGraph &g, const Recording &r, bool tiledCache)
{
    Counts c;
    addBarriers(c, kSubmissionPrologue | InvalidateTextureHeaders | InvalidateSamplers);
    c.shaderInvalNoWfi += 2;
    const std::vector<uint32_t> encoderOf = transferEncoders(r);
    uint32_t blitEncoder = kNone;
    bool computeOpen = false;
    Engine previous = Engine::None;
    for (uint32_t head = 0; head < g.nodes.size();) {
        uint32_t end = head + 1;
        while (end < g.nodes.size() && g.nodes[end].continues)
            ++end;
        const Node &node = g.nodes[head];
        const uint32_t drainsBefore = c.drains;
        if (threeD(node)) {
            passBarrier(c, false);
            bool storage = false;
            inPass(c, g, r, head, end, tiledCache, storage);
            c.tiledFlushes += tiledCache;
            if (storage) {
                ++c.shaderInval;
                ++c.drains;
            }
            blitEncoder = kNone;
            computeOpen = false;
        } else if (node.engine == Engine::Copy || node.engine == Engine::TwoD) {
            const uint32_t encoder =
                node.source != kNone && node.rule != Rule::Recorded && node.kind != NodeKind::Convert
                    ? encoderOf[node.source]
                    : kNone;
            if (encoder == kNone || encoder != blitEncoder) {
                ++c.wfi;
                ++c.drains;
            }
            blitEncoder = encoder;
            computeOpen = false;
        } else if (node.kind == NodeKind::Dispatch) {
            if (!computeOpen) {
                passBarrier(c, false);
                ++c.headerInval;
                ++c.samplerInval;
            } else {
                ++c.shaderInval;
                ++c.texInval;
                ++c.drains;
            }
            computeOpen = true;
            blitEncoder = kNone;
        }
        if (switchDrains(previous, node.engine) && c.drains == drainsBefore) {
            ++c.switchDrains;
            ++c.drains;
        }
        if (onChannel(g.nodes[end - 1].engine))
            previous = g.nodes[end - 1].engine;
        head = end;
    }
    return c;
}

}  // namespace raw::plan::test
