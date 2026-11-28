// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures as render targets (ycbcr_store.h): Metal holds a pass's texels as halves and stores each
// texel's luma code and each chroma block's rounded mean code (code formats), or m2v's matrix rule (converted ones). The driver
// renders the pass into a memoryless RGBA32Float stand-in (its pipelines are made for that format in the attachment's place);
// the pack's store states then draw the luma plane and the chroma blocks from the pass's backing of it, every code worked out in
// the program (pack/ycbcr_store.metal). The 2D engine's 2:1 bilinear halving stores a tie rounded down where Metal's mean rounds
// it up (24.9% of 4:2:0 and 48.1% of 4:2:2 chroma bytes off on the device), so the blocks are a draw too. A pass that loads the
// texture renders into a stand-in a pass of the load state filled first (loadYcbcrStandIn), and stores only the tiles its draws
// reach (ycbcr_load.h). R10Unorm_PACKED and RG10Unorm_PACKED planes (packed_plane.h) render the same way, into an
// R32Float or RG32Float stand-in, through one store state into their words, which gives each loaded code back.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalReflection.h"
#include "format_standin.h"
#include "overlap_rows.h"
#include "packed_plane.h"
#include "planar_target.h"
#include "standin_blend.h"
#include "ycbcr_load.h"
#include "ycbcr_meet.h"
#include "ycbcr_store.h"

static_assert(gm20b::kPackedLoadTag + 8 <= gm20b::kDriverPassColorTag &&
                  gm20b::kDriverPassColorTag + 8 <= gm20b::kYcbcrLoadTag,
              "the driver passes' colour tags between the loaded packed planes' and the loaded YCbCr textures'");

static gm20b::Pipeline *packState(const char *name)
{
    return name ? gRM->raw->ownPipeline(name) : nullptr;
}

static gm20b::Pipeline *loadState(const gm20b::Texture &t)
{
    return t.ycbcrStore->plane ? gRM->raw->ycbcrLoadPipeline(t.ycbcrStore->plane)
                               : packState(gm20b::ycbcrLoadState(t.ycbcr));
}

// A one-plane 4:2:2 target whose pairs' rows overlap: its luma and chroma images are stored, then its pair bytes drawn from
// them (overlap_rows.h)
static bool pairsOverlap(const gm20b::Texture &t)
{
    return t.onePlane && t.planes.lumaPitch < 4 * t.chroma->width;
}

// Where a plane's rows overlap, the pool image its rows are stored into and the pack's state that draws its bytes from it
static bool overlapReady(const gm20b::Texture *plane)
{
    return !plane || !gm20b::overlapsRows(*plane) || (plane->sampled && packState(gm20b::overlapState(plane->format)));
}

static bool loadable(const gm20b::Texture &t)
{
    const gm20b::YcbcrStoreShape &s = *t.ycbcrStore;
    return loadState(t) && (s.plane || (packState(gm20b::ycbcrMaskedStoreState(false, s)) &&
                                        packState(gm20b::ycbcrMaskedStoreState(true, s))));
}

void makeYcbcrTarget(gm20b::Texture &t, uint32_t pixelFormat, const gm20b::YcbcrStoreShape &store)
{
    gm20b::Device &d = *gRM->raw;
    const auto rendered = [](const gm20b::Texture *x) {
        return x && (x->renderable || x->shadowTarget);
    };
    gm20b::Texture *standIn = new gm20b::Texture;
    // a packed plane's words are its one plane, as YCBCR10_444_1P's are (its luma header's); a texture over plane 1 stores
    // its luma codes alone
    const bool chroma = !store.plane && !store.words && t.chroma;
    const bool meet = gm20b::ycbcrPlanesMeet(t, store);
    const bool overlaps = t.onePlane ? !pairsOverlap(t) || packState(gm20b::overlapState(10))
                          : meet     ? packState(gm20b::kOverlapMeetState) != nullptr
                                     : overlapReady(t.luma) && (!chroma || overlapReady(t.chroma));
    // RGB8_444_2P's pipelines render into an RGBA16Float stand-in, as the planar path's targets do (TegraGM20BMetalOffline.mm)
    const uint32_t standInFormat =
        pixelFormat == 542 ? gm20b::kPlanarStandInFormat : gm20b::ycbcrStandInFormat(pixelFormat);
    if (!rendered(t.luma) || (chroma && !rendered(t.chroma)) || !overlaps || !d.ycbcrStorePipeline(false, store) ||
        (chroma && !d.ycbcrStorePipeline(true, store)) ||
        !d.createMemoryless(t.width, t.height, standInFormat, *standIn)) {
        delete standIn;
        SWITCH_REFUSE_DETAIL("gm20b", "ycbcr.target", pixelFormat,
                             "no stand-in or store state for a %ux%u format %u render target: its passes fail their "
                             "command buffers (enc.planarrt)",
                             t.width, t.height, pixelFormat);
        return;
    }
    t.planarStandIn = standIn;
    t.ycbcrStore = new gm20b::YcbcrStoreShape(store);
}

// A target without a stand-in is one made without render-target usage (Metal's validation stops the pass) or one whose
// stand-in or store states the device could not make; a loaded one draws the pack's own load and tile-masked store states
const char *ycbcrTargetPassRefusal(const gm20b::Texture &t, MTLRenderPassColorAttachmentDescriptor *a)
{
    const char *why = !t.planarStandIn || !t.ycbcrStore   ? "a YCbCr texture with no stand-in as a render target"
                      : a.loadAction != MTLLoadActionLoad ? nullptr
                      : loadable(t) ? nullptr
                                    : "a YCbCr target loaded: the pack has no load or tile-masked store state for it";
    if (why)
        SWITCH_REFUSE_DETAIL("gm20b", "pass.ycbcr", a.loadAction, "%ux%u format %u: %s: the command buffer fails",
                             t.width, t.height, uint32_t(a.texture.pixelFormat), why);
    return why;
}

int storeYcbcrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, const gm20b::Texture &standIn, const char **why,
                     const gm20b::PassReach *loaded)
{
    gm20b::Device &d = *gRM->raw;
    const bool masked = loaded && !t.ycbcrStore->plane;
    if (masked && !loaded->mask)
        return 0;
    const gm20b::TurnMask mask = masked ? gm20b::ycbcrStoreMask(t, *loaded) : gm20b::TurnMask{};
    const bool meet = gm20b::ycbcrPlanesMeet(t, *t.ycbcrStore);
    for (const bool chroma : {false, true}) {
        if (chroma && (!t.chroma || t.ycbcrStore->words))
            continue;
        gm20b::Texture *plane = chroma ? t.chroma : t.luma;
        gm20b::Texture scratch;
        gm20b::Texture *rows = plane->storePitch ? &(scratch = gm20b::storedRows(*plane, t)) : plane;
        const bool overlap = !t.onePlane && gm20b::overlapsRows(*rows);
        gm20b::PassDesc p;
        p.color[0].texture = overlap || meet ? plane->sampled : rows;
        p.color[0].load = masked ? gm20b::ColorAttachment::Load : gm20b::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        gm20b::RenderEncoder *e = rc->render(p);
        if (!e) {
            *why = "a YCbCr texture's plane the driver cannot render";
            return 1;
        }
        e->setPipeline(masked ? packState(gm20b::ycbcrMaskedStoreState(chroma, *t.ycbcrStore))
                              : d.ycbcrStorePipeline(chroma, *t.ycbcrStore));
        e->setTexture(0, &standIn);
        e->setBytes(2, 0, t.ycbcrStore, sizeof *t.ycbcrStore);
        if (masked) {
            e->setTexture(1, mask.mask);
            e->setBytes(2, gm20b::kYcbcrTilesSlot, &mask.tiles, sizeof mask.tiles);
            if (t.ycbcrStore->packed)
                e->setTexture(2, overlap ? plane->sampled : plane);
        }
        if (t.ycbcrStore->srgb) {
            const gm20b::Bo &table = d.srgbEncodeTable();
            e->setBytesAt(2, gm20b::kSrgbEncodeSlot, table.va, uint32_t(table.bytes), table.cpu);
        }
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return 2;
        e->end();
        if (overlap && !meet)
            if (const int failed = gm20b::storeOverlapRows(*rc, packState(gm20b::overlapState(plane->format)),
                                                           plane->format, rows->va, *rows, *plane->sampled, nullptr,
                                                           gm20b::overlapPlaneRows(*rows, chroma, *t.ycbcrStore),
                                                           gm20b::frameTiles(t), masked ? &mask : nullptr, why))
                return failed;
    }
    if (meet) {
        if (const int failed = gm20b::storeMeetingPlanes(d, *rc, t, masked ? &mask : nullptr, why))
            return failed;
    } else if (pairsOverlap(t)) {
        if (const int failed = gm20b::storeOverlapRows(*rc, packState(gm20b::overlapState(10)), 10, t.planes.luma, t,
                                                       *t.luma, t.chroma, gm20b::overlapPairsOf(t),
                                                       gm20b::frameTiles(t), masked ? &mask : nullptr, why))
            return failed;
    } else if (t.onePlane) {
        gm20b::BlitEncoder *merge = rc->blit();
        merge->mergePlane(t);
        merge->end();
    }
    return 0;
}

// What the pass loads into a block-linear stand-in of the command buffer's, which the pass renders into in place of the memoryless
// one: each read's tile value (ycbcr_load.h), or a packed 10-bit plane's codes over 1023, which its store gives back
// (packed_plane.h packedPlaneLoad). The planes' sampled shadows are refreshed first, as for a conversion.
int loadYcbcrStandIn(gm20b::CommandBuffer *rc, const gm20b::Texture &t, uint32_t j, gm20b::Texture *&loaded,
                     const char **why)
{
    gm20b::BlitEncoder *fill = nullptr;
    // a one-plane 4:2:2 target's luma and chroma images hold its pairs before its load reads them and its tile-masked store
    // keeps the tiles the pass does not reach
    if (t.onePlane) {
        fill = rc->blit();
        fill->splitPlane(t);
    }
    for (const gm20b::Texture *plane : {t.luma, t.chroma})
        if (plane && plane->sampled && rc->readTexture(plane) == plane) {
            if (!fill)
                fill = rc->blit();
            fill->copyTexels(*plane, 0, 0, 0, *plane->sampled, 0, 0, 0, plane->width, plane->height);
        }
    if (fill)
        fill->end();
    const uint32_t tag = (t.ycbcrStore->plane ? gm20b::kPackedLoadTag : gm20b::kYcbcrLoadTag) + j;
    loaded = rc->turnScratch(t.planarStandIn->format, t.width, t.height, tag);
    if (!loaded) {
        *why = "no stand-in for a loaded YCbCr target";
        return 1;
    }
    gm20b::PassDesc p;
    p.color[0].texture = loaded;
    p.color[0].load = gm20b::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    gm20b::RenderEncoder *e = rc->render(p);
    if (!e) {
        *why = "a loaded YCbCr target's stand-in the driver cannot render";
        return 1;
    }
    e->setPipeline(loadState(t));
    e->setBytes(2, 0, t.shape, sizeof t.shape);
    e->setTexture(0, t.luma);
    if (!t.ycbcrStore->plane)
        e->setTexture(1, t.chroma ? t.chroma : t.luma);
    e->setViewport(0, 0, t.width, t.height, 0, 1);
    e->setScissor(0, 0, t.width, t.height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return 2;
    e->end();
    return 0;
}

// The tiles of its frame a pass's draws reach (reach.h), a YCbCr target cleared and stored counting as any other
// attachment does
bool reachOfLoadedTargets(const SwitchMetalPass &pass, const gm20b::PassDesc &p, gm20b::DrawReplay &draws,
                          gm20b::CommandBuffer &rc, gm20b::PassReach &out, const char **why)
{
    gm20b::PassDesc q = p;
    gm20b::Texture targets[8];
    for (uint32_t k = 0; k < p.count && k < 8; k++)
        if (__unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)pass.color[k].texture;
            t && t->_raw.planarStandIn) {
            targets[k] = gm20b::ycbcrReachAttachment(t->_raw);
            q.color[k].texture = &targets[k];
        }
    return rc.reach(q, p.width, p.height, draws, out, why);
}

void gm20bAlphaOneBlend(MTLRenderPipelineColorAttachmentDescriptor *a)
{
    a.sourceRGBBlendFactor = MTLBlendFactor(gm20b::alphaOneFactor(uint32_t(a.sourceRGBBlendFactor), true));
    a.destinationRGBBlendFactor = MTLBlendFactor(gm20b::alphaOneFactor(uint32_t(a.destinationRGBBlendFactor), true));
    a.sourceAlphaBlendFactor = MTLBlendFactor(gm20b::alphaOneFactor(uint32_t(a.sourceAlphaBlendFactor), false));
    a.destinationAlphaBlendFactor =
        MTLBlendFactor(gm20b::alphaOneFactor(uint32_t(a.destinationAlphaBlendFactor), false));
}

// An extended-range attachment's state is the descriptor's own: the translator lowers it onto the attachment's
// RGBA16Float stand-in with Metal's clamp of its output (translate/NativeMetalDescriptors.rs)
static bool remadeForStandIn(uint32_t format)
{
    return gm20b::ycbcrRenderedFormat(format) || (gm20b::formatStandIn(format) && !gm20b::xrTarget(format));
}

uint32_t standInTargetFormat(MTLRenderPipelineDescriptor *d)
{
    for (NSUInteger c = 0; c < 8; c++)
        if (const uint32_t format = uint32_t(d.colorAttachments[c].pixelFormat); remadeForStandIn(format))
            return format;
    return 0;
}

@implementation SwitchMetalDevice (YcbcrTargets)

// A state of an attachment the driver renders through a stand-in, YCbCr (ycbcr_store.h) or of a format the GM20B renders
// none of (format_standin.h), made for its stand-in format, blended and channel-masked ones too: the stand-in holds the
// pass's texels, the channels a mask keeps as loaded or cleared, and a format without alpha blends by a destination alpha of
// 1 (standin_blend.h). An M4 Max blends YCbCr in halves of its own rounding, so a blended store is within one code of
// Metal's (harness/ycbcrblendoracle.mm); a masked one is Metal's. A format ycbcrTargetRefusal names has no target texture
// (planYcbcr refuses its render-target usage), so its state draws no pass, as Metal makes it all the same. A framework's
// stand-in state the pack holds is the device's archive hit (TegraGM20BMetalStates.mm); any other creation under
// FailOnBinaryArchiveMiss checks the listed archives under the app's descriptor, not its stand-in.
- (id<MTLRenderPipelineState>)standInTargetState:(MTLRenderPipelineDescriptor *)d
                                         options:(MTLPipelineOption)o
                                      reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                           error:(NSError **)error
{
    if (const char *why = gm20bUnsupportedRenderState(d))
        return gm20bRefuseUnsupported(why, error);
    const MTLPipelineOption withoutMiss = o & ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    MTLRenderPipelineDescriptor *made = [d copy];
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[c];
        const uint32_t format = uint32_t(a.pixelFormat);
        if (const uint32_t standIn = gm20b::xrTarget(format) ? 0 : gm20b::formatStandIn(format)) {
            made.colorAttachments[c].pixelFormat = MTLPixelFormat(standIn);
            if (!gm20b::formatHoldsAlpha(format))
                gm20bAlphaOneBlend(made.colorAttachments[c]);
            continue;
        }
        if (!gm20b::ycbcrRenderedFormat(format))
            continue;
        MTLRenderPipelineColorAttachmentDescriptor *standIn = made.colorAttachments[c];
        standIn.pixelFormat = MTLPixelFormat(gm20b::ycbcrStandInFormat(format));
        gm20bAlphaOneBlend(standIn);
    }
    SwitchMetalPipeline *p = gm20bPipelineShell(self, made, withoutMiss, error);
    if (!p)
        return nil;
    const bool deviceArchive = p->_packable && p->_raw;
    if (p->_completable && !deviceArchive && gm20bRenderArchiveMissed(d, o, error))
        return nil;
    if (!gm20bMakeRecord(p, made, error))
        return nil;
    if (!p->_raw) {
        if (error)
            *error = [NSError
                errorWithDomain:MTLLibraryErrorDomain
                           code:MTLLibraryErrorCompileFailure
                       userInfo:@{NSLocalizedDescriptionKey : @"the driver made no state for a stand-in's target"}];
        return nil;
    }
    if (reflection)
        *reflection = gm20bRenderReflection(p, made, withoutMiss);
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

@end
