// SPDX-License-Identifier: MIT
// The Metal side of decoded formats (decoded.h): their uploads, recorded and encoded as decode passes, and their
// readbacks, which move nothing.
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "decoded.h"

// The format `t` decodes its uploads by, null for a texture of any other format
const gm20b::DecodedFormat *gm20bDecoded(SwitchMetalTexture *t);
// A copyFromBuffer:toTexture: into decoded `t` as Metal's rules take it, `size` cut to its level (whose last blocks
// hold its edge texels); false: refused with one line, nothing recorded
bool gm20bDecodedCopyRecorded(SwitchMetalCommandBuffer *cb, const char *call, SwitchMetalBuffer *b,
                              SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize &size,
                              NSUInteger offset, NSUInteger row, NSUInteger image);
// Its decode passes: plane by plane of a 3D level, `image` bytes apart, into the stored image of `t`'s levels owner
bool gm20bEncodeDecodedCopy(gm20b::CommandBuffer *rc, SwitchMetalBuffer *b, SwitchMetalTexture *t, uint64_t offset,
                            uint64_t row, uint64_t image, const uint32_t size[3], uint32_t slice, uint32_t level,
                            const uint32_t origin[3], const char **why);
// replaceRegion of decoded `t`: the caller's blocks once into a buffer of the device's, decoded by a blit of an
// internal command buffer committed after them. False: `t` is of another format
bool gm20bDecodedReplace(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes,
                         NSUInteger row, NSUInteger image);
// A readback (getBytes, a blit into a buffer) of decoded `t` that keeps no blocks: nothing moved, one line. False: `t`
// is of another format
bool gm20bDecodedReadback(SwitchMetalTexture *t, const char *call);
// The device's queue for the uploads the driver commits itself (replaceRegion's, a view's first decode)
id<MTLCommandQueue> gm20bDecodeQueue(id<MTLDevice> device);

// TegraGM20BMetalDecodedFamily.mm: a decoded texture that keeps its blocks, and its views of other forms
// The texture `t` is, or views, when it keeps its blocks; nil otherwise
SwitchMetalTexture *gm20bFamilyOwner(SwitchMetalTexture *t);
// The kept blocks of new texture `t` where its format keeps them (zeros); false: no memory for them
bool gm20bKeepDecodedBlocks(SwitchMetalTexture *t);
// A view of `of` as another form of its footprint: an image of its own, made by a blit of the kept blocks into it
id<MTLTexture> gm20bFormView(SwitchMetalTexture *of, MTLPixelFormat format, MTLTextureType type, NSRange levels,
                             NSRange slices);
// A view of another form going: out of its texture's forms, on the submit worker
void gm20bForgetForm(SwitchMetalTexture *view);
// An upload of `t`'s region from `source` (offset, row, image bytes): into the kept blocks unless they are the source,
// then decoded into `t` alone (`fill`: a view's first decode) or into the texture and every view of another form
bool gm20bEncodeFamilyUpload(gm20b::CommandBuffer *rc, SwitchMetalCommandBuffer *cb, gm20b::BlitEncoder *&be,
                             SwitchMetalBuffer *source, uint64_t offset, uint64_t row, uint64_t image,
                             SwitchMetalTexture *t, const uint32_t size[3], uint32_t slice, uint32_t level,
                             const uint32_t origin[3], bool fill, const char **why);
// A blit of `t`'s region into buffer `into`: its kept blocks' rows
bool gm20bEncodeFamilyReadback(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, SwitchMetalTexture *t,
                               SwitchMetalBuffer *into, uint64_t offset, uint64_t row, uint64_t image,
                               const uint32_t size[3], uint32_t slice, uint32_t level, const uint32_t origin[3]);
// A copy into `t`, which keeps its blocks: the source's blocks into the kept copy, then every form decoded from it
bool gm20bEncodeFamilyTextureCopy(gm20b::CommandBuffer *rc, SwitchMetalCommandBuffer *cb, gm20b::BlitEncoder *&be,
                                  SwitchMetalTexture *s, uint32_t sourceSlice, uint32_t sourceLevel,
                                  const uint32_t from[3], SwitchMetalTexture *t, uint32_t slice, uint32_t level,
                                  const uint32_t origin[3], const uint32_t size[3], const char **why);
// getBytes of decoded `t`: its kept blocks where it keeps them, else nothing moved (gm20bDecodedReadback). False: another
// format
bool gm20bDecodedGetBytes(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, void *bytes,
                          NSUInteger row, NSUInteger image);
