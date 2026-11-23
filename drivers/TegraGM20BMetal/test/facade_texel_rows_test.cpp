// SPDX-License-Identifier: MIT
// A texel buffer over a surface the kernel laid out block-linear for a target made over it first, on the facade over
// the kernel model: Metal makes it, and the driver holds it in a pool of the surface's bytes in order (texel_rows.h).
// The copy engine fills the pool from the surface's rows before a pass that samples it or renders into it, a blit
// command and a kernel's encoder, and copies it back into the rows after each that writes it; none of it opens m2v.
#include "facade_compute.h"
#include "../texel_rows.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

@interface NSObject (SwitchTexelRowsFill)
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color;
@end

namespace {

using namespace harness;

// a 64x16 BGRA8 surface of rows 320 bytes apart; the buffer's 80 texels are row 0, its 64 texels and 64 bytes of padding, the
// longest Metal makes over a surface of no planes
constexpr uint32_t kWidth = 64, kHeight = 16, kRows = 320, kElements = 80;
constexpr uint32_t kCopy = 4, kCompute = 1;
constexpr uint32_t kLaunchDma = 0x0300, kSendPcasA = 0x02b4;  // NVB0B5_LAUNCH_DMA, NVB1C0_SEND_PCAS_A
const uint32_t kThreads[3] = {2, 2, 1};

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState;
id<MTLRenderPipelineState> gDraw;

// The kernel's record: texture 0, a BGRA8 texture buffer, stored
gm20b::PackKernel storeRecord(uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed));
    gm20b::BoundTexture bound[1] = {{0, MTLPixelFormatBGRA8Unorm, MTLTextureTypeTextureBuffer, 1, 1, {2, 3, 4, 5}}};
    const std::string key =
        gm20b::computeKey(air, "", maxThreads, false, kThreads, "", gm20b::computeTextures(bound, 1), "");
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(gm20b::KernelTexture), 0, 0};
    return k;
}

// The copy engine's copies out of the block-linear image at `surface` (F) and into it (D), and the compute launches (L), of
// every job submitted from `first` on, in order
std::string moves(size_t first, uint64_t surface)
{
    constexpr uint32_t kOffsetInUpper = 0x400,
                       kOffsetOutUpper = 0x408;  // NVB0B5_OFFSET_IN_UPPER, NVB0B5_OFFSET_OUT_UPPER
    std::string out;
    uint32_t offsets[4] = {};
    const auto at = [&](uint32_t upper) {
        return uint64_t(offsets[upper]) << 32 | offsets[upper + 1];
    };
    const auto launch = [&](uint32_t value) {
        if (!(value & 3))
            return;
        if (!(value >> 7 & 1) && at(0) == surface)
            out += 'F';
        else if (!(value >> 8 & 1) && at(2) == surface)
            out += 'D';
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, sub = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4) {  // immediate data (the copy engine's launches among them): no payload
                if (sub == kCopy && method == kLaunchDma)
                    launch(count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i) {
                const uint32_t m = op == 1 ? method + 4 * k : op == 5 ? method + (k ? 4 : 0) : method;
                if (sub == kCopy && m == kLaunchDma)
                    launch(w[i]);
                else if (sub == kCopy && m >= kOffsetInUpper && m <= kOffsetOutUpper + 4)
                    offsets[(m - kOffsetInUpper) / 4] = w[i];
                else if (sub == kCompute && m == kSendPcasA)
                    out += 'L';
            }
        }
    }
    return out;
}

class FacadeTexelRows : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        kernel_model::model.infoFlags |= TegraGM20BInfoSurfaceLayout;
        kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {storeRecord(maxThreads)},
                                 {kernelFacts(0, maxThreads)}, true, "", {}, {sampled}));
        ASSERT_TRUE(gDevice != nil);
        metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library newFunctionWithName:@(kernelName(0).c_str())];
        gState =
            [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        ASSERT_TRUE(gState != nil) << (error.description.UTF8String ?: "");
        id<MTLLibrary> shaders =
            [gDevice newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
        ASSERT_TRUE(shaders != nil) << (error.description.UTF8String ?: "");
        gDraw =
            [gDevice newRenderPipelineStateWithDescriptor:descriptor(shaders, MTLPixelFormatBGRA8Unorm) error:&error];
        ASSERT_TRUE(gDraw != nil) << (error.description.UTF8String ?: "");
    }

    void SetUp() override
    {
        surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @(kWidth),
            (id)kIOSurfaceHeight : @(kHeight),
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfaceBytesPerRow : @(kRows),
            (id)kIOSurfaceAllocSize : @(kRows * kHeight),
            (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
        });
        ASSERT_TRUE(surface != nullptr);
        ASSERT_EQ(IOSurfaceGetBytesPerRow(surface), kRows);
        MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:kWidth
                                                                                    height:kHeight
                                                                                 mipmapped:NO];
        t.storageMode = MTLStorageModeShared;
        t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        target = (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:t iosurface:surface plane:0];
        ASSERT_TRUE(target != nil && target->_gm20bOk && target->_raw.blockLinear)
            << "the kernel tiled it for the target";
        MTLTextureDescriptor *b = [MTLTextureDescriptor
            textureBufferDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                             width:kElements
                                   resourceOptions:MTLResourceStorageModeShared
                                             usage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                                                   MTLTextureUsageShaderWrite];
        buffer = (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:b iosurface:surface plane:0];
        ASSERT_TRUE(buffer != nil && buffer->_gm20bOk) << "Metal makes it";
        ASSERT_NE(buffer->_raw.texelRows, nullptr);
        EXPECT_EQ(buffer->_raw.texelRows->import, target->_raw.import) << "the surface's rows";
        const uint32_t *h = gRM->raw->headerWords(buffer->_raw.tic);
        EXPECT_EQ(h[1] | uint64_t(h[2] & 0xffff) << 32, buffer->_raw.va) << "a texel buffer header over its pool";
    }

    void TearDown() override
    {
        buffer = nil;
        target = nil;
        if (surface)
            CFRelease(surface);
    }

    // the surface's block-linear image, which the pool's copies read and write
    uint64_t surfaceVa() const
    {
        return buffer->_raw.texelRows->import->bo.va;
    }

    IOSurfaceRef surface = nullptr;
    SwitchMetalTexture *target = nil, *buffer = nil;
};

TEST_F(FacadeTexelRows, APassRendersItsPoolFilledFromTheRowsAndCopiedBack)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)buffer;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(gm20bPath(cb), "gm20b");
    EXPECT_EQ(moves(jobs, surfaceVa()), "FD") << "row 0's texels each way";
}

// A draw samples its pool, filled before the pass: other textures over the surface may have written its rows
TEST_F(FacadeTexelRows, ADrawSamplesItsPoolFilledFromTheRows)
{
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLSamplerState> sampler = [gDevice newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    id<MTLCommandBuffer> cb = drawSampling(gDevice, gDraw, (id<MTLTexture>)buffer, sampler);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(gm20bPath(cb), "gm20b");
    EXPECT_EQ(moves(jobs, surfaceVa()), "F") << "nothing copied back: no draw stores to it";
}

// A blit command reaches the pool (its image): fillTexture: (Apple SPI) of texels 10 to 29
TEST_F(FacadeTexelRows, ABlitFillsItsPoolFilledFromTheRowsAndCopiedBack)
{
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [(id)e fillTexture:(id<MTLTexture>)buffer
                 level:0
                 slice:0
                region:MTLRegionMake1D(10, 20)
                 color:MTLClearColorMake(0, 1, 0, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(gm20bPath(cb), "gm20b");
    EXPECT_EQ(moves(jobs, surfaceVa()), "FD") << "filled before the command, copied back after it";
}

TEST_F(FacadeTexelRows, AKernelStoresItsPoolFilledFromTheRowsAndCopiedBack)
{
    const auto dispatch = [&] {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        [e setTexture:(id<MTLTexture>)buffer atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(4, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(kThreads[0], kThreads[1], kThreads[2])];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return cb;
    };
    // the first dispatch finds the state's kernel in the pack off the encoding thread; the second has it
    dispatch();
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = dispatch();
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(gm20bPath(cb), "gm20b");
    EXPECT_EQ(moves(jobs, surfaceVa()), "FLD") << "filled as its encoder begins, copied back as it ends";
}

// One texel past the surface's rows: an M4 Max's Metal asserts, validation off too ("bytesPerRow (320) must be greater or
// equal to (324) bytes"), so the driver makes none and logs Metal's stop (gm20b:tex.metal)
TEST_F(FacadeTexelRows, ATexelBufferPastTheSurfacesRowsIsNil)
{
    MTLTextureDescriptor *b = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:kElements + 1
                                                                           resourceOptions:MTLResourceStorageModeShared
                                                                                     usage:MTLTextureUsageShaderRead];
    EXPECT_EQ([gDevice newTextureWithDescriptor:b iosurface:surface plane:0], nil);
}

// The surface's pages hold the target's texels in GOBs: a no-copy buffer over them, or over a page of them, would read
// and write those as rows, so none is made; one over pages of no such surface is
TEST_F(FacadeTexelRows, ANoCopyBufferOverTheSurfacesPagesIsNil)
{
    ASSERT_TRUE(target->_raw.import && target->_raw.import->kernelTiled());
    uint8_t *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(surface));
    EXPECT_EQ([gDevice newBufferWithBytesNoCopy:base
                                         length:IOSurfaceGetAllocSize(surface)
                                        options:MTLResourceStorageModeShared
                                    deallocator:nil],
              nil);
    EXPECT_EQ(
        [gDevice newBufferWithBytesNoCopy:base + 4096 length:4096 options:MTLResourceStorageModeShared deallocator:nil],
        nil)
        << "a page inside the surface";
    void *pages = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    ASSERT_NE(pages, MAP_FAILED);
    EXPECT_NE(
        [gDevice newBufferWithBytesNoCopy:pages length:16384 options:MTLResourceStorageModeShared deallocator:nil],
        nil);
}

// Last, as it refuses submissions. The target's texels reach a CPU reader through the copy engine alone: where its
// transfer fails nothing moves, since the task's mapping of the surface holds rows while a CPU lock is held
TEST_F(FacadeTexelRows, ATargetsReadTheCopyEngineFailedMovesNothing)
{
    const MTLRegion all = MTLRegionMake2D(0, 0, kWidth, kHeight);
    std::vector<uint8_t> bytes(kWidth * 4 * kHeight, 0x5a);
    [(id<MTLTexture>)target getBytes:bytes.data() bytesPerRow:kWidth * 4 fromRegion:all mipmapLevel:0];
    std::fill(bytes.begin(), bytes.end(), uint8_t(0x5a));
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.refuse = 1000;
    }
    [(id<MTLTexture>)target getBytes:bytes.data() bytesPerRow:kWidth * 4 fromRegion:all mipmapLevel:0];
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.refuse = 0;
    }
    EXPECT_EQ(bytes, std::vector<uint8_t>(bytes.size(), 0x5a)) << "the CPU detiled the task's mapping";
}

}  // namespace
