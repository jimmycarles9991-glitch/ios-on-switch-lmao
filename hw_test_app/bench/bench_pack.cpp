// SPDX-License-Identifier: MIT
// The bench's pack kind: a pack state drawn with its own programs as the render server draws it, each quad with the
// caller's uniform blocks, in one form of a split draw (switch_bench_pack_form), and each prologue's frontier read back
// from the upload ring once the command buffer's fence passed and before the next begin() reuses it.
#include <cstring>
#include <string>
#include <vector>
#include "hw_test_app/bench/bench_device.h"

namespace bench {
namespace {

constexpr uint32_t kTriangleStrip = 4, kMaxQuads = 64, kMaxBlocks = 8, kMaxVertexBytes = 4096;
// Each quad's blocks lie in a region of their own, so two quads' different contents never share an address
constexpr uint64_t kQuadRegion = 64 * 1024;

std::string invalidPack(const switch_bench_experiment &x)
{
    const switch_bench_pack &p = x.pack;
    if (!x.template_key)
        return "pack: a template key";
    if (p.form > SWITCH_BENCH_PACK_SPLIT_WHOLE)
        return "pack: form";
    if (!p.quad_count || p.quad_count > kMaxQuads || !p.quads)
        return "pack: 1..64 quads";
    for (uint32_t q = 0; q < p.quad_count; q++) {
        const switch_bench_quad &quad = p.quads[q];
        if (quad.rect[0] >= quad.rect[2] || quad.rect[1] >= quad.rect[3] || quad.rect[2] > x.width ||
            quad.rect[3] > x.height)
            return "pack: a quad's rect outside the target";
        if (quad.block_count > kMaxBlocks || (quad.block_count && !quad.blocks))
            return "pack: at most 8 blocks a quad";
        if (quad.vertices && (!quad.vertex_bytes || quad.vertex_bytes > kMaxVertexBytes || !quad.count ||
                              quad.primitive > kTriangleStrip))
            return "pack: a quad's vertices (1..4096 bytes), primitive (0..4) and count (> 0)";
        for (uint32_t k = 0; k < quad.block_count; k++) {
            const switch_bench_block &b = quad.blocks[k];
            if ((b.stage != 1 && b.stage != 2) || b.slot >= 16 || !b.bytes || !b.data ||
                uint64_t(b.offset) + b.bytes > kQuadRegion)
                return "pack: a block's stage (1, 2), slot (< 16) or extent (inside 64 KiB)";
        }
    }
    return {};
}

void applyForm(raw::CommandBuffer &cb, uint32_t form)
{
    cb.forceWholePrograms(form == SWITCH_BENCH_PACK_WHOLE);
    cb.forceSplit(form != SWITCH_BENCH_PACK_WHOLE);
    cb.splitProbe(form == SWITCH_BENCH_PACK_WHOLE_GENERIC ? raw::SplitProbe::Generic
                  : form == SWITCH_BENCH_PACK_SPLIT_WHOLE ? raw::SplitProbe::LaunchOnly
                                                          : raw::SplitProbe::Twin);
}

bool drawQuads(switch_bench &b, const switch_bench_experiment &x, raw::CommandBuffer &cb, Target &t,
               raw::Pipeline *pipeline, const raw::Buffer &blocks, Submission &s)
{
    raw::PassDesc pass;
    pass.count = 1 + uint32_t(t.others.size());
    pass.color[0].texture = &t.color;
    pass.color[0].load = raw::ColorAttachment::Clear;
    memcpy(pass.color[0].clear, x.clear, sizeof x.clear);
    for (size_t i = 0; i < t.others.size(); i++) {
        pass.color[1 + i].texture = &t.others[i];
        pass.color[1 + i].load = raw::ColorAttachment::Clear;
        pass.color[1 + i].store = false;
    }
    raw::RenderEncoder *e = cb.render(pass);
    if (!e)
        return s.refused("the raw path cannot render the target");
    e->setViewport(0, 0, x.width, x.height, 0, 1);
    e->setScissor(0, 0, x.width, x.height);
    e->setPipeline(pipeline);
    for (uint32_t q = 0; q < x.pack.quad_count; q++) {
        const switch_bench_quad &quad = x.pack.quads[q];
        bindTemplate(e, *pipeline, quad.rect, x.width, x.height, b.image, b.sampler);
        for (uint32_t k = 0; k < quad.block_count; k++)
            e->setBuffer(int(quad.blocks[k].stage), quad.blocks[k].slot, &blocks,
                         q * kQuadRegion + quad.blocks[k].offset);
        if (quad.vertices && pipeline->rec->vertexSlot != 0xff)
            e->setBytes(1, pipeline->rec->vertexSlot, quad.vertices, quad.vertex_bytes);
        const char *why = nullptr;
        const bool own = quad.vertices != nullptr;
        if (!e->draw(own ? quad.primitive : kTriangleStrip, own ? quad.first : 0, own ? quad.count : 4, 1, 0, &why)) {
            e->end();
            const raw::Refusal refusal = cb.refusal;
            char line[160];
            snprintf(line, sizeof line, "quad %u refused: %s (predicate %u: %u %u %u)", q, why ? why : "?", refusal.id,
                     refusal.a, refusal.b, refusal.c);
            return s.refused(line);
        }
    }
    e->end();
    return true;
}

// The command buffer's frontiers into the caller's room, in launch order; launches counts them all
void readFrontiers(raw::Device &d, const raw::CommandBuffer &cb, switch_bench_result &r)
{
    const size_t room = r.frontier ? r.frontier_bytes : 0;
    size_t at = 0;
    r.launches = 0;
    for (const auto &[va, bytes] : cb.splitFrontiers()) {
        r.launches++;
        if (at + bytes <= room && d.readUpload(va, r.frontier + at, bytes))
            at += bytes;
    }
    r.frontier_bytes = at;
}

}  // namespace

void runPack(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    if (const std::string why = invalidPack(x); !why.empty())
        return fail(r, SWITCH_BENCH_INVALID, why);
    raw::Device &d = *b.device;
    raw::Pipeline *pipeline = d.pipeline(x.template_key, raw::PackForm::Default, x.pack.ca_transform != 0);
    if (!pipeline)
        return fail(r, SWITCH_BENCH_INVALID, "pack: no state of that key in the pack");
    r.template_key = x.template_key;
    raw::Buffer blocks;
    if (!d.createBuffer(uint64_t(x.pack.quad_count) * kQuadRegion, blocks, true))
        return fail(r, SWITCH_BENCH_ERROR, "no memory for the quads' blocks");
    for (uint32_t q = 0; q < x.pack.quad_count; q++)
        for (uint32_t k = 0; k < x.pack.quads[q].block_count; k++) {
            const switch_bench_block &block = x.pack.quads[q].blocks[k];
            memcpy(blocks.cpu + q * kQuadRegion + block.offset, block.data, block.bytes);
        }
    Target t;
    const std::vector<uint32_t> others =
        x.single_target ? std::vector<uint32_t>() : otherFormats(d.pack().string(pipeline->rec->keyString));
    bool ran = makeTarget(d, x, others, false, t, r);
    Submission s(b, x, r);
    raw::CommandBuffer *last = nullptr;
    for (uint32_t k = 0; ran && k < x.warmup + x.repeats; k++) {
        uint64_t gpu = 0, wall = 0;
        ran = s.record(
            [&](raw::CommandBuffer &cb) {
                applyForm(cb, x.pack.form);
                last = &cb;
                return drawQuads(b, x, cb, t, pipeline, blocks, s);
            },
            gpu, wall);
        if (ran && k >= x.warmup) {
            r.gpu_ns[k - x.warmup] = gpu;
            r.wall_ns[k - x.warmup] = wall;
        }
    }
    if (ran && last)
        readFrontiers(d, *last, r);
    if (ran && !(ran = readBack(d, t.color, x, raw::formatInfo(x.format)->bpp, r.texels, r.texel_bytes)))
        fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a read lease on the target");
    if (ran)
        checkAlive(b, x, t, r);
    destroyTarget(d, t);
    d.destroyBuffer(blocks);
}

}  // namespace bench
