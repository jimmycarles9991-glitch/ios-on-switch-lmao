// SPDX-License-Identifier: MIT
#include "plan_validate.h"

namespace raw::plan {

namespace {

// MTLCommandBuffer.h: MTLCommandBufferErrorInternal
constexpr long kCommandBufferErrorInternal = 1;

Finding found(Verdict v, Problem p, uint32_t encoder)
{
    Finding f;
    f.verdict = v;
    f.problem = p;
    f.encoder = encoder;
    return f;
}

uint32_t levelSize(uint32_t size, uint16_t level)
{
    const uint32_t s = level < 32 ? size >> level : 0;
    return s ? s : 1;
}

Problem targetGap(const ResourceFacts &f)
{
    if (f.has(ResourceFacts::RotatedPlane))
        return Problem::RotatedTarget;
    if (f.has(ResourceFacts::TwoPlaneXr))
        return Problem::TwoPlaneExtendedRange;
    if (f.has(ResourceFacts::PlanarTarget))
        return Problem::PlanarTarget;
    if (f.samples > 1 && !f.has(ResourceFacts::Memoryless))
        return Problem::StoredMultisample;
    return Problem::None;
}

Finding checkPass(const Recording &r, const Encoder &e, uint32_t index, const Answers &answers)
{
    const PassDesc &pass = r.passes[e.pass];
    if (pass.layers > 1 && !answers.layeredRendering)
        return found(Verdict::QueryGated, Problem::LayeredRendering, index);
    if (pass.rateMap && !answers.rateMaps)
        return found(Verdict::QueryGated, Problem::RasterizationRateMap, index);
    if (pass.samplePositions && !answers.samplePositions)
        return found(Verdict::QueryGated, Problem::SamplePositions, index);
    for (const Attachment *a : {&pass.color[0], &pass.color[1], &pass.color[2], &pass.color[3], &pass.color[4],
                                &pass.color[5], &pass.color[6], &pass.color[7], &pass.depth, &pass.stencil}) {
        if (a->texture == kNone)
            continue;
        const ResourceFacts &f = r.resources[a->texture];
        if ((pass.width && pass.width > levelSize(f.width, a->level)) ||
            (pass.height && pass.height > levelSize(f.height, a->level)))
            return found(Verdict::ClientError, Problem::RenderTargetSize, index);
        if (f.has(ResourceFacts::Memoryless) && (a->load == Load::Load || a->store == Store::Store))
            return found(Verdict::ClientError, Problem::MemorylessAccess, index);
        if (const Problem gap = targetGap(f); gap != Problem::None)
            return found(Verdict::NoGpuRung, gap, index);
    }
    return {};
}

Finding checkDraw(const Recording &r, const Encoder &e, const Draw &d, uint32_t index)
{
    const PassDesc &pass = r.passes[e.pass];
    const DrawState &s = r.states[d.state];
    if (d.indexSize && d.indexOffset % d.indexSize)
        return found(Verdict::ClientError, Problem::IndexBufferOffset, index);
    for (uint32_t k = 0; k < s.bindings.count; ++k) {
        const Binding &b = r.bindings[s.bindings.first + k];
        if (b.resource == kNone)
            continue;
        const ResourceFacts &f = r.resources[b.resource];
        if (f.has(ResourceFacts::Memoryless))
            return found(Verdict::ClientError, Problem::MemorylessAccess, index);
        if (f.kind == ResourceFacts::Sampler && f.has(ResourceFacts::UnsupportedSampler))
            return found(Verdict::NoGpuRung, Problem::SamplerAddressMode, index);
        if (f.kind == ResourceFacts::Texture && f.samples > 1 && b.path == Path::Texture)
            return found(Verdict::NoGpuRung, Problem::StoredMultisample, index);
    }
    if (s.program == kNone)
        return {};
    const ProgramFacts &p = r.programs[s.program];
    uint8_t samples = 1;
    for (uint32_t j = 0; j < 8; ++j) {
        const Attachment &a = pass.color[j];
        const uint16_t format = a.texture != kNone ? r.resources[a.texture].format : 0;
        if (a.texture != kNone)
            samples = r.resources[a.texture].samples;
        if (p.colorFormats[j] != format)
            return found(Verdict::ClientError, Problem::PipelineColorFormat, index);
        if ((p.fetchMask >> j & 1) && (p.fetchRung[j] & 0x0f) == uint8_t(Rung::Interlock) &&
            !(p.forms & ProgramFacts::StorageForm))
            return found(Verdict::NoGpuRung, Problem::FetchWithoutStorageForm, index);
    }
    const uint16_t depth = pass.depth.texture != kNone ? r.resources[pass.depth.texture].format : 0;
    if (pass.depth.texture != kNone)
        samples = r.resources[pass.depth.texture].samples;
    if (p.depthFormat != depth)
        return found(Verdict::ClientError, Problem::PipelineDepthFormat, index);
    if (p.samples != samples)
        return found(Verdict::ClientError, Problem::PipelineSampleCount, index);
    return {};
}

Finding checkTransfer(const Recording &r, const Transfer &t, uint32_t index)
{
    for (uint32_t slot : {t.source, t.destination})
        if (slot != kNone && r.resources[slot].has(ResourceFacts::Memoryless))
            return found(Verdict::ClientError, Problem::MemorylessAccess, index);
    const auto past = [&](uint32_t slot, uint64_t offset) {
        if (slot == kNone)
            return false;
        const ResourceFacts &f = r.resources[slot];
        return f.kind == ResourceFacts::Buffer && f.bytes && t.bytes && offset + t.bytes > f.bytes;
    };
    if ((t.kind == Transfer::CopyBuffer &&
         (past(t.source, t.sourceOffset) || past(t.destination, t.destinationOffset))) ||
        (t.kind == Transfer::FillBuffer && past(t.destination, t.destinationOffset)))
        return found(Verdict::ClientError, Problem::BufferRange, index);
    if (t.kind == Transfer::CopyTexture && Caps::format(r.resources[t.source].format) &&
        Caps::format(r.resources[t.destination].format) &&
        Caps::format(r.resources[t.source].format)->bytes != Caps::format(r.resources[t.destination].format)->bytes)
        return found(Verdict::ClientError, Problem::CopyTexelSize, index);
    return {};
}

Finding checkDispatch(const Dispatch &d, uint32_t index, const Answers &answers)
{
    if (!d.perThread || answers.nonUniformThreadgroups)
        return {};
    for (uint32_t i = 0; i < 3; ++i)
        if (d.threads[i] && d.grid[i] % d.threads[i])
            return found(Verdict::QueryGated, Problem::NonUniformThreadgroups, index);
    return {};
}

Finding checkEncoder(const Recording &r, uint32_t index, const Answers &answers)
{
    const Encoder &e = r.encoders[index];
    if (e.kind == EncoderKind::Render)
        if (const Finding f = checkPass(r, e, index, answers); !f.plannable())
            return f;
    for (uint32_t i = 0; i < e.ops.count; ++i) {
        const Op &op = r.ops[e.ops.first + i];
        Finding f;
        if (op.kind == OpKind::Draw)
            f = checkDraw(r, e, r.draws[op.payload], index);
        else if (op.kind == OpKind::Transfer)
            f = checkTransfer(r, r.transfers[op.payload], index);
        else if (op.kind == OpKind::Dispatch)
            f = checkDispatch(r.dispatches[op.payload], index, answers);
        if (!f.plannable())
            return f;
    }
    return {};
}

Problem unplannedProblem(const Node &n)
{
    switch (n.kind) {
    case NodeKind::Resolve:
        return Problem::ResolveFormat;
    case NodeKind::MipChain:
        return Problem::MipFormat;
    case NodeKind::Convert:
        return Problem::ConversionState;
    default:
        return Problem::CompressedCopy;
    }
}

}  // namespace

Finding validate(const Recording &r, const Built &built, const Answers &answers, const ChannelState &channel)
{
    if (channel.lost) {
        Finding f = found(Verdict::CommandBufferError, Problem::ChannelLost, kNone);
        f.commandBufferError = kCommandBufferErrorInternal;
        return f;
    }
    for (uint32_t i = 0; i < r.encoders.size(); ++i)
        if (const Finding f = checkEncoder(r, i, answers); !f.plannable())
            return f;
    for (uint32_t n : built.unplanned)
        return found(Verdict::NoGpuRung, unplannedProblem(built.graph.nodes[n]), built.graph.nodes[n].source);
    return {};
}

}  // namespace raw::plan
