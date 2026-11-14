// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_shadow.h over a byte model of the GPU: textures whose rows are 16 bytes off 32 are read and
// written through 32-aligned images, the copy engine moving the texels at the steps the planner gives, and every
// surface byte ends as a kernel reaching the rows in place leaves it (padding untouched, a CPU write between two
// command buffers seen by the second). mediaanalysisd's shape on boot 21: Core Image's kernel reads an RGBA8 texture
// (slot 1) and writes the luma (R8) and chroma (RG8) planes of one 4:2:0 surface, every row 16 bytes off 32.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../raw/runtime/raw_kernel_shadow.h"

namespace {

using Bytes = std::vector<uint8_t>;

struct Rows {
    Bytes *memory;
    uint64_t offset;
    uint32_t width, height, bpp, pitch;

    uint8_t *texel(uint32_t x, uint32_t y) const
    {
        return memory->data() + offset + uint64_t(y) * pitch + uint64_t(x) * bpp;
    }

    raw::ByteSpan span(uint64_t base) const
    {
        return {base + offset, uint64_t(pitch) * (height - 1) + uint64_t(width) * bpp};
    }
};

// A texture: its rows in a surface, and the image of them a kernel reaches when it is shadowed
struct Texture {
    uint64_t base;  // the surface's GPU address
    Rows rows;
    Bytes imageBytes;
    Rows image;
    bool shadowed;

    Texture(uint64_t surfaceVa, Bytes &surface, uint64_t offset, uint32_t width, uint32_t height, uint32_t bpp,
            uint32_t pitch)
        : base(surfaceVa), rows{&surface, offset, width, height, bpp, pitch},
          imageBytes(size_t((width * bpp + 31) & ~31u) * height, 0xee),
          image{&imageBytes, 0, width, height, bpp, (width * bpp + 31) & ~31u}, shadowed(pitch & 31)
    {}

    const Rows &reached() const
    {
        return shadowed ? image : rows;
    }
};

void copyTexels(const Rows &from, const Rows &to)
{
    for (uint32_t y = 0; y < from.height; ++y)
        memcpy(to.texel(0, y), from.texel(0, y), size_t(from.width) * from.bpp);
}

constexpr uint64_t kSourceVa = 0x10000000, kPlanesVa = 0x20000000;

raw::KernelUse use(const Texture &t, bool writes)
{
    return {&t, t.rows.span(t.base), t.shadowed, writes};
}

void steps(const raw::KernelShadows::Steps &s)
{
    for (const void *o : s.copyBack)
        copyTexels(static_cast<const Texture *>(o)->image, static_cast<const Texture *>(o)->rows);
    for (const void *o : s.fill)
        copyTexels(static_cast<const Texture *>(o)->rows, static_cast<const Texture *>(o)->image);
}

// Core Image's conversion: each luma texel from its RGBA texel, each chroma pair from the 2x2 block's first
void convert(const Rows &rgba, const Rows &luma, const Rows &chroma)
{
    for (uint32_t y = 0; y < luma.height; ++y)
        for (uint32_t x = 0; x < luma.width; ++x) {
            const uint8_t *p = rgba.texel(x, y);
            *luma.texel(x, y) = uint8_t((p[0] + 2 * p[1] + p[2]) / 4);
            if (!(x & 1) && !(y & 1) && x / 2 < chroma.width && y / 2 < chroma.height) {
                chroma.texel(x / 2, y / 2)[0] = uint8_t(p[2] ^ 0x80);
                chroma.texel(x / 2, y / 2)[1] = uint8_t(p[0] ^ 0x80);
            }
        }
}

struct Frame {
    static constexpr uint32_t kWidth = 332, kHeight = 78, kRgbaPitch = 1328, kPlanePitch = 336;
    Bytes source = Bytes(size_t(kRgbaPitch) * kHeight), planes = Bytes(size_t(kPlanePitch) * (kHeight + kHeight / 2));
    Texture rgba{kSourceVa, source, 0, kWidth, kHeight, 4, kRgbaPitch};
    Texture luma{kPlanesVa, planes, 0, kWidth, kHeight, 1, kPlanePitch};
    Texture chroma{kPlanesVa, planes, uint64_t(kPlanePitch) * kHeight, kWidth / 2, kHeight / 2, 2, kPlanePitch};

    Frame()
    {
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = uint8_t(i * 37 + 11);
        memset(planes.data(), 0x5a, planes.size());
    }
};

TEST(KernelShadow, MediaanalysisdsConversionReachesTheSurfaceAsInPlace)
{
    Frame shadowed, direct;
    ASSERT_TRUE(shadowed.rgba.shadowed && shadowed.luma.shadowed && shadowed.chroma.shadowed);
    convert(direct.rgba.rows, direct.luma.rows, direct.chroma.rows);

    const raw::KernelUse uses[] = {use(shadowed.luma, true), use(shadowed.rgba, false), use(shadowed.chroma, true)};
    raw::KernelShadows plan;
    plan.begin(uses, 3);
    for (const auto &u : uses)
        copyTexels(static_cast<const Texture *>(u.object)->rows, static_cast<const Texture *>(u.object)->image);
    const auto &before = plan.before(uses, 3);
    EXPECT_TRUE(before.empty());
    steps(before);
    convert(shadowed.rgba.reached(), shadowed.luma.reached(), shadowed.chroma.reached());
    plan.after(uses, 3);
    const auto back = plan.end();
    EXPECT_EQ(back.size(), 2u);
    for (const void *o : back)
        copyTexels(static_cast<const Texture *>(o)->image, static_cast<const Texture *>(o)->rows);
    EXPECT_EQ(shadowed.planes, direct.planes) << "every texel and every padding byte as the in-place run left it";
    EXPECT_EQ(shadowed.source, direct.source);
}

TEST(KernelShadow, WithoutTheCopyBackTheSurfaceNeverSeesTheKernel)
{
    Frame f;
    const Bytes before = f.planes;
    convert(f.rgba.reached(), f.luma.reached(), f.chroma.reached());
    EXPECT_EQ(f.planes, before) << "the kernel's stores land in the images alone";
}

TEST(KernelShadow, ACpuWriteBetweenTwoCommandBuffersIsSeenByTheSecond)
{
    Frame f, direct;
    raw::KernelShadows plan;
    const raw::KernelUse uses[] = {use(f.luma, true), use(f.rgba, false), use(f.chroma, true)};
    for (int buffer = 0; buffer < 2; ++buffer) {
        if (buffer) {
            f.source[5] ^= 0xff;
            direct.source[5] ^= 0xff;
        }
        plan.begin(uses, 3);
        for (const auto &u : uses)
            copyTexels(static_cast<const Texture *>(u.object)->rows, static_cast<const Texture *>(u.object)->image);
        steps(plan.before(uses, 3));
        convert(f.rgba.reached(), f.luma.reached(), f.chroma.reached());
        plan.after(uses, 3);
        for (const void *o : plan.end())
            copyTexels(static_cast<const Texture *>(o)->image, static_cast<const Texture *>(o)->rows);
        convert(direct.rgba.rows, direct.luma.rows, direct.chroma.rows);
    }
    EXPECT_EQ(f.planes, direct.planes);
}

TEST(KernelShadow, AnotherResourceOverTheBytesSeesEarlierStores)
{
    Frame f;
    const raw::KernelUse first[] = {use(f.luma, true), use(f.rgba, false), use(f.chroma, true)};
    const raw::KernelUse buffer = {f.planes.data(), {kPlanesVa, f.planes.size()}, false, false};
    raw::KernelShadows plan;
    plan.begin(first, 3);
    EXPECT_TRUE(plan.before(first, 3).empty());
    plan.after(first, 3);
    const auto &steps2 = plan.before(&buffer, 1);
    EXPECT_EQ(steps2.copyBack.size(), 2u) << "both planes' images back before a buffer over the surface reads it";
    EXPECT_TRUE(plan.end().empty());
}

TEST(KernelShadow, AnImageIsFilledAgainAfterAnotherResourceWroteItsRows)
{
    Frame f;
    const raw::KernelUse read[] = {use(f.rgba, false)};
    const raw::KernelUse write = {f.source.data(), {kSourceVa, f.source.size()}, false, true};
    raw::KernelShadows plan;
    plan.begin(read, 1);
    EXPECT_TRUE(plan.before(read, 1).empty());
    plan.after(read, 1);
    EXPECT_TRUE(plan.before(&write, 1).empty());
    plan.after(&write, 1);
    const auto &again = plan.before(read, 1);
    ASSERT_EQ(again.fill.size(), 1u);
    EXPECT_EQ(again.fill[0], &f.rgba);
    plan.after(read, 1);
    EXPECT_TRUE(plan.before(read, 1).empty()) << "filled once per write, not before every dispatch";
}

TEST(KernelShadow, TwoStoresToTheSameBytesInOneDispatchLeaveTheImages)
{
    Frame f;
    const raw::KernelUse buffer = {f.planes.data(), {kPlanesVa, f.planes.size()}, false, true};
    const raw::KernelUse stores[] = {use(f.luma, true), buffer};
    const raw::KernelUse later[] = {{buffer.object, buffer.span, false, false}, use(f.chroma, false)};
    raw::KernelShadows plan;
    plan.begin(stores, 2);
    EXPECT_TRUE(plan.before(stores, 2).empty());
    plan.after(stores, 2);
    const auto &next = plan.before(later, 2);
    ASSERT_EQ(next.copyBack.size(), 1u) << "the luma image goes back after the buffer's stores, so its values stand";
    EXPECT_EQ(next.copyBack[0], &f.luma);
    ASSERT_EQ(next.fill.size(), 1u) << "the chroma image is filled again over the bytes the buffer wrote";
    EXPECT_EQ(next.fill[0], &f.chroma);
}

}  // namespace
