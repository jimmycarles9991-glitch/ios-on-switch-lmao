// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures as render targets (raw_ycbcr_store.h): Metal holds a pass's texels as halves and stores each
// texel's luma code and each chroma block's rounded mean code (code formats), or m2v's matrix rule (converted ones). The raw path
// renders the pass into a memoryless RGBA32Float stand-in (its pipelines are made for that format in the attachment's place);
// the pack's store states then draw the luma plane and the chroma blocks from the pass's backing of it, every code worked out in
// the program (pack/ycbcr_store.metal). The 2D engine's 2:1 bilinear halving stores a tie rounded down where Metal's mean rounds
// it up (24.9% of 4:2:0 and 48.1% of 4:2:2 chroma bytes off on the device), so the blocks are a draw too. A pass that loads the
// texture is refused: Metal reloads the chroma interpolated between blocks. R10Unorm_PACKED and RG10Unorm_PACKED planes
// (raw_packed_plane.h) render the same way, into an R32Float or RG32Float stand-in, through one store state into their words.
#import "RawMetalInternal.h"
#include "raw_ycbcr_store.h"

void makeYcbcrTarget(raw::Texture &t, uint32_t pixelFormat, const raw::YcbcrStoreShape &store)
{
    raw::Device &d = *gRM->raw;
    const auto rendered = [](const raw::Texture *x) {
        return x && (x->renderable || x->shadowTarget);
    };
    raw::Texture *standIn = new raw::Texture;
    const bool chroma = !store.plane;  // a packed plane's words are its one plane
    if (!rendered(t.luma) || (chroma && !rendered(t.chroma)) || !d.ycbcrStorePipeline(false, store) ||
        (chroma && !d.ycbcrStorePipeline(true, store)) ||
        !d.createMemoryless(t.width, t.height, raw::ycbcrStandInFormat(pixelFormat), *standIn)) {
        delete standIn;
        SWITCH_REFUSE_DETAIL("raw", "ycbcr.target", pixelFormat,
                             "no stand-in or store state for a %ux%u format %u render target: its passes go to m2v "
                             "(enc.planarrt)",
                             t.width, t.height, pixelFormat);
        return;
    }
    t.planarStandIn = standIn;
    t.ycbcrStore = new raw::YcbcrStoreShape(store);
}

const char *ycbcrTargetPassRefusal(const raw::Texture &t, MTLRenderPassColorAttachmentDescriptor *a)
{
    const char *why = !t.planarStandIn || !t.ycbcrStore   ? "a YCbCr texture with no stand-in as a render target"
                      : a.loadAction != MTLLoadActionLoad ? nullptr
                      : t.ycbcrStore->plane
                          ? "a packed 10-bit plane loaded: what Metal loads of its codes is not measured (an M4 "
                            "Max's passes into one fault)"
                          : "a YCbCr target loaded: Metal reloads its chroma interpolated between blocks";
    if (why)
        SWITCH_REFUSE_DETAIL("raw", "pass.ycbcr", a.loadAction, "%ux%u format %u: %s", t.width, t.height,
                             uint32_t(a.texture.pixelFormat), why);
    return why;
}

int storeYcbcrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &standIn, const char **why)
{
    raw::Device &d = *gRM->raw;
    for (const bool chroma : {false, true}) {
        if (chroma && !t.chroma)
            continue;
        raw::PassDesc p;
        p.color[0].texture = chroma ? t.chroma : t.luma;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *e = rc->render(p);
        if (!e) {
            *why = "a YCbCr texture's plane the raw path cannot render";
            return 1;
        }
        e->setPipeline(d.ycbcrStorePipeline(chroma, *t.ycbcrStore));
        e->setTexture(0, &standIn);
        e->setBytes(2, 0, t.ycbcrStore, sizeof *t.ycbcrStore);
        if (t.ycbcrStore->srgb) {
            const raw::Bo &table = d.srgbEncodeTable();
            e->setBytesAt(2, raw::kSrgbEncodeSlot, table.va, uint32_t(table.bytes), table.cpu);
        }
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return 2;
        e->end();
    }
    static std::atomic<bool> said{false}, saidPlane{false};
    if (t.ycbcrStore->plane) {
        if (!saidPlane.exchange(true, std::memory_order_relaxed))
            os_log(
                rawLog(),
                "raw: packed 10-bit plane stored on the GPU (first: %ux%u, %u codes a texel): pass into a memoryless "
                "stand-in, words drawn by the pack's store state",
                t.width, t.height, t.ycbcrStore->plane);
    } else if (!said.exchange(true, std::memory_order_relaxed)) {
        os_log(rawLog(),
               "raw: YCbCr target stored on the GPU (first: %ux%u, chroma %ux%u, %u-bit %{public}s): pass into a "
               "memoryless stand-in, luma and chroma blocks drawn by the pack's store states",
               t.width, t.height, t.chroma->width, t.chroma->height, t.ycbcrStore->max > 255 ? 10u : 8u,
               t.ycbcrStore->srgb        ? "sRGB"
               : t.ycbcrStore->converted ? "converted"
                                         : "codes");
    }
    return 0;
}

uint32_t ycbcrTargetFormat(MTLRenderPipelineDescriptor *d)
{
    for (NSUInteger c = 0; c < 8; c++)
        if (raw::ycbcrRenderedFormat(uint32_t(d.colorAttachments[c].pixelFormat)))
            return uint32_t(d.colorAttachments[c].pixelFormat);
    return 0;
}

@implementation RMDevice (YcbcrTargets)

// Decided before any compile: the raw path renders no target ycbcrTargetRefusal names, nor a blended or channel-masked one
// (Metal's store of those is not measured): such a state is m2v's alone, as it was, without the raw path's translation (3-13 s
// in cameracaptured, then refused). Any other's state is made for its stand-in format. The m2v twin is of that format too:
// m2v renders no pass into a code target, so it only answers for the state. The archives a creation under
// FailOnBinaryArchiveMiss lists answer for the app's state, not its stand-in.
- (id<MTLRenderPipelineState>)ycbcrTargetState:(MTLRenderPipelineDescriptor *)d
                                       options:(MTLPipelineOption)o
                                    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                         error:(NSError **)error
{
    if (const char *why = rmUnsupportedRenderState(d))
        return rmRefuseUnsupported(why, error);
    if (rmRenderArchiveMissed(d, o, error))
        return nil;
    o &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    MTLRenderPipelineDescriptor *made = [d copy];
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[c];
        const uint32_t format = uint32_t(a.pixelFormat);
        if (!raw::ycbcrRenderedFormat(format))
            continue;
        const MTLColorWriteMask writes = MTLColorWriteMask(raw::ycbcrTargetWrites(format));
        const char *why = raw::ycbcrTargetRefusal(format);
        if (!why && (a.blendingEnabled || (a.writeMask & writes) != writes))
            why = "a blended or channel-masked YCbCr attachment";
        if (why) {
            SWITCH_REFUSE_DETAIL("raw", "state.ycbcr", format,
                                 "a state of a format %u color attachment, left to m2v without a raw compile: %s",
                                 format, why);
            RMPipeline *p = [RMPipeline new];
            p->_device = self;
            p.label = d.label;
            p->_twin = [RM_M2V("a YCbCr target state's twin") newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                                           options:o
                                                                                        reflection:reflection
                                                                                             error:error];
            return p->_twin ? (id<MTLRenderPipelineState>)p : nil;
        }
        made.colorAttachments[c].pixelFormat = MTLPixelFormat(raw::ycbcrStandInFormat(format));
    }
    RMPipeline *p = rmPipelineFor(self, made, o, error);
    if (!p)
        return nil;
    if (!p->_raw) {
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{
                                         NSLocalizedDescriptionKey : @"the raw path made no state for a YCbCr "
                                                                     @"target, and m2v renders none"
                                     }];
        return nil;
    }
    MTLRenderPipelineDescriptor *twin = twinDescriptor(made);
    if (reflection)
        p->_twin = [RM_M2V("a YCbCr target state's twin") newRenderPipelineStateWithDescriptor:twin
                                                                                       options:o
                                                                                    reflection:reflection
                                                                                         error:error];
    else if (native_bridge_metal_render_pipeline_lazy)
        p->_twin = native_bridge_metal_render_pipeline_lazy(RM_M2V("a YCbCr target state's twin"), twin, o);
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

@end
