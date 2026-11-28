// SPDX-License-Identifier: MIT
// The range encode's copies of the images textures are reached through, and of texel buffers held in a pool of a
// block-linear surface's rows (texel_rows.h) around the passes that render them and the blit commands that reach them
// (a draw's and a kernel's are refresh(), writtenBack() and the kernel shadows')
#import "TegraGM20BMetalRangeEncode.h"
#include "overlap_rows.h"
#include "texel_rows.h"
#include "xr_pools.h"

void gm20bMoveShadowImage(gm20b::BlitEncoder &b, const gm20b::Texture &t, bool fill)
{
    if (t.xrPlanes) {
        gm20b::moveXrPools(b, t, fill);
    } else if (t.texelRows) {
        gm20b::moveTexelRows(b, t, fill);
    } else if (fill) {
        b.copyTexels(t, 0, 0, 0, *t.sampled, 0, 0, 0, t.width, t.height);
    } else {
        gm20b::Texture scratch;
        b.copyTexels(*t.sampled, 0, 0, 0, gm20b::storedRows(t, t, scratch), 0, 0, 0, t.width, t.height);
    }
}

// A clear reaches the render area alone in a pass that draws (TegraGM20BMetalRangeResolve.mm), and the pool goes back whole: it
// is filled before every pass into it
void SwitchMetalRangeEncode::texelRowsTargets(const SwitchMetalPass &pass)
{
    const uint8_t mask = pass.colorMask ? pass.colorMask : 0xffu;
    for (uint8_t m = mask; m; m &= uint8_t(m - 1)) {
        const SwitchMetalPass::Color &a = pass.color[__builtin_ctz(m)];
        __unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)a.texture;
        if (!t || !t->_raw.texelRows)
            continue;
        gm20b::BlitEncoder *b = rc_->blit();
        gm20b::moveTexelRows(*b, t->_raw, true);
        b->end();
        if (a.store != MTLStoreActionDontCare &&
            std::find(shadowsWritten_.begin(), shadowsWritten_.end(), t) == shadowsWritten_.end())
            shadowsWritten_.push_back(t);
    }
}

void SwitchMetalRangeEncode::blitTexelRows(const Cmd &c, bool before)
{
    __unsafe_unretained SwitchMetalTexture *read = nil, *written = nil;
    switch (c.op) {
    case OpCopyTexture:
        read = (__bridge SwitchMetalTexture *)c.obj;
        written = (__bridge SwitchMetalTexture *)c.obj2;
        break;
    case OpFillTexture:
        written = (__bridge SwitchMetalTexture *)c.obj;
        break;
    case OpCopyBufferToTexture:
        written = (__bridge SwitchMetalTexture *)c.obj2;
        break;
    case OpCopyTextureToBuffer:
        read = (__bridge SwitchMetalTexture *)c.obj;
        break;
    default:
        return;
    }
    if (read == written)
        read = nil;
    for (__unsafe_unretained SwitchMetalTexture *t : {read, written}) {
        if (!t || !t->_raw.texelRows || (!before && t != written))
            continue;
        // the name encodeBlit orders the command by: every texture over the surface stands for its memory
        const void *surface = (t->_levelsOwner ?: t)->_surface;
        be_->access(surface, surface);
        gm20b::moveTexelRows(*be_, t->_raw, before);
    }
}
