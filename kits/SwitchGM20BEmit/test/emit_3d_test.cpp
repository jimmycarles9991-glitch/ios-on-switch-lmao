// SPDX-License-Identifier: MIT
// The kit's 3D state sequences and barriers against the raw path's, for the same inputs. raw_emit.cpp's blend
// registers are driven directly. The sequences raw_gpu.cpp wrote inline before it moved onto the kit (and raw_target.h's
// target words) are kept here as replays through raw::Push with raw_gpu.cpp's constants (driver_constants.h reads
// them from its text); raw_gpu.cpp is checked to call the kit where it wrote them.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_emit.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_push.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_target.h"
#include "kits/SwitchGM20B/test/driver_constants.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmit3D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "push_words.h"

namespace {

namespace g = driver::raw_gpu_cpp;
using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;
using switch_gm20b_test::Words;
using testing::HasSubstr;

const std::string &rawGpu()
{
    static const std::string text =
        switch_ios::test::SourceText("../../../drivers/TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
    return text;
}

void expectRawSays(const char *line)
{
    EXPECT_THAT(rawGpu(), HasSubstr(line)) << "raw_gpu.cpp changed: replay its new form here";
}

uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

template <typename F> std::vector<uint32_t> ours(F emit)
{
    std::vector<uint32_t> words(256);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    emit(&p);
    words.resize(nv_push_dw_count(&p));
    return words;
}

template <typename F> std::vector<uint32_t> theirs(F emit)
{
    std::vector<uint32_t> words(256);
    raw::Push p{words.data(), words.data() + words.size()};
    emit(p);
    words.resize(size_t(p.at - words.data()));
    return words;
}

TEST(Emit3D, ViewportAndScissorAsRawWritesThem)
{
    expectRawSays("switch_gm20b_viewport_state v = viewportState(viewport_);\n        turnViewport(rot_, v);\n        "
                  "kit(p, switch_gm20b_viewport, 0u, &v);");
    expectRawSays("kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));");
    const switch_gm20b_viewport_state v = {{640.0f, -360.0f, 1.0f}, {640.0f, 360.0f, 0.0f}, 0, 1280, 0, 720};
    const uint32_t viewport[8] = {bits(640.0f), bits(-360.0f), bits(1.0f),  bits(640.0f),
                                  bits(360.0f), bits(0.0f),    1280u << 16, 720u << 16};
    const uint32_t scissor[2] = {200u << 16 | 10u, 300u << 16 | 20u};
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_viewport(p, 0, &v);
                  switch_gm20b_scissor(p, 0, 10, 200, 20, 300);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.incArray(uint32_t(g::c_VIEWPORT_SCALE_X), viewport, 6);
                  p.inc(uint32_t(g::c_VIEWPORT_CLIP_H), {viewport[6], viewport[7]});
                  p.inc(uint32_t(g::c_SCISSOR_ENABLE), {1, scissor[0], scissor[1]});
              })));
}

TEST(Emit3D, ViewportSwizzleAsRawWritesIt)
{
    expectRawSays("kit(p, switch_gm20b_viewport_swizzle, 0u, word);");
    for (const uint32_t word : {0x6420u, 0x6402u})
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_viewport_swizzle(p, 0, word);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.m(0x0a18, word);
                  })));
}

TEST(Emit3D, BlendConstantAsRawWritesIt)
{
    expectRawSays("p.incArray(BLEND_CONST_RED, blendColor_, 4);");
    const float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const uint32_t words[4] = {bits(0.25f), bits(0.5f), bits(0.75f), bits(1.0f)};
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_blend_constant(p, rgba);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.incArray(uint32_t(g::c_BLEND_CONST_RED), words, 4);
              })));
}

TEST(Emit3D, ClearAsRawWritesIt)
{
    expectRawSays("kit(p, switch_gm20b_clear, &s);");
    expectRawSays("clearRect(p, r.x0, r.y0, r.x1, r.y1, v, c.target, area ? zsBits : 0, layers);");
    for (const bool depth : {false, true}) {
        const switch_gm20b_clear_state c = {
            {0x3f800000, 0, 0x3f000000, 0x3f800000}, 4, 1280, 8, 720, 2, depth, depth, 0, 1, 0};
        const uint32_t zsBits = depth ? 3 : 0;
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_clear(p, &c);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.inc(uint32_t(g::c_COLOR_CLEAR_VALUE), {c.value[0], c.value[1], c.value[2], c.value[3]});
                      p.inc(uint32_t(g::c_CLEAR_RECT_H),
                            {uint32_t(c.xmax) << 16 | c.xmin, uint32_t(c.ymax) << 16 | c.ymin});
                      p.m(uint32_t(g::c_CLEAR_SURFACE_CONTROL), 0x10);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 1);
                      p.m(uint32_t(g::c_CLEAR_SURFACE), 0x3c | c.target << 6 | zsBits);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 0);
                  })));
    }
}

TEST(Emit3D, LayeredClearCallsTheLayerLoop)
{
    for (const bool depth : {false, true}) {
        const switch_gm20b_clear_state c = {
            {0x3f800000, 0, 0x3f000000, 0x3f800000}, 4, 1280, 8, 720, 2, depth, depth, 0, 6, 9};
        const uint32_t zsBits = depth ? 3 : 0;
        EXPECT_EQ(Methods(ours([&](nv_push *p) {
                      switch_gm20b_clear(p, &c);
                  })),
                  Methods(theirs([&](raw::Push &p) {
                      p.inc(uint32_t(g::c_COLOR_CLEAR_VALUE), {c.value[0], c.value[1], c.value[2], c.value[3]});
                      p.inc(uint32_t(g::c_CLEAR_RECT_H),
                            {uint32_t(c.xmax) << 16 | c.xmin, uint32_t(c.ymax) << 16 | c.ymin});
                      p.m(uint32_t(g::c_CLEAR_SURFACE_CONTROL), 0x10);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 1);
                      p.m(0x3800 + 8 * 9, 0x3c | c.target << 6 | zsBits);
                      p.m(0x3800 + 8 * 9 + 4, 6);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 0);
                  })));
    }
}

// raw_target.h's colorTargetWords and disabledTargetWords, the raw path's target words before the kit's
constexpr uint32_t kColorTargetWords = 9;

void handTargetWords(const raw::ColorTargetDesc &d, uint32_t w[kColorTargetWords])
{
    w[0] = uint32_t(d.va >> 32);
    w[1] = uint32_t(d.va);
    w[3] = d.height;
    w[4] = d.rtFormat;
    w[6] = 1;
    w[8] = 0;
    if (!d.blockLinear) {
        w[2] = d.pitch;
        w[5] = 0x1000;
        w[7] = 0;
        return;
    }
    w[2] = d.bpp ? d.pitch / d.bpp : 0;
    w[5] = uint32_t(d.blockHeight & 0xf) << 4;
    w[7] = uint32_t(d.layerBytes >> 2);
}

void handDisabledTargetWords(uint32_t w[kColorTargetWords])
{
    for (uint32_t i = 0; i < kColorTargetWords; i++)
        w[i] = 0;
    w[2] = 64;
    w[6] = 1;
}

TEST(Emit3D, RenderTargetsAsRawWritesThem)
{
    expectRawSays("const switch_gm20b_target k = kitTarget(d);\n    kit(p, switch_gm20b_color_target, index, &k, "
                  "t.comptags);");
    expectRawSays("kit(p, switch_gm20b_color_target, index, &k, true);");
    expectRawSays("kit(p, switch_gm20b_render_area, count, planeArea().x1, planeArea().y1);");
    raw::ColorTargetDesc pitch;
    pitch.va = 0x1234560000;
    pitch.pitch = 5120;
    pitch.height = 720;
    pitch.bpp = 4;
    pitch.rtFormat = NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8;
    raw::ColorTargetDesc level = pitch;
    level.blockLinear = true;
    level.pitch = 2560;
    level.height = 360;
    level.blockHeight = 3;
    level.layerBytes = 0x500000;
    for (uint32_t j : {0u, 3u}) {
        for (const raw::ColorTargetDesc &d : {pitch, level}) {
            const switch_gm20b_target t = raw::kitTarget(d);
            uint32_t w[kColorTargetWords];
            handTargetWords(d, w);
            EXPECT_EQ(Words(ours([&](nv_push *p) {
                          switch_gm20b_color_target(p, j, &t, false);
                      })),
                      Words(theirs([&](raw::Push &p) {
                          p.incArray(uint32_t(g::c_COLOR_TARGET) + 0x40 * j, w, kColorTargetWords);
                          p.m(uint32_t(g::c_COLOR_COMPRESSION) + 4 * j, 0);
                      })));
        }
        switch_gm20b_target off = {};
        off.width = 64;
        off.layers = 1;
        uint32_t w[kColorTargetWords];
        handDisabledTargetWords(w);
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_color_target(p, j, &off, true);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.incArray(uint32_t(g::c_COLOR_TARGET) + 0x40 * j, w, kColorTargetWords);
                      p.m(uint32_t(g::c_COLOR_COMPRESSION) + 4 * j, 1);
                  })));
    }
    for (uint32_t count : {1u, 4u}) {
        uint32_t ct = 0;
        for (uint32_t k = 0; k < 8; k++)
            ct |= k << (4 + 3 * k);
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_render_area(p, count, 1280, 720);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.m(uint32_t(g::c_CT_SELECT), ct | count);
                      p.inc(uint32_t(g::c_SURFACE_CLIP_H), {1280u << 16, 720u << 16});
                  })));
    }
}

TEST(Emit3D, BarriersAsRawWritesThem)
{
    expectRawSays("kit(p_, switch_gm20b_pass_barrier, tiledCache(dev_->k_.info()));");
    expectRawSays("kit(p, switch_gm20b_pass_barrier, false);");
    expectRawSays("kit(p, switch_gm20b_pixel_shader_barrier);\n}");
    for (const bool tiled : {false, true}) {
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_pass_barrier(p, tiled);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      if (tiled)
                          p.m(uint32_t(driver::raw_tiled_cache_h::c_TILED_CACHE_FLUSH), 0);
                      p.m(uint32_t(g::c_WAIT_FOR_IDLE), 0);
                      p.m(uint32_t(g::c_INVALIDATE_SHADER_CACHES),
                          uint32_t(g::c_kFlushData | g::c_kInvalidateData | g::c_kInvalidateConstant));
                      p.m(uint32_t(g::c_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI), 0);
                  })));
    }
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_pixel_shader_barrier(p);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.m(uint32_t(g::c_PIXEL_SHADER_BARRIER), 0);
                  p.m(uint32_t(g::c_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED), 0);
              })));
}

// raw_emit.cpp's blend registers for a key, as (method, value) pairs the hardware reads
std::vector<Method> rawBlend(const std::string &key, uint32_t first, uint32_t last)
{
    std::array<uint8_t, 80> header = {};
    header[72] = 0xff;  // the fragment program writes all four components of targets 0 and 1
    raw::EmitProgram fs = {header.data(), 16, false, 0};
    std::vector<raw::EmitRegister> regs;
    std::string why;
    EXPECT_TRUE(raw::emitState(key, {}, fs, regs, why)) << why;
    std::vector<Method> out;
    for (const raw::EmitRegister &r : regs)
        if (r.method >= first && r.method < last)
            out.push_back({SUBC_NVB197, r.method, r.value});
    return out;
}

TEST(Emit3D, BlendAsRawEmitsIt)
{
    // Target 1 blends source-over (SRC_ALPHA, ONE_MINUS_SRC_ALPHA; ONE, ONE_MINUS_SRC_ALPHA; ADD), target 0 MAX of
    // ONEs; both write all components
    const std::string key = "samples=1 ds=0/0 c0=80:15:1:1:1:4:1:1:4 c1=80:15:1:4:5:0:1:5:0";
    const switch_gm20b_blend_state over = {NVB197_SET_BLEND_PER_TARGET_COLOR_OP_V_OGL_FUNC_ADD,
                                           NVB197_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF_V_OGL_SRC_ALPHA,
                                           NVB197_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF_V_OGL_ONE_MINUS_SRC_ALPHA,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_OP_V_OGL_FUNC_ADD,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF_V_OGL_ONE,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF_V_OGL_ONE_MINUS_SRC_ALPHA};
    const switch_gm20b_blend_state max = {NVB197_SET_BLEND_PER_TARGET_COLOR_OP_V_OGL_MAX,
                                          NVB197_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_OP_V_OGL_MAX,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF_V_OGL_ONE};
    const uint32_t perTarget1 = NVB197_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(1);
    std::vector<Method> raw1 = rawBlend(key, NVB197_SET_BLEND(1), NVB197_SET_BLEND(2));
    std::vector<Method> rawPer1 = rawBlend(key, perTarget1, perTarget1 + 0x20);
    raw1.insert(raw1.end(), rawPer1.begin(), rawPer1.end());
    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_blend(p, 1, &over);
              })),
              raw1);

    // Target 0: raw leaves SEPARATE_FOR_ALPHA(0) to the context's baseline; the kit writes it, as NVK does
    std::vector<Method> raw0 = rawBlend(key, NVB197_SET_BLEND(0), NVB197_SET_BLEND(1));
    std::vector<Method> rawPer0 = rawBlend(key, NVB197_SET_BLEND_PER_TARGET_COLOR_OP(0), perTarget1);
    raw0.push_back({SUBC_NVB197, NVB197_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(0), 1});
    raw0.insert(raw0.end(), rawPer0.begin(), rawPer0.end());
    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_blend(p, 0, &max);
              })),
              raw0);

    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_ct_write(p, 1, 0xf);
              })),
              rawBlend(key, NVB197_SET_CT_WRITE(1), NVB197_SET_CT_WRITE(2)));
}

}  // namespace
