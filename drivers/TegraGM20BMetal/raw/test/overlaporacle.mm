// SPDX-License-Identifier: MIT
// raw GM20B path: raw_overlap.h's runs against the pixels this Mac's GPU covers. Random meshes through Core Animation's
// vertex transform (nine-part grids at whole, half and arbitrary pixels, axis-aligned T-junctions, fans, overlapping and
// folded quads, rotated grids), each triangle drawn with additive blending into a target cleared per run: no pixel of
// a run is covered twice. Prints the meshes, their runs and the pixels covered twice within a run (none passes).
// usage: overlaporacle [meshes]
// build: xcrun clang++ -O2 -std=c++17 -fobjc-arc overlaporacle.mm -framework Foundation -framework Metal -o overlaporacle
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

#include "../runtime/raw_overlap.h"

namespace {
constexpr uint32_t kWidth = 320, kHeight = 240;

NSString *const kShaders =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"vertex float4 transform(const device float4 *p [[buffer(0)]], const device ushort *index [[buffer(1)]], "
    @"constant float4x4 &m [[buffer(2)]], uint v [[vertex_id]]) { return m * p[index[v]]; }\n"
    @"fragment float cover() { return 1.0 / 255.0; }\n";

struct Mesh {
    float matrix[16];
    std::vector<float> positions;
    std::vector<uint16_t> indices;  // a triangle list

    uint16_t vertex(float x, float y)
    {
        positions.insert(positions.end(), {x, y, 0.5f, 1});
        return uint16_t(positions.size() / 4 - 1);
    }

    void quad(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
    {
        indices.insert(indices.end(), {a, b, c, b, d, c});
    }
};

float pixel(std::mt19937 &rng, float v)
{
    switch (rng() % 3) {
    case 0:
        return std::round(v);
    case 1:
        return std::round(v) + 0.5f;
    default:
        return v;
    }
}

Mesh randomMesh(std::mt19937 &rng)
{
    std::uniform_real_distribution<float> at(-20, 300), extent(1, 80);
    Mesh m;
    float pixels[16] = {2.0f / kWidth, 0, 0, 0, 0, -2.0f / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    if (rng() % 4 == 0) {  // rotated about the target's centre
        const float a = std::uniform_real_distribution<float>(0, 6.2831853f)(rng), c = std::cos(a), s = std::sin(a);
        const float r[16] = {
            c * 2.0f / kWidth, -s * 2.0f / kHeight, 0, 0, s * 2.0f / kWidth, c * -2.0f / kHeight, 0, 0, 0, 0, 1, 0,
            -c + s * 0.75f,    s + c * 0.75f,       0, 1};
        memcpy(pixels, r, sizeof pixels);
    }
    memcpy(m.matrix, pixels, sizeof pixels);
    switch (rng() % 4) {
    case 0: {  // a nine-part grid
        float xs[4], ys[4];
        xs[0] = pixel(rng, at(rng));
        ys[0] = pixel(rng, at(rng) * 0.75f);
        for (int i = 1; i < 4; i++) {
            xs[i] = xs[i - 1] + std::max(1.0f, pixel(rng, extent(rng)));
            ys[i] = ys[i - 1] + std::max(1.0f, pixel(rng, extent(rng)));
        }
        uint16_t v[4][4];
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                v[j][i] = m.vertex(xs[i], ys[j]);
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                m.quad(v[j][i], v[j][i + 1], v[j + 1][i], v[j + 1][i + 1]);
        break;
    }
    case 1: {  // an axis-aligned T-junction: a quad beside two stacked quads
        const float x0 = pixel(rng, at(rng)), y0 = pixel(rng, at(rng) * 0.75f), w = pixel(rng, extent(rng)) + 1,
                    h = pixel(rng, extent(rng)) + 2, half = pixel(rng, h / 2);
        m.quad(m.vertex(x0, y0), m.vertex(x0 + w, y0), m.vertex(x0, y0 + h), m.vertex(x0 + w, y0 + h));
        m.quad(m.vertex(x0 + w, y0), m.vertex(x0 + 2 * w, y0), m.vertex(x0 + w, y0 + half),
               m.vertex(x0 + 2 * w, y0 + half));
        m.quad(m.vertex(x0 + w, y0 + half), m.vertex(x0 + 2 * w, y0 + half), m.vertex(x0 + w, y0 + h),
               m.vertex(x0 + 2 * w, y0 + h));
        break;
    }
    case 2: {  // a fan around a centre
        const float cx = at(rng), cy = at(rng) * 0.75f, r = extent(rng);
        const uint16_t c = m.vertex(cx, cy);
        const int n = 3 + int(rng() % 8);
        std::vector<uint16_t> rim;
        for (int i = 0; i < n; i++)
            rim.push_back(m.vertex(cx + r * std::cos(6.2831853f * i / n), cy + r * std::sin(6.2831853f * i / n)));
        for (int i = 0; i < n; i++)
            m.indices.insert(m.indices.end(), {c, rim[i], rim[(i + 1) % n]});
        break;
    }
    default: {  // random quads, overlapping or not
        const int n = 2 + int(rng() % 4);
        for (int i = 0; i < n; i++) {
            const float x0 = pixel(rng, at(rng)), y0 = pixel(rng, at(rng) * 0.75f), w = pixel(rng, extent(rng)) + 1,
                        h = pixel(rng, extent(rng)) + 1;
            m.quad(m.vertex(x0, y0), m.vertex(x0 + w, y0), m.vertex(x0, y0 + h), m.vertex(x0 + w, y0 + h));
        }
        break;
    }
    }
    return m;
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        const uint32_t count = argc > 1 ? uint32_t(atoi(argv[1])) : 4000;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kShaders options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"transform"];
        pd.fragmentFunction = [lib newFunctionWithName:@"cover"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:kWidth
                                                                                     height:kHeight
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> readback = [dev newBufferWithLength:kWidth * kHeight options:MTLResourceStorageModeShared];
        std::mt19937 rng(11);
        uint64_t twice = 0, runs = 0, split = 0, unbounded = 0;
        for (uint32_t n = 0; n < count; n++) {
            Mesh m = randomMesh(rng);
            raw::DrawGeometry g;
            g.bounded = true;
            g.matrix = reinterpret_cast<const uint8_t *>(m.matrix);
            g.positions = {reinterpret_cast<const uint8_t *>(m.positions.data()), 16, 0, 4};
            g.positionBytes = m.positions.size() * sizeof(float);
            g.indices = reinterpret_cast<const uint8_t *>(m.indices.data());
            g.indexSize = 2;
            g.count = uint32_t(m.indices.size());
            g.viewport = {0, 0, kWidth, kHeight};
            g.limit = {0, 0, kWidth, kHeight};
            raw::TriangleRuns t;
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            if (!t.split(g, false, ranges)) {
                unbounded++;
                continue;
            }
            if (ranges.empty())
                ranges.push_back({0, g.count});
            else
                split++;
            for (const auto &[begin, end] : ranges) {
                runs++;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:pass];
                [en setRenderPipelineState:state];
                [en setVertexBytes:m.positions.data() length:m.positions.size() * sizeof(float) atIndex:0];
                [en setVertexBytes:m.indices.data() + begin length:(end - begin) * sizeof(uint16_t) atIndex:1];
                [en setVertexBytes:m.matrix length:sizeof m.matrix atIndex:2];
                [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:end - begin];
                [en endEncoding];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:target
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                                    toBuffer:readback
                           destinationOffset:0
                      destinationBytesPerRow:kWidth
                    destinationBytesPerImage:kWidth * kHeight];
                [bl endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const uint8_t *px = static_cast<const uint8_t *>(readback.contents);
                for (uint32_t i = 0; i < kWidth * kHeight; i++)
                    if (px[i] > 1 && twice++ < 8)
                        printf("mesh %u run [%u, %u): pixel (%u, %u) covered %u times\n", n, begin, end, i % kWidth,
                               i / kWidth, px[i]);
            }
        }
        printf("overlaporacle: %u meshes, %llu split, %llu unbounded, %llu runs, %llu pixels covered twice in a run\n",
               count, (unsigned long long)split, (unsigned long long)unbounded, (unsigned long long)runs,
               (unsigned long long)twice);
        return twice ? 1 : 0;
    }
}
