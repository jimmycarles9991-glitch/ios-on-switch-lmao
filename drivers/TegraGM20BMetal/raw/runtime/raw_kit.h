// SPDX-License-Identifier: MIT
// kits/SwitchGM20BEmit's emitters writing into a raw Push: the kit's push over the Push's room, the Push then past what
// they wrote. The kit is C and out of line: each sequence is a call.
#pragma once
#include <cstddef>
#include <cstdint>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmit3D.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitCopy.h"
#include "raw_push.h"
#include "raw_transfer.h"

namespace raw {

// One of the kit's emitters, emit(push, args...)
template <typename Emit, typename... Args> void kit(Push &p, Emit emit, Args &&...args)
{
    nv_push n;
    nv_push_init(&n, p.at, size_t(p.end - p.at), SUBC_MASK_ALL);
    emit(&n, static_cast<Args &&>(args)...);
    p.at = n.end;
}

// SET_REMAP_COMPONENTS of a texel copy: DST_X..W = SRC_X..W, each texel min(bpp, 4)-byte components
inline uint32_t copyTexelRemap(uint32_t bpp)
{
    const uint32_t component = bpp < 4 ? bpp : 4;
    const uint32_t count = bpp / component;
    return 0 | 1 << 4 | 2 << 8 | 3 << 12 | (component - 1) << 16 | (count - 1) << 20 | (count - 1) << 24;
}

// A side's texel is the remap's component count times its component size; a block-linear side's width is its row stride
// in those texels
inline switch_gm20b_copy_surface kitSurface(const CopySurface &s, uint32_t bpp)
{
    switch_gm20b_copy_surface k = {};
    k.base_addr = s.va;
    k.is_3d = true;
    k.bpp = bpp;
    k.row_stride = s.pitch;
    k.tiled = s.blockLinear;
    k.extent_el.height = s.height;
    k.extent_el.depth = s.depth;
    k.offset_el.x = s.blockLinear ? s.x : 0;
    k.offset_el.y = s.blockLinear ? s.y : 0;
    k.offset_el.z = s.layer;
    k.y_log2 = s.blockHeight;
    k.z_log2 = s.blockDepth;
    return k;
}

// `width` x `height` texels through `remap`, a SET_REMAP_COMPONENTS word; `ordered`: after every earlier copy completed
inline void emitCopy(Push &p, const CopySurface &source, const CopySurface &destination, uint32_t width,
                     uint32_t height, uint32_t remap, bool ordered)
{
    const uint32_t size = (remap >> 16 & 3) + 1;
    switch_gm20b_copy c = {};
    c.src = kitSurface(source, size * ((remap >> 20 & 3) + 1));
    c.dst = kitSurface(destination, size * ((remap >> 24 & 3) + 1));
    c.remap.comp_size = uint8_t(size);
    for (uint32_t i = 0; i < 4; i++)
        c.remap.dst[i] = uint8_t(remap >> 4 * i & 7);
    c.extent_el.width = width;
    c.extent_el.height = height;
    c.extent_el.depth = c.extent_el.array_len = 1;
    const uint8_t type =
        ordered ? NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED : NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED;
    kit(p, switch_gm20b_copy_rect, &c, type);
}

// A byte copy in lines of at most NVK's 128 KiB (nvk_cmd_copy_memory_ce), pitch both sides, no remap: one multi-line copy
// of the whole lines, one of the rest. The first starts after every copy before it on the channel landed (a copy of the
// same bytes there may hold older ones); the second's bytes are the first's neighbours, not its own.
constexpr uint32_t kCopyLineBytes = 1u << 17;
constexpr uint32_t kCopyBytesWords = 2 * SWITCH_GM20B_COPY_RECT_WORDS(1);

inline void emitCopyBytes(Push &p, uint64_t source, uint64_t destination, uint64_t bytes)
{
    bool first = true;
    auto lines = [&](uint64_t at, uint32_t length, uint32_t count) {
        switch_gm20b_copy c = {};
        c.src.bpp = 1;
        c.src.row_stride = length;
        c.src.extent_el = {length, count, 1, 1};
        c.dst = c.src;
        c.src.base_addr = source + at;
        c.dst.base_addr = destination + at;
        c.extent_el = {length, count, 1, 1};
        kit(p, switch_gm20b_copy_rect, &c,
            uint8_t(first ? NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED
                          : NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
        first = false;
    };
    const uint64_t whole = bytes / kCopyLineBytes, rest = bytes % kCopyLineBytes;
    if (whole)
        lines(0, kCopyLineBytes, uint32_t(whole));
    if (rest)
        lines(whole * kCopyLineBytes, uint32_t(rest), 1);
}

}  // namespace raw
