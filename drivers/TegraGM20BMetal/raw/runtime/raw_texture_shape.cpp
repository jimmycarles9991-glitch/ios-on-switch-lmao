// SPDX-License-Identifier: MIT
#include "raw_texture_shape.h"
#include <algorithm>
#include "raw_gpu.h"
#include "raw_srgb_pair.h"
#include "raw_xr.h"
#include "raw_ycbcr.h"
#include "raw_zeta.h"

namespace raw::shapes {

namespace {

bool extended(uint32_t format)
{
    return xrForm(format) != XrForm::None;
}

bool zetaFormat(uint32_t format)
{
    return zetaFormatIndex(format) < kZetaFormats;
}

constexpr uint32_t kD24S8 = 255, kX32S8 = 261, kX24S8 = 262, kGBGR422 = 240, kBGRG422 = 241;

bool pvrtc(uint32_t format)
{
    return format >= 160 && format <= 167;
}

// ETC2 and EAC: blocks of one plane, which no 3D texture takes
bool flatBlocks(uint32_t format)
{
    return (format >= 170 && format <= 183) || pvrtc(format);
}

bool astcHdr(uint32_t format)
{
    return format >= 222 && format <= 236;
}

bool compressed(uint32_t format)
{
    return blockCompressed(formatInfo(format)) || pvrtc(format) || astcHdr(format);
}

bool fourTwoTwo(uint32_t format)
{
    return format == kGBGR422 || format == kBGRG422;
}

bool depthStencil(uint32_t format)
{
    return zetaFormat(format) || format == kD24S8 || format == kX32S8 || format == kX24S8;
}

constexpr uint32_t kTypeBuffer = 9;

bool multisampled(const TextureShape &s)
{
    return s.type == kType2DMultisample || s.type == kType2DMultisampleArray;
}

bool oneDimensional(const TextureShape &s)
{
    return s.type == kType1D || s.type == kType1DArray;
}

uint64_t fullChain(const TextureShape &s)
{
    const uint64_t side = std::max({s.width, s.height, s.type == kType3D ? s.depth : 1, uint64_t(1)});
    uint64_t levels = 0;
    for (uint64_t v = side; v; v >>= 1)
        levels++;
    return levels;
}

}  // namespace

// A format the raw path has a header for. RGB10A8_2P_XR10 private, memoryless or of its own memory: its texels are
// m2v's own layout (raw_xr.h), whose colour words Metal's CPU copies move (cpuXrWords) as its blits do, never a
// buffer's or a surface's bytes; one over a two-plane surface's planes is taken before this (xrPlanesShape)
bool keptFormat(const TextureShape &s)
{
    if (xrForm(s.format) == XrForm::Planes && !s.ownMemory && s.storage != kPrivate && s.storage != kMemoryless)
        return false;
    // RG8Unorm_sRGB samples through a stand-in, which a texture of a buffer's or a surface's bytes has none of
    if (srgbPair(s.format) && !s.ownMemory)
        return false;
    // a block-compressed texture is private: written and read by the copy engine in whole blocks, never by the CPU
    const FormatInfo *f = formatInfo(s.format);
    return f && (!blockCompressed(f) || s.storage == kPrivate);
}

// A texture the raw path keeps: 2D, one level, one sample, a format it has a header for, not rotated (the descriptor's
// rotation SPI: Core Animation's window-server surfaces on a rotated display, CA::OGL::MetalContext::get_surface; m2v
// renders those rotated)
bool rawShape(const TextureShape &s)
{
    return s.type == kType2D && s.levels <= 1 && s.samples <= 1 && s.array <= 1 && s.depth <= 1 && !s.rotation &&
           keptFormat(s);
}

// A mipmapped texture the raw path keeps as a block-linear chain of its levels (raw::Device::createMipTexture, raw/fx):
// 2D, one sample, one layer, no rotation, of a format the raw path holds as it is (an extended-range one as its codes,
// with a stand-in of its levels; one it renders none of is laid out alike, and a pass into a level or generateMipmaps
// refuses it by its own predicate). Private,
// memoryless (r7r: Core Animation makes 776x24 mipmapped surfaces memoryless too, kept here as private ones, a
// superset) or shared: CA::OGL::MetalContext::update_surface copies IOSurface content it draws with a trilinear
// minification filter into a texture of the surface texture's descriptor (shared) with every level, then generates them
// (on the 18:13 boot of 11-06, 49 command buffers of a 1280x720 one went to m2v). A shared one's CPU access reads and
// writes the chain (cpuLevel).
bool chainShape(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    return s.type == kType2D && s.levels > 1 && s.samples <= 1 && s.array <= 1 && s.depth <= 1 && !s.rotation &&
           (s.storage == kPrivate || s.storage == kMemoryless || s.storage == kShared) && f && !blockCompressed(f);
}

// A 1D texture the raw path keeps (raw::Device::createLine): one level, of a format it has a header for. Memory of its
// own only: a 1D header has no pitch layout, which a buffer's or a surface's rows are.
bool lineShape(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    return s.type == kType1D && s.levels <= 1 && s.samples <= 1 && s.array <= 1 && s.height <= 1 && s.depth <= 1 &&
           s.storage != kMemoryless && f && !blockCompressed(f);
}

// m2v makes a 3D texture it may render into 2D-array compatible (image_plan.rs render_3d_flags), which NIL lays out one
// plane a block; Unknown usage is every usage (texture_descriptor.rs effective_usage). A mipmapped one is laid out so
// too: generateMipmapsForTexture: draws each level's planes (mipVolumePasses), and GM20B's colour target names no plane
// inside a block of several
bool volumeRendered(const TextureShape &s)
{
    return !s.usage || (s.usage & kUsageTarget) || s.levels > 1;
}

// A 3D texture the raw path keeps (raw::Device::createVolume): of a format it has a header for, memory of its own, any
// levels
bool volumeShape(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    return s.type == kType3D && s.samples <= 1 && s.array <= 1 && s.storage != kMemoryless && f && !blockCompressed(f);
}

// Laid out one plane a block, as a render target: m2v's twin is 2D-array compatible exactly where NVK renders the
// format
bool volumeView2d(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    return volumeRendered(s) && f && f->rt;
}

// A 2D array, cube or cube array the raw path keeps (raw::Device::createArray): one sample, of a format it has a header
// for, memory of its own, any levels; its header's type (fx::kTexTwoDArray, kTexCube, kTexCubeArray), 0: none
uint32_t arrayType(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    const bool kept = s.samples <= 1 && s.depth <= 1 && s.storage != kMemoryless && f && !blockCompressed(f);
    if (!kept)
        return 0;
    switch (s.type) {
    case kType1DArray:
        return s.height <= 1 ? fx::kTexOneDArray : 0;
    case kType2DArray:
        return fx::kTexTwoDArray;
    case kTypeCube:
        return s.width == s.height && s.array <= 1 ? fx::kTexCube : 0;
    case kTypeCubeArray:
        return s.width == s.height ? fx::kTexCubeArray : 0;
    default:
        return 0;
    }
}

// A depth/stencil texture of a format the raw path renders (raw_zeta.h kZetaFormatInfo), no rotation: memoryless, one
// level, 2D of one sample or 2D multisampled of 2 or 4; or stored (private or shared: the CPU's copies through the copy
// engine, raw_zeta_cpu.h) in the raw path's own zeta memory (createOwnedZeta), of any usage and levels, 2D of one
// sample, a 2D array, cube or cube array rendered there a slice at a time or layered (a pass into a level past the
// first refuses at its own predicate), moved into NVK's image only if a replay needs m2v's twin
bool zetaShape(const TextureShape &s)
{
    if (!zetaFormat(s.format) || s.rotation || s.depth > 1)
        return false;
    const bool flat = s.type == kType2D && s.samples <= 1 && s.array <= 1;
    if (s.storage == kMemoryless)
        return (flat || (s.type == kType2DMultisample && (s.samples == 2 || s.samples == 4) && s.array <= 1)) &&
               s.levels <= 1;
    const bool layers =
        (s.type == kType2DArray || s.type == kTypeCubeArray || (s.type == kTypeCube && s.array <= 1)) && s.samples <= 1;
    return (s.storage == kPrivate || s.storage == kShared) && (flat || layers);
}

// A memoryless multisampled color texture, 2D, one level, 2 or 4 samples (the counts the device answers yes for), no
// rotation, of a format the raw path has a header for, not extended range (raw::Device::createMemoryless with its
// samples; an extended-range one is held as private)
bool multisampleShape(const TextureShape &s)
{
    return s.type == kType2DMultisample && s.levels <= 1 && s.array <= 1 && s.depth <= 1 &&
           (s.samples == 2 || s.samples == 4) && !s.rotation && s.storage == kMemoryless && keptFormat(s) &&
           !extended(s.format);
}

// A memoryless 2D array or cube, one sample and one level, no rotation, of a depth/stencil format the raw path renders
// or a color format it holds as it is (raw::Device::createMemoryless with its layers; an extended-range one is held as
// private): its layers, 0: none
uint32_t memorylessLayers(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    const bool target = zetaFormat(s.format) || (f && !blockCompressed(f) && !extended(s.format));
    const bool layered = s.type == kType2DArray || s.type == kTypeCube;
    if (s.storage != kMemoryless || !layered || s.samples > 1 || s.levels > 1 || s.depth > 1 || s.rotation || !target)
        return 0;
    return textureLayers(s.type, uint32_t(std::min<uint64_t>(s.array, 2048)));
}

// A block-compressed 2D texture, 2D array, cube, cube array or 3D texture (BC and ASTC: blocks one plane deep, as Metal
// holds them) the raw path keeps in block-linear memory of its own (raw::Device::createBlockTexture): any levels,
// layers and planes, private or shared (the CPU's copies move whole blocks of NIL's image), one sample, no rotation; a
// private one of one level and one layer is rawShape's
bool blockShape(const TextureShape &s)
{
    const FormatInfo *f = formatInfo(s.format);
    const bool layered = s.type == kType2DArray || s.type == kTypeCube || s.type == kTypeCubeArray;
    const bool planes = s.type == kType3D ? !flatBlocks(s.format) : s.depth <= 1;
    return f && blockCompressed(f) && (s.type == kType2D || layered || s.type == kType3D) && planes && s.samples <= 1 &&
           !s.rotation && (s.storage == kPrivate || s.storage == kShared) && !rawShape(s);
}

// A stored multisampled texture the raw path keeps: private or shared (the CPU's copies: cpuSamples), 2D multisampled
// at a count the device answers yes for (2, 4), one level, no rotation; a colour format it holds as it is
// (Device::createStoredMultisample; one it renders none of is laid out alike, and a pass into it refuses it), or a
// depth/stencil one of any usage (Device::createOwnedZeta; a shared one's CPU copies through the copy engine)
StoredSamples storedSamples(const TextureShape &s)
{
    if (s.type != kType2DMultisample || (s.samples != 2 && s.samples != 4) || s.levels > 1 || s.array > 1 ||
        s.depth > 1 || s.rotation || (s.storage != kPrivate && s.storage != kShared))
        return StoredSamples::None;
    if (zetaFormat(s.format))
        return StoredSamples::Zeta;
    const FormatInfo *f = formatInfo(s.format);
    return f && !blockCompressed(f) ? StoredSamples::Colour : StoredSamples::None;
}

uint32_t rawShapeTerm(const TextureShape &s)
{
    return s.type != kType2D              ? 1
           : s.levels > 1                 ? 2
           : s.samples > 1                ? 3
           : (s.array > 1 || s.depth > 1) ? 4
           : s.rotation                   ? 5
           : !keptFormat(s)               ? 6
                                          : 0;
}

Route route(const TextureShape &s)
{
    if (chainShape(s))
        return Route::Chain;
    if (blockShape(s))
        return Route::Block;
    if (lineShape(s))
        return Route::Line;
    if (storedSamples(s) != StoredSamples::None)
        return Route::StoredSamples;
    if (volumeShape(s))
        return Route::Volume;
    if (arrayType(s))
        return Route::Array;
    const bool zeta = zetaShape(s);
    if (s.storage != kMemoryless && zeta)
        return Route::StoredZeta;
    if (s.storage == kMemoryless && (rawShape(s) || zeta || multisampleShape(s) || memorylessLayers(s)))
        return Route::Memoryless;
    if (s.storage == kMemoryless) {
        // a memoryless shape no pass backing takes (1D, 3D, mipmapped, a cube array, blocks): held as a private one, as
        // a memoryless 2D chain is, a superset of what Metal keeps of it
        TextureShape held = s;
        held.storage = kPrivate;
        return route(held);
    }
    return rawShape(s) ? Route::Plain : Route::None;
}

uint8_t headerType(uint32_t type)
{
    switch (type) {
    case kType1D:
        return fx::kTexOneD;
    case kType1DArray:
        return fx::kTexOneDArray;
    case kType2DArray:
        return fx::kTexTwoDArray;
    case kTypeCube:
        return fx::kTexCube;
    case kTypeCubeArray:
        return fx::kTexCubeArray;
    case kType3D:
        return fx::kTexThreeD;
    default:
        return fx::kTexTwoD;
    }
}

bool create(Device &d, const TextureShape &s, Route route, bool tiled, uint32_t compressedKind, Texture &t)
{
    const uint32_t w = uint32_t(s.width), h = uint32_t(s.height), f = s.format;
    const uint32_t levels = uint32_t(std::max<uint64_t>(s.levels, 1)),
                   samples = uint32_t(std::max<uint64_t>(s.samples, 1));
    const uint32_t layers = textureLayers(s.type, uint32_t(std::min<uint64_t>(s.array, 2048)));
    switch (route) {
    case Route::Chain:
        return d.createMipTexture(w, h, f, levels, t, nullptr, compressedKind);
    case Route::Block:
        return d.createBlockTexture(w, h, levels, layers, f, headerType(s.type), t, uint32_t(s.depth));
    case Route::Line:
        return d.createLine(w, f, t);
    case Route::StoredSamples:
        return storedSamples(s) == StoredSamples::Zeta
                   ? d.createOwnedZeta(w, h, f, samples, t)
                   : d.createStoredMultisample(w, h, f, samples, t, nullptr, compressedKind);
    case Route::Volume:
        return d.createVolume(w, h, uint32_t(s.depth), f, volumeView2d(s), t, levels);
    case Route::Array:
        return d.createArray(w, h, layers, f, arrayType(s), t, levels, nullptr, compressedKind);
    case Route::StoredZeta:
        return d.createOwnedZeta(w, h, f, 1, !s.usage || (s.usage & kUsageRead), t, layers, headerType(s.type), levels);
    case Route::Memoryless:
        return d.createMemoryless(w, h, f, t, samples, std::max(memorylessLayers(s), 1u)) &&
               (xrForm(f) != XrForm::Planes || d.createXrMemoryless(t));
    case Route::Plain:
        return (compressedKind && d.createCompressedTexture(w, h, f, compressedKind, t)) ||
               d.createTexture(w, h, f, t, s.storage == kShared, tiled || s.storage == kMemoryless);
    case Route::None:
        break;
    }
    return false;
}

// Metal's texture descriptor rules as an M4 Max applies them (newTextureWithDescriptor: of 16,884 shapes the raw path
// left to m2v, m2vf-targets2's probe; metal2vulkan-runtime texture_descriptor.rs check_made, its EVs), validation's
// stops first, then its nils
const char *metalRefusal(const TextureShape &s)
{
    const bool ms = multisampled(s), line = oneDimensional(s), buffer = s.type == kTypeBuffer;
    if (ms && (isYcbcrFormat(s.format) || xrForm(s.format) == XrForm::Planes))
        return "a two-plane format takes no multisampled texture";
    if (s.format == kX24S8)
        return "X24_Stencil8 is a view's format alone";
    if (depthStencil(s.format) && (line || buffer || s.type == kType3D))
        return "a depth/stencil format takes no 1D, 3D or buffer texture";
    if (fourTwoTwo(s.format) && s.type != kType2D)
        return "a 4:2:2 format takes a 2D texture alone";
    if (compressed(s.format) && (line || ms || (flatBlocks(s.format) && s.type == kType3D)))
        return "a block-compressed format takes no 1D or multisampled texture, and ETC, EAC and PVRTC no 3D one";
    if (!s.width || !s.height || !s.depth)
        return "a side of 0";
    if (((line || buffer) && s.height != 1) || (s.type != kType3D && s.depth != 1))
        return "a height other than 1 of a 1D or buffer texture or a depth other than 1 of one not 3D";
    const uint64_t side = s.type == kType3D ? 2048 : s.type == kTypeBuffer ? 1ull << 28 : 16384;
    if (s.width > side || s.height > side || s.depth > side)
        return "a side past the type's largest";
    if ((s.type == kTypeCube || s.type == kTypeCubeArray) && s.width != s.height)
        return "a cube not square";
    if ((fourTwoTwo(s.format) && (s.width & 1)) ||
        (pvrtc(s.format) && (s.width != s.height || (s.width & (s.width - 1)))))
        return "an extent the format's blocks do not take";
    const bool arrayed = s.type == kType1DArray || s.type == kType2DArray || s.type == kTypeCubeArray ||
                         s.type == kType2DMultisampleArray;
    if ((!arrayed && s.array != 1) || !s.array || s.array > (s.type == kTypeCubeArray ? 341u : 2048u))
        return "an array length the type does not take";
    if (!s.levels || s.levels > fullChain(s) || ((ms || line || fourTwoTwo(s.format)) && s.levels != 1))
        return "a level count past the chain, or other than 1 of a multisampled, 1D or 4:2:2 texture";
    if (!s.samples || (ms ? s.samples == 1 : s.samples != 1))
        return "a sample count the type does not take";
    if (xrForm(s.format) == XrForm::Planes && (s.type == kTypeCube || s.type == kTypeCubeArray) &&
        s.storage != kMemoryless)
        return "nil: RGB10A8_2P_XR10 makes a memoryless cube alone";
    if (isYcbcrFormat(s.format))
        return "nil: a YCbCr format's texture is an IOSurface plane's alone";
    return nullptr;
}

const char *identityRefusal(const TextureShape &s, const answers::Kind &k)
{
    if (s.type == kType2DMultisampleArray && !answers::kindCount(k, "supportsTexture2DMultisampleArray"))
        return "supportsTexture2DMultisampleArray answers NO";
    if (multisampled(s) && !answers::kindAnswer(k, "supportsTextureSampleCount:", s.samples))
        return "supportsTextureSampleCount: answers NO for its count";
    if (s.type == kTypeCubeArray && !k.cubeArrays)
        return "no cube map texture arrays below Apple4 (Apple's Metal feature set tables)";
    if (s.format == kD24S8 && !answers::kindCount(k, "isDepth24Stencil8PixelFormatSupported"))
        return "isDepth24Stencil8PixelFormatSupported answers NO";
    if (astcHdr(s.format) && !k.astcHdr)
        return "no ASTC HDR below Apple6 (Apple's Metal feature set tables)";
    return nullptr;
}

const char *hardwareRefusal(const TextureShape &s)
{
    if (pvrtc(s.format))
        return "no GM20B texture format decodes PVRTC (NIL's nil_formats.csv lists none)";
    if (astcHdr(s.format))
        return "GM20B's texture unit decodes ASTC LDR alone (clb097tex.h names no HDR form; nil_formats.csv's "
               "ASTC rows are UNORM and sRGB)";
    return nullptr;
}

}  // namespace raw::shapes
