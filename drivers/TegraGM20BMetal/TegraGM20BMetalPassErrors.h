// SPDX-License-Identifier: MIT
// Render passes that fail their command buffer on the driver, nothing of them run anywhere: attachments Metal's
// validation layer stops (MTLRenderPass.h; Metal's pixel format capabilities), and the driver's own passes around a
// client's (load, store and resolve passes, stand-ins, conversions) stopped by a want of memory or the driver's error.
#pragma once
#import "TegraGM20BMetalInternal.h"

// `pass` (the encoder's copy) as an Apple GPU runs it with API validation off (an M4 Max, macOS 27.2.1:
// harness/passoracle.mm): a memoryless attachment loaded reads zeros and stores nothing; one aspect of a
// Depth32Float_Stencil8 texture attached alone leaves the other as it was; a slice of a type with none and a depth
// plane of one not 3D are ignored; a colour attachment of a block format, or past its texture's levels, slices or
// planes, keeps none of its writes (a device-made RGB10A8_2P_XR10 keeps a pass's: HW Test gpu.gm20b.xr_planes_images, made
// case, made_kept_off 0); renderTargetArrayLength past an
// attachment's layers draws the layers it has. CustomSampleDepthStore keeps each sample's depth, which a stored zeta
// holds uncompressed (comp.h storedZetaKind), so it is the plain store. An X32_Stencil8 view as the stencil
// attachment is the Depth32Float_Stencil8 texture it views, at the view's level and slice in it. True: it counted a
// skip (a pass that differs from the M4 Max's), which the pass's every making counts again.
bool gm20bPassAsApple(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *pass);

// Why Metal's validation stops `pass` as a whole, null where it takes it: attachments of different sample counts
// (MTLRenderPipeline.h rasterSampleCount: the samples per fragment of the render pass)
const char *gm20bPassShapeInvalid(MTLRenderPassDescriptor *pass);
// Of a pass gm20bPassAsApple made, why colour attachment `a` fails it, null where it does not: another device's texture
// (not in this GPU's address space), one of a depth/stencil format (Apple's GPU writes the colour's bits into it), a
// block format's whose memoryless stand-in could not be made, or one of MTLPixelFormatUnspecialized, which Metal makes
// no texture of
const char *gm20bColorTargetInvalid(MTLRenderPassColorAttachmentDescriptor *a);
// ... a resolve into a texture of another format (no rule across formats but the 8-bit twins: m2v's resolve.rs), another
// device's, or past its texture's levels, slices or planes. One into a multisampled texture an M4 Max runs, each sample
// the resolved colour (passoracle resolve_other_count): the driver's (TegraGM20BMetalRangeSeal.mm resolvePasses)
const char *gm20bColorPartInvalid(MTLRenderPassColorAttachmentDescriptor *a);
// ... of the pass's depth and stencil attachments: another device's texture, one with no depth aspect as the depth
// attachment or no stencil aspect as the stencil attachment, a level or slice past its texture's, a one-sample one
// resolved (a GPU fault on Apple's GPU), a resolve into another device's texture, one without the aspect, or past its
// texture's levels or slices
const char *gm20bZetaAttachmentInvalid(MTLRenderPassDescriptor *pass);
// Whether a depth or stencil aspect resolves into a memoryless texture, which an M4 Max fails its command buffer for
// (MTLCommandBufferErrorInvalidResource: harness/passformsoracle.mm zeta_resolves)
bool gm20bZetaResolvesIntoMemoryless(MTLRenderPassDescriptor *pass);

// A pass Metal's validation stops (`rule`) or one the driver cannot make (`why`, failing with `code`), found as it
// is recorded: its encoder records nothing more and the command buffer fails, its predicate `p` (values `a`, `b`)
// named in its path. The first logs its rule's line (gm20b:pass.invalid).
void gm20bPassInvalid(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule);
void gm20bPassFailed(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, NSString *why,
                     MTLCommandBufferError code = MTLCommandBufferErrorInternal);

// The driver's own work around a pass failed as it was encoded (`why`): the command buffer fails with `code`.
// Encoded ahead of its commit, the commit's encode meets the same failure and fails it then.
void gm20bPassWorkFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, MTLCommandBufferError code);
// ... a draw of that work the encoder refused (`f`): a want of memory where it is one (argument tables, a texture
// header), else the driver's error
void gm20bOwnDrawFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, const gm20b::Refusal &f);
