// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"

// The render pass attachments the driver takes (TegraGM20BMetalEncoder.mm renderCommandEncoderWithDescriptor:)

// The pass's depth/stencil texture: its depth attachment's, else its stencil attachment's
id<MTLTexture> gm20bZetaAttachment(MTLRenderPassDescriptor *pass);
// Whether store action `a` resolves
bool gm20bResolvesStore(MTLStoreAction a);
// What a store action keeps of the attachment itself: a resolve's as DontCare (MultisampleResolve) or Store
MTLStoreAction gm20bKeptStore(MTLStoreAction a);
gm20b::StencilSample gm20bStencilPick(MTLRenderPassDescriptor *pass);
// Whether the depth and stencil attachments are apart: two textures, or one at two levels or slices. An M4 Max loads, clears
// and stores each aspect in its own texture, level and slice, whatever the other's format (Depth32Float_Stencil8 beside a
// Stencil8, a Depth32Float beside a Depth32Float_Stencil8's stencil) and extent (the render area their smallest:
// harness/passformsoracle.mm zeta_two_*, pair_sizes, mixed_pairs). The pass renders a Depth32Float_Stencil8 of theirs
// (gm20b::PassDesc::separate), every layer of a layered pass from each one's own slice, each resolved aspect from its own
// texture after the pass stores it there. GM20B's zeta formats pair stencil only with a 24-bit or 32-bit float depth
// (SET_ZT_FORMAT), so a Depth16Unorm's depth goes through it converted by draws (gm20b::CommandBuffer::depthConvert), a layer
// a pass, a multisampled one's a sample at a time where the pack has those states; a multisampled one's depth neither
// loaded nor kept needs no conversion
bool gm20bSeparateZeta(MTLRenderPassDescriptor *pass);

enum class SwitchMetalZetaForm : uint8_t {
    Raw,
    NotRaw,   // an attachment the driver holds no zeta of
    Unbuilt,  // a form not built (*why)
};
// Whether the driver renders the pass's depth/stencil attachments (gm20b::DepthStencilAttachment): one zeta texture
// (zeta.h) as each aspect its format has (Stencil8 a stencil attachment alone), or a pair apart (gm20bSeparateZeta),
// each at a level and slice it holds, each aspect's resolve one the driver draws. Called on a pass gm20bPassAsApple made
// and gm20bZetaAttachmentInvalid took.
SwitchMetalZetaForm gm20bRawDepthStencil(MTLRenderPassDescriptor *pass, bool separate, const char **why);
// Level `level`, slice `slice` of zeta texture `t` as the image a pass renders or resolves into: `t` itself (level 0 of
// slice 0, or a memoryless one), else `part` made from the chain of t's levels owner; null: no such level
const gm20b::Texture *gm20bZetaPart(SwitchMetalTexture *t, NSUInteger level, NSUInteger slice, gm20b::Texture &part);
// The texture header of that part, made once and freed with its owner's (gm20b::Device::attachmentTic); 0: none
uint32_t gm20bZetaPartTic(SwitchMetalTexture *t, NSUInteger level, NSUInteger slice, const gm20b::Texture &part);
// A resolve the driver draws after the pass (the pack's resolve state of its format, TegraGM20BMetalEncode.mm): of a
// multisampled attachment into a level, slice or depth plane of a one-sample texture of its format the driver
// renders to (the attachment's own target, attachment_target.h), or a one-sample attachment's, which is a copy; a
// stand-in attachment's into a one-sample stand-in target (TegraGM20BMetalRangeSeal.mm standInResolve)
bool gm20bRawResolve(SwitchMetalCommandBuffer *cb, MTLRenderPassColorAttachmentDescriptor *a, SwitchMetalTexture *from,
                     SwitchMetalPass::Color &facts);
// A colour target of a format the GM20B renders none of, through a stand-in (format_standin.h) of the pass's layers
// and its samples (gm20b::formatStandInSamples), a turned one's in the plane's shape and turned
// (TegraGM20BMetalRangeForms.mm formatStandIns); a level, slice or 3D plane of it is taken as any target's part is
// (gm20bAttachmentPart)
bool gm20bFormatTarget(SwitchMetalTexture *x);
// A view's whole first level, a block-linear 2D or 1D image of a colour target format that createPartView marks no target
// (one of several levels, or 1D): the pass binds it as it is, renderable
bool gm20bWholeViewTarget(SwitchMetalTexture *x);
// Why the driver renders no form of level `level`, slice `slice` (a 3D texture's plane) of raw texture `x` as a colour
// target, null where it does: a target as it is, a pitch one through a shadow, a stand-in, a part, a texture buffer's row
// (gm20b::texelBufferTarget) or a plane inside blocks of several (gm20b::planeInBlocks)
const char *gm20bColorTargetMissing(SwitchMetalTexture *x, NSUInteger level, NSUInteger slice);
// ... why it binds no image of that part (`resolving`: one a resolve reads)
const char *gm20bColorPartMissing(SwitchMetalTexture *x, NSUInteger level, NSUInteger slice, bool resolving);
// Why a pass into planar or YCbCr target `x` fails its command buffer, null where the driver renders it. A planar target
// has no stand-in where it was made without render-target usage, which Metal's validation stops a pass into, or the device
// could not make one (gm20b:planar.target)
const char *gm20bPlanarTargetRefusal(SwitchMetalTexture *x, MTLRenderPassColorAttachmentDescriptor *a);
