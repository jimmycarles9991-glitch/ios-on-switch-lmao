// A pass into textures made without MTLTextureUsageRenderTarget (Metal's validation error), validation off: what an M4 Max
// keeps. usage: nort
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <vector>

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;

static void run(MTLTextureType type, NSUInteger plane)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = type;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.width = d.height = 16;
    d.depth = type == MTLTextureType3D ? 4 : 1;
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    std::vector<uint32_t> fill(16 * 16 * d.depth, 0x11111111);
    [t replaceRegion:MTLRegionMake3D(0, 0, 0, 16, 16, d.depth)
          mipmapLevel:0
                slice:0
            withBytes:fill.data()
          bytesPerRow:64
        bytesPerImage:1024];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].depthPlane = plane;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<uint32_t> back(16 * 16 * d.depth);
    [t getBytes:back.data()
          bytesPerRow:64
        bytesPerImage:1024
           fromRegion:MTLRegionMake3D(0, 0, 0, 16, 16, d.depth)
          mipmapLevel:0
                slice:0];
    printf("%s plane %lu: status %ld, texel 0 of each plane:", type == MTLTextureType3D ? "3D" : "2D",
           (unsigned long)plane, (long)cb.status);
    for (NSUInteger z = 0; z < d.depth; z++)
        printf(" %08x", back[z * 256]);
    printf("\n");
}

int main()
{
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        q = [dev newCommandQueue];
        run(MTLTextureType2D, 0);
        run(MTLTextureType3D, 2);
    }
}
