// SPDX-License-Identifier: MIT
// Extended-range textures a pass's draws write or sample, on the driver with nothing of it on m2v. A pass whose draws write
// more two-plane RGB10A8_2P_XR10 textures than gm20b::PassDesc holds inline (each draw writes 16 slots and different draws
// different textures) takes an image for every one and renders, no image's backing tag one another kind of scratch of the
// pass holds. A texel buffer a draw samples is converted into its stand-in first: its codes copied in order into the
// stand-in's rows by the copy engine, then the conversion's pass (TegraGM20BMetalRangeStep.mm standIns).
#include "facade_pass_scene.h"
#import "../TegraGM20BMetalXrPlanes.h"
#include "../attachment_target.h"
#include "../format_standin.h"
#include "../push.h"

#include <set>

namespace {

using namespace pass_scene;

void writesEverySlot(const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    gm20b::setEntryCount(pp, gm20b::kWrittenSlots);
    for (uint32_t slot = 0; slot < gm20b::kWrittenSlots; slot++) {
        pp.entry[slot] = {uint16_t(16 * slot), gm20b::EntryTexture, 2, uint8_t(slot), gm20b::SamplerNone, 0, 0};
        pp.writtenFormat[slot] = 550;
    }
    pp.setBytes = 16 * gm20b::kWrittenSlots;
    pp.memoryEntries = (1u << gm20b::kWrittenSlots) - 1;
    pp.xrSlots = request.variant.xrSlots;
}

// A b3a8 surface of 64x64: a plane of words and one of alpha bytes, each plane's rows `pad` bytes past its texels
IOSurfaceRef twoPlaneSurface(unsigned pad = 0)
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(64 * bytes + pad),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 64 * (64 * 4 + pad)),
            (id)kIOSurfacePlaneSize : @(64 * (64 * bytes + pad))
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(64 * (64 * 5 + 2 * pad)),
        (id)kIOSurfacePlaneInfo : planes
    });
}

// A 64x64 RGB10A8_2P_XR10 texture over the two planes of a surface of its own (twoPlaneSurface)
id<MTLTexture> twoPlaneTexture(id<MTLDevice> device, unsigned pad = 0)
{
    IOSurfaceRef surface = twoPlaneSurface(pad);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> t = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
    if (surface)
        CFRelease(surface);
    return t;
}

// `count` two-plane textures written in one pass: draws of 16 written slots each, the last one's first slots the rest
void twoPlaneTexturesWrittenInOnePass(uint32_t count)
{
    RenderCompiler compiler;
    compiler.shapes = writesEverySlot;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "xr-images",
                                    writePack(packKey(MTLPixelFormatRGBA16Float), {}, {}, true, "",
                                              {{"raw.xr.xr_planes_merge_frag", MTLPixelFormatRG32Uint},
                                               {"raw.xr.xr_planes_split_frag", MTLPixelFormatR32Uint}}),
                                    device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil && ((SwitchMetalPipeline *)state)->_raw);
    ASSERT_EQ(((SwitchMetalPipeline *)state)->_raw->writtenSlots, 0xffff);
    std::vector<id<MTLTexture>> written;
    for (uint32_t k = 0; k < count; k++) {
        written.push_back(twoPlaneTexture(device));
        ASSERT_TRUE(written.back() != nil) << k;
    }
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64);
    ASSERT_TRUE(colour != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = colour;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    // The variant reading the 16 slots in their planes is asked once, at the first bind of all of them (a later group
    // leaves every slot holding a two-plane texture: the same forms), and taken from the first draw on. Bound one by one,
    // each bind would ask the variant of the slots bound so far, a burst the harness's compiler does not answer.
    gm20b::RenderVariant pairs;
    pairs.xrSlots = 0xffff;
    __block bool asked = false, readied = false;
    id<MTLCommandBuffer> cb = run(device, p, ^(id<MTLRenderCommandEncoder> e) {
        [e setRenderPipelineState:state];
        for (size_t first = 0; first < written.size(); first += gm20b::kWrittenSlots) {
            const size_t n = std::min<size_t>(gm20b::kWrittenSlots, written.size() - first);
            id<MTLTexture> __unsafe_unretained group[gm20b::kWrittenSlots];
            for (size_t k = 0; k < n; k++)
                group[k] = written[first + k];
            [e setFragmentTextures:group withRange:NSMakeRange(0, n)];
            if (!first) {
                gm20b::Variant<gm20b::Pipeline> *variant = variantOf(state, pairs);
                asked = variant != nullptr;
                readied = asked && ready(*variant);
            }
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    });
    EXPECT_TRUE(asked);
    EXPECT_TRUE(readied) << "the draws read the textures in their planes, through no stand-in";
    expectRaw(device, cb);
}

void samplesTextureZero(const gm20b::CompileRequest &, gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerNone, 0, 0};
    pp.setBytes = 16;
    pp.memoryEntries = 1;
}

// A copy engine launch's destination, or a pass's colour target 0, in the order the jobs from `first` on pushed them
struct Event {
    bool copy;
    uint64_t address;
};

std::vector<Event> events(size_t first)
{
    constexpr uint32_t kTargetA = 0x0800, kTargetB = 0x0804, kTargetFormat = 0x0810, kOutUpper = 0x0408,
                       kOutLower = 0x040c, kLaunchDma = 0x0300;
    std::vector<Event> out;
    uint64_t target = 0, to = 0;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_3D && m == kTargetA)
            target = (target & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_3D && m == kTargetB)
            target = (target & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_3D && m == kTargetFormat && v)
            out.push_back({false, target});
        else if (subc == gm20b::SUBC_COPY && m == kOutUpper)
            to = (to & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_COPY && m == kOutLower)
            to = (to & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_COPY && m == kLaunchDma)
            out.push_back({true, to});
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

size_t indexOf(const std::vector<Event> &e, bool copy, uint64_t address)
{
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i].copy == copy && e[i].address == address)
            return i;
    return SIZE_MAX;
}

void aSampledTexelBufferIsConvertedFromItsCodes()
{
    RenderCompiler compiler;
    compiler.shapes = samplesTextureZero;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "xr-texel-buffer",
                                    writePack(packKey(MTLPixelFormatRGBA16Float), {}, {}, true, "",
                                              {{gm20b::xrConversionOf(552), MTLPixelFormatRGBA16Float}}),
                                    device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil && ((SwitchMetalPipeline *)state)->_raw);
    constexpr NSUInteger kTexels = 300;
    id<MTLBuffer> buffer = [device newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(552)
                                                                                     width:kTexels
                                                                           resourceOptions:MTLResourceStorageModeShared
                                                                                     usage:MTLTextureUsageShaderRead];
    id<MTLTexture> texels = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:kTexels * 8];
    ASSERT_TRUE(texels != nil && ((SwitchMetalTexture *)texels)->_gm20bOk);
    const gm20b::Texture *standIn = ((SwitchMetalTexture *)texels)->_raw.standIn;
    ASSERT_TRUE(standIn && standIn->luma) << "a stand-in of rows and the image its codes are copied into";
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64);
    ASSERT_TRUE(colour != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = colour;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p, ^(id<MTLRenderCommandEncoder> e) {
                  [e setRenderPipelineState:state];
                  [e setFragmentTexture:texels atIndex:0];
                  [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
              }));
    const std::vector<Event> e = events(first);
    const size_t copied = indexOf(e, true, standIn->luma->va), converted = indexOf(e, false, standIn->va),
                 drawn = indexOf(e, false, ((SwitchMetalTexture *)colour)->_raw.va);
    ASSERT_NE(copied, SIZE_MAX) << "the codes copied into the stand-in's image";
    ASSERT_NE(converted, SIZE_MAX) << "the conversion's pass into the stand-in";
    ASSERT_NE(drawn, SIZE_MAX);
    EXPECT_LT(copied, converted);
    EXPECT_LT(converted, drawn);
}

void samplesTextureZeroInPairs(const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record)
{
    samplesTextureZero(request, record);
    record.pipeline.xrSlots = request.variant.xrSlots;
}

// Over two planes whose rows lie 16 bytes past a multiple of 32, which no texture header addresses, RGB10A8_2P_XR10's
// header pair reads each plane's pool image (xr_pools.h). The copy engine fills both pools before the first pass
// that samples the texture, not before the next while nothing wrote it, and again after a copy wrote its rows.
void aTwoPlaneTextureOfRowsOff32IsReadThroughItsPools()
{
    RenderCompiler compiler;
    compiler.shapes = samplesTextureZeroInPairs;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "xr-pools",
                                    writePack(packKey(MTLPixelFormatRGBA16Float), {}, {}, true, "",
                                              {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                               {"raw.xr.xr_planes_merge_frag", MTLPixelFormatRG32Uint},
                                               {"raw.xr.xr_planes_split_frag", MTLPixelFormatR32Uint}}),
                                    device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil && ((SwitchMetalPipeline *)state)->_raw);
    id<MTLTexture> texture = twoPlaneTexture(device, 16);
    ASSERT_TRUE(texture != nil && ((SwitchMetalTexture *)texture)->_gm20bOk) << "rows of 272 and 80 bytes";
    const gm20b::Texture &x = ((SwitchMetalTexture *)texture)->_raw;
    ASSERT_TRUE(x.xrPlanes && x.luma->sampled && x.chroma->sampled);
    for (uint32_t k = 0; k < 2; k++) {
        const uint64_t pool = (k ? x.chroma : x.luma)->sampled->va;
        const uint32_t *w = gRM->raw->headerWords(x.tic + k);
        EXPECT_EQ(w[1] & ~511u, uint32_t(pool) & ~511u) << "plane " << k;
        EXPECT_EQ(w[2] & 0xffff, uint32_t(pool >> 32)) << "plane " << k;
    }
    id<MTLTexture> colours[3];
    for (id<MTLTexture> __strong &c : colours) {
        c = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64);
        ASSERT_TRUE(c != nil);
    }
    id<MTLBuffer> words = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
    gm20b::RenderVariant pairs;
    pairs.xrSlots = 1;
    bool readied = false;
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    for (uint32_t k = 0; k < 3; k++) {
        if (k == 1) {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:words
                       sourceOffset:0
                  sourceBytesPerRow:64 * 4
                sourceBytesPerImage:64 * 64 * 4
                         sourceSize:MTLSizeMake(64, 64, 1)
                          toTexture:texture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
        }
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = colours[k];
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e setRenderPipelineState:state];
        [e setFragmentTexture:texture atIndex:0];
        if (!k) {
            gm20b::Variant<gm20b::Pipeline> *variant = variantOf(state, pairs);
            readied = variant && ready(*variant);
        }
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_TRUE(readied) << "the draws read the texture through its header pair";
    expectRaw(device, cb);
    const std::vector<Event> e = events(first);
    // the copy launches into each pool between two events
    const auto fills = [&](size_t from, size_t to) {
        std::set<uint64_t> pools;
        for (size_t i = from; i < to && i < e.size(); i++)
            if (e[i].copy && (e[i].address == x.luma->sampled->va || e[i].address == x.chroma->sampled->va))
                pools.insert(e[i].address);
        return pools.size();
    };
    size_t drawn[3];
    for (uint32_t k = 0; k < 3; k++) {
        drawn[k] = indexOf(e, false, ((SwitchMetalTexture *)colours[k])->_raw.va);
        ASSERT_NE(drawn[k], SIZE_MAX) << k;
    }
    const size_t copied = indexOf(e, true, x.luma->va);
    ASSERT_NE(copied, SIZE_MAX) << "the copy into plane 0's rows";
    EXPECT_EQ(fills(0, drawn[0]), 2u) << "each pool before the first pass that samples them";
    EXPECT_LT(drawn[0], copied);
    EXPECT_LT(copied, drawn[1]);
    EXPECT_EQ(fills(drawn[0], copied), 0u);
    EXPECT_EQ(fills(copied, drawn[1]), 2u) << "each again after the copy wrote the rows";
    EXPECT_EQ(fills(drawn[1], drawn[2]), 0u) << "nothing wrote them since";
}

MTLTextureDescriptor *rendered(uint32_t format, NSUInteger side)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:side
                                                                                height:side
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return d;
}

// RGB10A8_2P_XR10 and its sRGB twin written over one plane alone, a 'w30r' surface's or plane 1 of a b3a8 one: held in
// that plane as one over two planes is, its words the plane's rows, its alpha a memoryless target whose header is over a
// sparse range of the texture's own, which reads 0 and drops stores (an M4 Max stores no alpha there and reads it 0). A
// pass into one stores its stand-in's words into the rows.
void writtenOverOnePlaneIsHeldInIt()
{
    kernel_model::model.infoFlags = TegraGM20BInfoSparse;
    id<MTLDevice> device =
        open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                       {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                        {"raw.xr.xr_two_planes_srgb_frag", MTLPixelFormatRGBA16Float},
                        {gm20b::xrTargetState(550, gm20b::XrStep::Load, 1, true), MTLPixelFormatRGBA16Float},
                        {gm20b::xrTargetState(550, gm20b::XrStep::Store, 1, true), MTLPixelFormatR32Uint}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef one = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('w30r'))
    });
    ASSERT_TRUE(one);
    IOSurfaceRef several = twoPlaneSurface();
    ASSERT_TRUE(several);

    const struct {
        IOSurfaceRef s;
        uint32_t format;
        NSUInteger plane, side;
    } kCases[] = {{one, 550, 0, 64}, {one, 551, 0, 64}, {several, 550, 1, 16}};

    id<MTLTexture> target = nil;
    for (const auto &c : kCases) {
        SwitchMetalTexture *t = (SwitchMetalTexture *)[device newTextureWithDescriptor:rendered(c.format, c.side)
                                                                             iosurface:c.s
                                                                                 plane:c.plane];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << c.format << " plane " << c.plane;
        const gm20b::Texture &x = t->_raw;
        ASSERT_TRUE(x.xrPlanes && x.luma && x.chroma) << c.format;
        EXPECT_TRUE(x.chroma->memoryless) << c.format;
        const size_t rows =
            IOSurfaceGetPlaneCount(c.s) ? IOSurfaceGetBytesPerRowOfPlane(c.s, c.plane) : IOSurfaceGetBytesPerRow(c.s);
        EXPECT_EQ(x.luma->pitch, uint32_t(rows)) << c.format;
        const uint32_t *alpha = gRM->raw->headerWords(x.tic + 1);
        ASSERT_GE(x.window.bytes, uint64_t(c.side) * c.side) << c.format;
        EXPECT_EQ(uint64_t(alpha[1] & ~31u) | uint64_t(alpha[2] & 0xffff) << 32, x.window.va) << "alpha over its sink";
        EXPECT_NE(x.standIn, nullptr) << c.format;
        if (!target)
            target = (id<MTLTexture>)t;
    }
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = target;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const uint64_t rows = ((SwitchMetalTexture *)target)->_raw.luma->va;
    EXPECT_NE(indexOf(events(first), false, rows), SIZE_MAX) << "the words into the rows";
    for (IOSurfaceRef s : {one, several})
        CFRelease(s);
}

// RGB10A8_2P_XR10 written over a surface of no planes the kernel laid out block-linear for a BGR10_XR target made over it
// first: Metal makes it and reads and stores the surface's words (an M4 Max over a 'w30r' a BGR10_XR target cleared to
// 0xe009fefe: the 550 reads (0.2510, 0.5, 0.7490, 0), a kernel's store leaves 0x27f80100), so the driver holds its words
// in the kernel's layout: its header pair's words a block-linear header, its CPU copies and its blits with another such
// texture in that layout
void writtenOverATiledSurfaceIsHeldInItsLayout()
{
    kernel_model::model.infoFlags = TegraGM20BInfoSparse | TegraGM20BInfoSurfaceLayout;
    kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                           {gm20b::xrConversionOf(554), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef surfaces[2];
    SwitchMetalTexture *xr[2];
    for (IOSurfaceRef &s : surfaces) {
        s = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @64,
            (id)kIOSurfaceHeight : @64,
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfacePixelFormat : @(uint32_t('w30r'))
        });
        ASSERT_TRUE(s);
        SwitchMetalTexture *first =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:rendered(554, 64) iosurface:s plane:0];
        ASSERT_TRUE(first != nil && first->_gm20bOk && first->_raw.blockLinear) << "the kernel tiled it for the target";
        SwitchMetalTexture *t =
            (SwitchMetalTexture *)[device newTextureWithDescriptor:rendered(550, 64) iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_gm20bOk) << "Metal makes it";
        ASSERT_TRUE(t->_raw.xrPlanes && t->_raw.luma && t->_raw.chroma);
        const gm20b::Texture &words = *t->_raw.luma;
        EXPECT_TRUE(words.blockLinear);
        EXPECT_EQ(words.va, first->_raw.va) << "the surface's own words";
        EXPECT_TRUE(t->_raw.chroma->memoryless) << "no alpha plane";
        const uint32_t *pair = gRM->raw->headerWords(t->_raw.tic);
        EXPECT_EQ(pair[2] >> 21 & 7, 3u) << "HEADER_VERSION SELECT_BLOCKLINEAR";
        xr[&s - surfaces] = t;
    }
    const gm20b::Texture &words = *xr[0]->_raw.luma;
    const uint32_t word = 0x2abcdef1;
    const uint8_t *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surfaces[0]));
    [(id<MTLTexture>)xr[0] replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:&word bytesPerRow:4];
    EXPECT_EQ(memcmp(base + tgm_layout_offset(&words.import->layout, 5 * 4, 3), &word, 4), 0)
        << "the CPU writes the word where the layout holds texel (5, 3)";
    uint32_t back = 0;
    [(id<MTLTexture>)xr[0] getBytes:&back bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
    EXPECT_EQ(back, word);
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:(id<MTLTexture>)xr[0]
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(8, 0, 0)
               sourceSize:MTLSizeMake(16, 8, 1)
                toTexture:(id<MTLTexture>)xr[1]
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    expectRaw(device, cb);
    EXPECT_NE(indexOf(events(first), true, xr[1]->_raw.luma->va), SIZE_MAX)
        << "the words copied, block-linear to block-linear";
    for (IOSurfaceRef s : surfaces)
        CFRelease(s);
}

}  // namespace

TEST(GM20BFacadeXrImages, WrittenOverOnePlaneIsHeldInIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(writtenOverOnePlaneIsHeldInIt);
}

TEST(GM20BFacadeXrImages, WrittenOverATiledSurfaceIsHeldInItsLayout)
{
    switch_ios::test::ExpectPassesInFreshProcess(writtenOverATiledSurfaceIsHeldInItsLayout);
}

TEST(GM20BFacadeXrImages, ATwoPlaneTextureOfRowsOff32IsReadThroughItsPools)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTwoPlaneTextureOfRowsOff32IsReadThroughItsPools);
}

TEST(GM20BFacadeXrImages, ASampledTexelBufferIsConvertedFromItsCodesFirst)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSampledTexelBufferIsConvertedFromItsCodes);
}

TEST(GM20BFacadeXrImages, TwentyFiveTwoPlaneTexturesWrittenInAPassRender)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        twoPlaneTexturesWrittenInOnePass(gm20b::PassDesc::kWrittenPlanes + 1);
    });
}

TEST(GM20BFacadeXrImages, FortyTwoPlaneTexturesWrittenInAPassRender)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        twoPlaneTexturesWrittenInOnePass(40);
    });
}

TEST(GM20BFacadeXrImages, NoImageTagIsAnotherScratchsOrAnotherImages)
{
    std::set<uint32_t> tags;
    for (uint32_t k = 0; k < 64; k++) {
        const uint32_t tag = xrPlanesImageTag(k);
        EXPECT_TRUE(tags.insert(tag).second) << "image " << k << " shares tag " << tag;
        EXPECT_FALSE(tag >= gm20b::kPlaneImageTag && tag < gm20b::kPlaneImageTag + 8) << k;
        EXPECT_FALSE(tag >= 48 && tag < kXrPlanesImageTag) << k << ": a turn copy's, reach's, conversion's or load's";
        EXPECT_FALSE(tag >= gm20b::kFormatStandInTag && tag < gm20b::kFormatStandInTag + 16)
            << k << ": a format stand-in's";
    }
}
