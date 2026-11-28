// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 and RGB10A8_2P_XR10_sRGB textures over a two-plane surface on the driver, drawn and sampled in their
// planes (xr_pass.h, xr_texture.cpp; the sRGB one drawn through its format stand-in, format_standin.h)
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "attachment_lifetime.h"

// Metal reads 550 and 551 over a plane followed by another as the words of that plane and the alpha bytes of the next, and over
// the last plane (or a surface of none) as the words alone (an M4 Max over planes 0, 1 and 2 of three and plane 0 of four, read
// and written: m2v-textures9 probes/xr.txt 1g-1k, 1n-1p)
bool xrLastPlane(IOSurfaceRef s, NSUInteger plane);
// A plane's rows as Metal's GPU reads and writes them, rounded down to 16 (probes/xr.txt 5a-5m)
size_t xrGpuRows(IOSurfaceRef s, NSUInteger plane);
// 550 or 551 over a plane followed by another: a 2D texture, one level, one sample, no rotation, of any usage. An M4 Max reads words that run on into the alpha plane at their pitch, and its pass gives a byte
// both planes' rows reach to whichever store comes later, taken across both planes (xr_pools.h xrPlanesCross). Metal
// reads the planes so whatever their elements (an M4 Max over 4+1, 4+2, 8+1, 2+1,
// 4+4 and '420v' planes: m2v-textures5 probes/xrplane0.mm), their top-left corner, each row at its plane's pitch where rows
// overlap or run on past the plane (xr_pools.h)
bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
// Whether the fragment stores `d` allows (Unknown or ShaderWrite usage) can be taken in its planes: the pack holds the
// merge and split states they go through (mergeXrPlanes), or it allows none
bool xrPlanesStorable(MTLTextureDescriptor *d);
// At `width` x `height` (a turned one's plane's extent); false with gm20b::surfaceRefusal naming the step
bool makeXrPlanes(SwitchMetalTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height);
// 550 or 551 written (Unknown, RenderTarget or ShaderWrite usage), or of rows under 16 bytes, over one plane alone, a
// surface's one or the last of several, 2D as xrPlanesShape's: an M4 Max's passes and kernels store BGR10_XR's words
// there with bits 30 and 31 clear and no alpha, and its reads take alpha 0 (m2v-textures4 probes/more.mm, tb.mm). Held
// as one over two planes is, with no alpha plane (xr_texture.cpp)
bool xrWordPlaneShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
bool makeXrWordPlane(SwitchMetalTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height);
// 550 or 551 read alone over one plane alone (a surface of one, or the last of several): an M4 Max reads BGR10_XR's
// (BGR10_XR_sRGB's) words there, alpha 0 (m2v-textures3 probes/xrsurf.mm, m2v-textures4 probes/reads.mm), which the raw
// path holds under that format's header, its stand-in reading alpha 0. The format those words are held as, and whether
// `t` is held so
uint32_t xrWordsFormat(uint32_t format);
bool xrWordsTexture(SwitchMetalTexture *t);
void readAlphaZero(gm20b::Texture &standIn);
// replaceRegion/getBytes of a texture held in its planes: its word plane's colour words alone, at the plane's own rows (SwitchMetalTexture
// _cpuRows where they are off 16) or in the kernel's block-linear layout of the surface, as Metal's CPU copies move them
// (xr.h), the alpha plane as it is. False: not such a texture, or a region off its one level and slice
bool xrPlaneWords(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes,
                  NSUInteger row, bool write);
// A pass that stores into `t` (held in its planes) through an RG32Uint image of its texels: the image, a block-linear scratch of
// the command buffer's (backing tag xrPlanesImageTag(k)), merged from the planes first; after the pass, the image split back
// into them. 0, or 1 a pass the driver cannot render, 2 a draw the encoder refused (*why)
constexpr uint32_t kXrPlanesImageTag = 104;
// Past every fixed tag (gpu.h kBackingTags: kFormatStandInTag's 16 are the last): a tag there is still a backing's own
// memory, only not in the device's last-backing cache
constexpr uint32_t kXrPlanesOverflowTag = 128;

// Image k's backing tag: 104 + k up to kFormatStandInTag (112), then 32 + k - 8 between the plane images' (attachment_target.h
// kPlaneImageTag) and the turn copies' (turned_pass.cpp kTurnTag 48), then kXrPlanesOverflowTag + k - 24 for as many as the
// pass writes
constexpr uint32_t xrPlanesImageTag(uint32_t k)
{
    return k < 8 ? kXrPlanesImageTag + k : k < 24 ? 32 + (k - 8) : kXrPlanesOverflowTag + (k - 24);
}

// The images live until their split, after the pass's resolves and YCbCr stores, which read earlier passes' backings
static_assert(kXrPlanesImageTag >= gm20b::kDriverPassColorTag + 8 &&
                  (xrPlanesImageTag(8) >= gm20b::kDriverPassColorTag + 8 ||
                   xrPlanesImageTag(23) < gm20b::kDriverPassColorTag),
              "an XR plane image's tag is a driver pass's");

int mergeXrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, uint32_t k, gm20b::Texture *&image,
                  const char **why);
int splitXrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, const gm20b::Texture &image, bool cleared,
                  const char **why);
