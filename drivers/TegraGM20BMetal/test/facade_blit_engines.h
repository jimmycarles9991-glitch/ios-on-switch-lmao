// SPDX-License-Identifier: MIT
// A blit command buffer's engine work run over the kernel model's memory: the copy engine's (gm20b_ce_model.h) and the
// 2D engine's (twod_sim.h; its solid rectangles here, which twod_sim.h does not run) methods of every job submitted from
// one on, so a test reads the bytes they leave.
#pragma once
#include "facade_harness.h"
#include "drivers/TegraGM20B/test/gm20b_ce_model.h"
#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../fx/test/twod_sim.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace harness {

// the copy engine's launches, the 2D engine's scaled blits and its solid rectangles
struct EngineRun {
    uint32_t copies = 0, twod = 0, solids = 0;
};

// FERMI_TWOD_A's solid rectangles as SwitchGM20BEmit2D.c's solid() sets them up (cl902d.h): RECTS, its colour word in the
// destination's own format written as it is under SRCCOPY, or the destination's bits complemented under ROP3 DSTINVERT,
// over [x0, x1) x [y0, y1) inside the clip, at the second point's Y
struct SolidRects {
    std::map<uint32_t, uint32_t> r;
    uint32_t rects = 0;
    std::string error;

    static uint32_t bytesOf(uint32_t format)
    {
        switch (format) {
        case 0xf3:  // Y8
            return 1;
        case 0xee:  // Y16
            return 2;
        case 0xcf:  // A8R8G8B8
        case 0xd0:
        case 0xd1:  // A2B10G10R10
        case 0xd5:  // A8B8G8R8
        case 0xd6:
            return 4;
        default:
            return 0;
        }
    }

    void method(uint32_t mthd, uint32_t value)
    {
        r[mthd] = value;
        if (mthd != NV902D_RENDER_SOLID_PRIM_POINT_Y(1) || r[NV902D_RENDER_SOLID_PRIM_MODE] != 4 /* RECTS */ ||
            r[NV902D_SET_RENDER_ENABLE_C] != NV902D_SET_RENDER_ENABLE_C_MODE_TRUE || !error.empty())
            return;
        const uint32_t format = r[NV902D_SET_DST_FORMAT], bpp = bytesOf(format), op = r[NV902D_SET_OPERATION];
        const bool invert = op == NV902D_SET_OPERATION_V_ROP && r[NV902D_SET_ROP] == 0x55;
        if (!bpp || r[NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT] != format ||
            (op != NV902D_SET_OPERATION_V_SRCCOPY && !invert)) {
            error = "a solid rectangle of a format, colour format or operation the model does not run";
            return;
        }
        const bool pitch = r[NV902D_SET_DST_MEMORY_LAYOUT] == NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH;
        const uint32_t width = r[NV902D_SET_DST_WIDTH], height = r[NV902D_SET_DST_HEIGHT],
                       yLog2 = r[NV902D_SET_DST_BLOCK_SIZE] >> 4 & 7, rowBytes = (width * bpp + 63) & ~63u;
        const uint64_t va = uint64_t(r[NV902D_SET_DST_OFFSET_UPPER] & 0xff) << 32 | r[NV902D_SET_DST_OFFSET_LOWER];
        uint32_t x0 = r[NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0)], y0 = r[NV902D_RENDER_SOLID_PRIM_POINT_Y(0)],
                 x1 = std::min(r[NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1)], width), y1 = std::min(value, height);
        if (r[NV902D_SET_CLIP_ENABLE]) {
            x0 = std::max(x0, r[NV902D_SET_CLIP_X0]), y0 = std::max(y0, r[NV902D_SET_CLIP_Y0]);
            x1 = std::min(x1, r[NV902D_SET_CLIP_X0] + r[NV902D_SET_CLIP_WIDTH]);
            y1 = std::min(y1, r[NV902D_SET_CLIP_Y0] + r[NV902D_SET_CLIP_HEIGHT]);
        }
        rects++;
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = x0; x < x1; x++) {
                const uint64_t at = va + (pitch ? uint64_t(y) * r[NV902D_SET_DST_PITCH] + x * bpp
                                                : tgm_layout_offset_of(rowBytes, uint8_t(yLog2), x * bpp, y));
                uint8_t *texel = kernel_model::model.host(at, bpp);
                if (!texel) {
                    error = "a solid rectangle outside the model's memory";
                    return;
                }
                for (uint32_t b = 0; b < bpp; b++)
                    texel[b] = invert ? uint8_t(~texel[b]) : uint8_t(r[NV902D_SET_RENDER_SOLID_PRIM_COLOR] >> (8 * b));
            }
    }
};

// kinds: each bind of kind 0 or 0xfe mapped cacheable with its kind (gm20b_gpu_memory.h: a GOB the L2 holds keeps the
// kind of its first access), and the host's MEM_OP methods run, so work that moves between kinds of the same pages
// with no kind switch between reads and writes them in the other kind's GOB order. The L2 starts empty at `first` and
// is dropped ahead of each host-written job, as the kernel drops it.
inline void runEngines(size_t first, EngineRun &out, bool kinds = false)
{
    Memory memory;
    for (const auto &[va, bind] : kernel_model::model.binds)
        if (auto *bytes = kernel_model::model.host(va, bind.second)) {
            const uint32_t kind = kernel_model::model.bindKinds[va];
            if (kinds && (kind == 0 || kind == TGM_LAYOUT_KIND))
                memory.map(va, bytes, bind.second, false, true, int(kind));
            else
                memory.map(va, bytes, bind.second, false);
        }
    CopyEngine engine(memory);
    engine.requireNonPipelined = false;
    SolidRects solids;
    twodsim::Sim twod;
    twod.host = [](uint64_t va, uint64_t bytes) {
        return kernel_model::model.host(va, bytes);
    };
    std::vector<uint32_t> init(gm20b::fx::twodInitWords());
    gm20b::Push push = {init.data(), init.data() + init.size()};
    gm20b::fx::emitTwodInit(push);
    ASSERT_TRUE(twod.run(init.data(), size_t(push.at - init.data()))) << twod.error();
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        if (kinds && kernel_model::model.jobs[j].hostWritten)
            memory.invalidateL2();  // the kernel's invalidate ahead of a host-written job (gpu_exec.c chan_exec_body)
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k) {
            if (methods[k].subchannel == gm20b::SUBC_COPY && methods[k].method >= 0x100)
                engine.method(gm20b::SUBC_COPY, methods[k].method, methods[k].value);
            if (kinds && (methods[k].method == 0x0030 || methods[k].method == 0x0034))  // MEM_OP_C, MEM_OP_D
                engine.method(methods[k].subchannel, methods[k].method, methods[k].value);
            if (methods[k].subchannel == gm20b::SUBC_2D)
                solids.method(methods[k].method, methods[k].value);
            ASSERT_TRUE(solids.error.empty()) << solids.error;
            ASSERT_TRUE(twod.feed(methods[k].subchannel, methods[k].method, methods[k].value)) << twod.error();
        }
    }
    ASSERT_FALSE(engine.fault);
    out.copies = engine.launches;
    out.twod = twod.launches;
    out.solids = solids.rects;
}

// `cb` committed and completed on the driver, its engines' work run (runEngines)
inline void commitOnRaw(id<MTLCommandBuffer> cb, EngineRun &out, bool kinds = false)
{
    const size_t first = kernel_model::model.jobs.size();
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
    auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
    ASSERT_TRUE([path isEqualToString:@"gm20b"]) << path.UTF8String;
    runEngines(first, out, kinds);
}

}  // namespace harness
