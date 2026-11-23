// SPDX-License-Identifier: MIT
// The blit encoder's probe ops (blit_encoder.h BlitEncoder::probe, command_buffer.h CommandBuffer::probeResolve): debug
// builds only
#include "fx/fx_image.h"
#include "gpu.h"
#include "gm20b_gpu_internal.h"
#include "tiled_cache.h"

namespace gm20b {

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static ProbeSurface probeSurface(const Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d;
    s.pitch = t.pitch;
    s.width = width;
    s.height = height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

// The op's surfaces from the textures' memory at encode, which a private texture's move to plain memory may have changed
// since the op was recorded; its formats and sizes as recorded
void BlitEncoder::probe(const ProbeOp &recorded, const Texture *read, const Texture &written)
{
    ProbeOp op = recorded;
    if (op.kind != ProbeOpKind::Method) {
        op.dst = probeSurface(written, op.dst.fmt2d, op.dst.width, op.dst.height);
        if (read)
            op.src = probeSurface(*read, op.src.fmt2d, op.src.width, op.src.height);
    }
    room(uint32_t(probeOpWords(op)) + kProbeBracketWords);
    if (read)
        cb_->use(*read, false);
    cb_->use(written, true);
    if (op.bracketed) {
        tiledCacheDrain(p_, dev_->k_.info());
        p_.m(WAIT_FOR_IDLE, 0);
    }
    emitProbeOp(p_, op);
    if (op.bracketed)
        probeOpAfter(p_);
    cb_->taken_.ops++;
}

bool CommandBuffer::probeResolve(const Texture &multisampled, const Texture &into)
{
    fx::MipChain grid;
    const uint32_t fmt2d = probeTwodFormat(into.format);
    ProbeOp op;
    if (!fmt2d ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid) ||
        !probeResolveOp(
            probeSurface(multisampled, fmt2d, multisampled.width << grid.sxLog2, multisampled.height << grid.syLog2),
            grid.sxLog2, grid.syLog2, probeSurface(into, fmt2d, into.width, into.height), op))
        return false;
    BlitEncoder *b = blit();
    b->probe(op, &multisampled, into);
    b->end();
    taken_.resolves++;
    return true;
}
#endif

}  // namespace gm20b
