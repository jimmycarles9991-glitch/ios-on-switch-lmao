// SPDX-License-Identifier: MIT
// A blit's sides as Metal takes them (TegraGM20BMetalBlitRules.mm): the rules MTLBlitCommandEncoder.h states, and plain Metal's
// answers past them that m2v measured (copy_plan.rs, blit_plan.rs), for the blit encoder's copies, fills and mipmaps
#pragma once
#import "TegraGM20BMetalInternal.h"

// blitAspect's answer where a Depth32Float_Stencil8 texture has no memory for its stencil temp
constexpr int kAspectNoMemory = -1;

// A texture-to-texture copy's form (OpCopyTexture's a) past gm20b::kAspectWhole and gm20b::kAspectStencil (a stencil view into
// another): a stencil view's stencil into a texture of one-byte texels, the other way, both planes of two textures held
// through planes, plane 0's bytes of a side held through planes (blit_planes.h planeBytes), and RGB10A8_2P_XR10 of a
// descriptor's with another (TegraGM20BMetalXrCopies.h)
constexpr uint32_t kCopyStencilOut = 3, kCopyStencilIn = 4, kCopyPlanes = 5, kCopyPlaneBytes = 6, kCopyXrPlanes = 7;

MTLSize levelSize(id<MTLTexture> t, NSUInteger level);
NSUInteger sliceCount(id<MTLTexture> t);
// A copy's texel origin and size as `t`'s elements (gm20b::toElements); false where the origin is not a whole block's
bool inElements(SwitchMetalTexture *t, MTLOrigin &o, MTLSize &size);
// In elements (inElements)
bool copySideOk(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size);
// A buffer side's planes: `image` bytes apart, each on the rows' terms (bufferRows), the last inside the buffer
bool bufferPlanes(uint64_t offset, uint64_t row, uint64_t image, MTLSize size, uint32_t bpp, uint64_t length,
                  bool intoTexture);
// The aspect (gm20b::kAspect*) a copy between `t` and a buffer moves under `options`, as plain Metal moves it
int blitAspect(SwitchMetalTexture *t, MTLBlitOption options);
// The bytes of each texel a copy between `t` and a buffer moves in the buffer
uint32_t blitUnit(SwitchMetalTexture *t, int aspect);

// The texture of this device the driver holds, else nil
SwitchMetalTexture *gm20bTexture(id<MTLTexture> t);
// A blit of a texture another device made: Metal's validation error, the command buffer fails (gm20bCallInvalid)
void notRawTexture(SwitchMetalCommandBuffer *cb, id<MTLTexture> t);

// A blit Metal's validation stops, recorded as nothing, as an M4 Max's driver runs it or where what it writes follows
// no rule (c38-audit/failed-counts/probe/RESULTS.txt): skipped as call.blit (gm20bSkipped), one line a process for each
// call and rule (gm20b:blit.invalid)
void skipInvalidBlit(const char *call, const char *why);
// ... of `cb`, or, a side memoryless (which an M4 Max fails InvalidResource), its command buffer fails InvalidResource
void invalidBlit(SwitchMetalCommandBuffer *cb, const char *call, const char *why);
// Where a copy's region in texture `t` runs past its `level`: cut to the level, as an M4 Max copies the part inside
// (c38-audit/failed-counts/probe: buf2tex_region_past, pat_region); skipped as call.blit, its line once
void cutToLevel(const char *call, SwitchMetalTexture *t, NSUInteger level, MTLOrigin o, MTLSize &size);
// ... a copy into `t` at an origin off its format's blocks: rounded down to the block, as an M4 Max writes it
// (pat_block)
void roundToBlocks(const char *call, SwitchMetalTexture *t, MTLOrigin &o);
// ... a copy from slice `slice` of a 2D texture, which has one: slice 0, as an M4 Max copies it (pat_slice2d)
void flatSlice(const char *call, SwitchMetalTexture *t, NSUInteger &slice);
// ... a copy between textures of two pixel formats, one of them held through planes: its bytes moved in the
// destination's texel, as an M4 Max moves them (m2v-textures11 probes/h5/copies.txt, m2v-textures12 probes/h5x.txt)
void copyAcrossFormats(const char *call);
// A colour fill of a texture held through planes: undefined on Apple's GPU (an M4 Max writes plane 0's region with a value
// that ignores the colour and follows the process's earlier fills), recorded as nothing, one line a process
// (gm20b:blit.undefined)
void skipUndefinedFill(const char *call);
// A blit Metal's rules take whose side the driver holds no image of (the facade's own fault, never a client's): its
// command buffer fails MTLCommandBufferErrorInternal, one line (gm20b:blit.image)
void failNoImage(__unsafe_unretained SwitchMetalCommandBuffer *cb, const char *call);
// No memory for a Depth32Float_Stencil8 texture's stencil temp: MTLCommandBufferErrorOutOfMemory, one line
// (gm20b:blit.stencil_memory)
void failNoStencilTemp(__unsafe_unretained SwitchMetalCommandBuffer *cb, const char *call);
// Why one side of a copy breaks Metal's rules, null where it keeps them
const char *copySideInvalid(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size);
// Why a texture-to-texture copy breaks Metal's rules: another sample count, formats of other copy units, a side outside
// its texture
const char *textureCopyInvalid(SwitchMetalTexture *a, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize size,
                               SwitchMetalTexture *b, NSUInteger ds, NSUInteger dl, MTLOrigin dorg);
// Why a copy between texture `t` and a buffer of `length` bytes breaks Metal's rules
const char *bufferCopyInvalid(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size,
                              uint64_t offset, uint64_t row, uint64_t image, uint64_t length, int aspect,
                              bool intoTexture);
// The levels a whole-texture copy starts at: the smaller's first, and the larger's of its size. False: neither has a level
// of the other's size.
bool sameSizeLevels(id<MTLTexture> s, id<MTLTexture> d, NSUInteger &sl, NSUInteger &dl);
