// SPDX-License-Identifier: MIT
// raw GM20B path: raw_bounds.h against the pixels this Mac's GPU covers. Random draws through Core Animation's vertex
// transform (float4x4 x position), each rasterized alone under a random Metal viewport into a target it clears:
//  - triangles (affine, scaled, rotated, with perspective, partly or wholly off the target): every covered pixel lies in
//    DrawBounds::pixels;
//  - axis-aligned rectangles as a 4-vertex strip or a 6-vertex list, corners on whole, half and arbitrary pixels, split
//    along either diagonal or (strips) folded: DrawBounds::rectangle takes exactly the split ones, every pixel of its
//    `certain` is covered and every covered pixel is in its `possible`.
// usage: boundsoracle [draws]
// build: xcrun clang++ -O2 -std=c++17 -fobjc-arc boundsoracle.mm -framework Foundation -framework Metal -o boundsoracle
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../runtime/raw_bounds.h"

namespace {
constexpr uint32_t kWidth = 320, kHeight = 240;

NSString *const kShaders =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"vertex float4 transform(const device float4 *p [[buffer(0)]], constant float4x4 &m [[buffer(2)]], "
    @"uint v [[vertex_id]]) { return m * p[v]; }\n"
    @"fragment float cover() { return 1.0; }\n";

struct Draw {
    float matrix[16];
    float positions[24];
    uint32_t vertices;
    MTLPrimitiveType type;
    bool split;  // a rectangle drawn as two triangles along a diagonal
    MTLViewport viewport;
};

MTLViewport randomViewport(std::mt19937 &rng)
{
    std::uniform_real_distribution<double> ox(-40, kWidth - 40), oy(-40, kHeight - 40), size(8, 400);
    return {ox(rng), oy(rng), size(rng), size(rng), 0, 1};
}

Draw randomTriangle(std::mt19937 &rng)
{
    std::uniform_real_distribution<float> unit(-1, 1), angle(0, 6.2831853f);
    Draw t = {};
    const float a = angle(rng), s = std::exp2(unit(rng) * 3), sy = std::exp2(unit(rng) * 3);
    const float c = std::cos(a), n = std::sin(a);
    const float m[16] = {c * s,     n * sy,    0, unit(rng) * 0.5f * (rng() % 3 == 0),
                         -n * s,    c * sy,    0, unit(rng) * 0.5f * (rng() % 3 == 0),
                         0,         0,         1, 0,
                         unit(rng), unit(rng), 0, 1};
    memcpy(t.matrix, m, sizeof m);
    for (int v = 0; v < 3; v++) {
        t.positions[v * 4 + 0] = unit(rng) * (rng() % 4 == 0 ? 3 : 1);
        t.positions[v * 4 + 1] = unit(rng) * (rng() % 4 == 0 ? 3 : 1);
        t.positions[v * 4 + 2] = 0.5f;
        t.positions[v * 4 + 3] = 1;
    }
    t.vertices = 3;
    t.type = MTLPrimitiveTypeTriangle;
    t.viewport = randomViewport(rng);
    return t;
}

// A rectangle in pixels through Core Animation's usual matrix (pixels to normalized coordinates, y down), its corners on
// whole, half or arbitrary pixels, as a strip or a list, split along a diagonal or (a strip in perimeter order) folded
Draw randomRectangle(std::mt19937 &rng)
{
    std::uniform_real_distribution<double> at(-30, 330), extent(1, 200);
    Draw d = {};
    auto pixel = [&](double v) {
        switch (rng() % 3) {
        case 0:
            return std::round(v);
        case 1:
            return std::round(v) + 0.5;
        default:
            return v;
        }
    };
    const double x0 = pixel(at(rng)), y0 = pixel(at(rng) * 0.75);
    const double x1 = x0 + std::max(1.0, pixel(extent(rng))), y1 = y0 + std::max(1.0, pixel(extent(rng)));
    d.viewport = {0, 0, double(kWidth), double(kHeight), 0, 1};
    const float m[16] = {2.0f / kWidth, 0, 0, 0, 0, -2.0f / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    memcpy(d.matrix, m, sizeof m);
    const float corner[4][2] = {
        {float(x0), float(y0)}, {float(x1), float(y0)}, {float(x0), float(y1)}, {float(x1), float(y1)}};
    static const int strips[3][4] = {{0, 1, 2, 3}, {1, 3, 0, 2}, {0, 1, 3, 2}};  // the last folded
    static const int lists[2][6] = {{0, 1, 2, 1, 3, 2}, {0, 1, 3, 0, 3, 2}};
    const bool list = rng() % 2;
    const int *order = list ? lists[rng() % 2] : strips[rng() % 3];
    d.vertices = list ? 6 : 4;
    d.type = list ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypeTriangleStrip;
    d.split = list || order != strips[2];
    for (uint32_t v = 0; v < d.vertices; v++) {
        d.positions[v * 4 + 0] = corner[order[v]][0];
        d.positions[v * 4 + 1] = corner[order[v]][1];
        d.positions[v * 4 + 2] = 0.5f;
        d.positions[v * 4 + 3] = 1;
    }
    return d;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        const uint32_t count = argc > 1 ? uint32_t(atoi(argv[1])) : 20000;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kShaders options:nil error:&e];
        if (!lib) {
            fprintf(stderr, "boundsoracle: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"transform"];
        pd.fragmentFunction = [lib newFunctionWithName:@"cover"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:kWidth
                                                                                     height:kHeight
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        std::mt19937 rng(7);
        constexpr uint32_t kBatch = 256;
        id<MTLBuffer> readback = [dev newBufferWithLength:kBatch * kWidth * kHeight
                                                  options:MTLResourceStorageModeShared];
        uint64_t misses = 0, covered = 0, boxed = 0, rejected = 0, rectangles = 0, exact = 0, wrongShape = 0;
        for (uint32_t done = 0; done < count; done += kBatch) {
            std::vector<Draw> batch(kBatch);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            for (uint32_t i = 0; i < kBatch; i++) {
                batch[i] = i % 2 ? randomRectangle(rng) : randomTriangle(rng);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:pass];
                [en setRenderPipelineState:state];
                [en setViewport:batch[i].viewport];
                [en setVertexBytes:batch[i].positions length:sizeof batch[i].positions atIndex:0];
                [en setVertexBytes:batch[i].matrix length:sizeof batch[i].matrix atIndex:2];
                [en drawPrimitives:batch[i].type vertexStart:0 vertexCount:batch[i].vertices];
                [en endEncoding];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:target
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                                    toBuffer:readback
                           destinationOffset:i * kWidth * kHeight
                      destinationBytesPerRow:kWidth
                    destinationBytesPerImage:kWidth * kHeight];
                [bl endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            const uint8_t *px = static_cast<const uint8_t *>(readback.contents);
            auto miss = [&](uint32_t draw, uint32_t x, uint32_t y, const char *what, const raw::PixelBox &b) {
                if (misses++ < 8)
                    printf("miss: draw %u pixel (%u, %u) %s [%u, %u) x [%u, %u)\n", draw, x, y, what, b.x0, b.x1, b.y0,
                           b.y1);
            };
            for (uint32_t i = 0; i < kBatch; i++) {
                const Draw &d = batch[i];
                const raw::WindowViewport vp = {d.viewport.originX, d.viewport.originY, d.viewport.width,
                                                d.viewport.height};
                raw::DrawBounds bounds(d.matrix, vp);
                const raw::PositionStream s = {reinterpret_cast<const uint8_t *>(d.positions), 16, 0, 4};
                bool ok = true;
                for (uint32_t v = 0; v < d.vertices; v++)
                    ok &= bounds.add(s, v);
                if (!ok) {
                    rejected++;
                    continue;
                }
                const raw::PixelBox b = bounds.pixels({0, 0, kWidth, kHeight});
                raw::PixelBox certain, possible;
                const bool rect = bounds.rectangle(d.type == MTLPrimitiveTypeTriangle, certain, possible);
                if (d.vertices > 3) {
                    rectangles++;
                    wrongShape += rect != d.split;
                    exact += rect && certain.x0 == possible.x0 && certain.x1 == possible.x1 &&
                             certain.y0 == possible.y0 && certain.y1 == possible.y1;
                }
                boxed += uint64_t(b.x1 - b.x0) * (b.y1 - b.y0);
                for (uint32_t y = 0; y < kHeight; y++)
                    for (uint32_t x = 0; x < kWidth; x++) {
                        const bool on = px[i * kWidth * kHeight + y * kWidth + x];
                        covered += on;
                        if (on && (x < b.x0 || x >= b.x1 || y < b.y0 || y >= b.y1))
                            miss(done + i, x, y, "outside the box", b);
                        if (rect && on && (x < possible.x0 || x >= possible.x1 || y < possible.y0 || y >= possible.y1))
                            miss(done + i, x, y, "covered outside possible", possible);
                        if (rect && !on && x >= certain.x0 && x < certain.x1 && y >= certain.y0 && y < certain.y1)
                            miss(done + i, x, y, "not covered inside certain", certain);
                    }
            }
        }
        printf(
            "boundsoracle: %u draws, %llu rejected (w), %llu pixels covered, %llu boxed, %llu misses; %llu rectangles, "
            "%llu taken or refused wrongly, %llu exact\n",
            count, (unsigned long long)rejected, (unsigned long long)covered, (unsigned long long)boxed,
            (unsigned long long)misses, (unsigned long long)rectangles, (unsigned long long)wrongShape,
            (unsigned long long)exact);
        misses += wrongShape;
        return misses ? 1 : 0;
    }
}
