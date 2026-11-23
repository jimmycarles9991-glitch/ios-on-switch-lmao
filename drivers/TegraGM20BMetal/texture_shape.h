// SPDX-License-Identifier: MIT
// What newTextureWithDescriptor: makes of a descriptor (TegraGM20BMetalDevice.mm gm20bNewTexture, TegraGM20BMetalHeap.mm heapTextureAsk):
// nil where Metal, this build's identity or GM20B makes none, else the raw creator that lays it out. Plain values, so a host
// test walks every shape (test/device_shapes_test.cpp).
#pragma once
#include <cstdint>
#include "device_answers.h"
#include "surface_image.h"

namespace gm20b {
class Device;
struct Texture;
}

namespace gm20b::shapes {

// MTLStorageMode's and MTLTextureUsage's values
enum : uint32_t {
    kShared = 0,
    kManaged = 1,
    kPrivate = 2,
    kMemoryless = 3
};

enum : uint64_t {
    kUsageRead = 1,
    kUsageWrite = 2,
    kUsageTarget = 4
};

struct TextureShape {
    uint32_t type = kType2D, format = 0, storage = kShared;
    uint64_t usage = 0, width = 1, height = 1, depth = 1, levels = 1, samples = 1, array = 1, rotation = 0;
    bool ownMemory = false;  // newTextureWithDescriptor:'s: memory the driver lays out, not a buffer's or a surface's
};

enum class StoredSamples : uint8_t {
    None,
    Colour,
    Zeta
};

// gm20bNewTexture's creator of a shape: Device::createMipTexture, createBlockTexture, createLine, createStoredMultisample or
// createOwnedZeta, createVolume, createArray, createOwnedZeta, createMemoryless, createTexture
enum class Route : uint8_t {
    None,
    Chain,
    Block,
    Line,
    StoredSamples,
    Volume,
    Array,
    StoredZeta,
    Memoryless,
    Plain
};

bool keptFormat(const TextureShape &s);
bool gm20bShape(const TextureShape &s);
bool chainShape(const TextureShape &s);
bool lineShape(const TextureShape &s);
bool volumeRendered(const TextureShape &s);
bool volumeShape(const TextureShape &s);
bool volumeView2d(const TextureShape &s);
uint32_t arrayType(const TextureShape &s);
bool zetaShape(const TextureShape &s);
bool multisampleShape(const TextureShape &s);
uint32_t memorylessLayers(const TextureShape &s);
bool blockShape(const TextureShape &s);
StoredSamples storedSamples(const TextureShape &s);
// Which gm20bShape term fails (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format; 0 none)
uint32_t gm20bShapeTerm(const TextureShape &s);
// A decoded format's (decoded.h) shape as the image it is held in: its stored format, private (block-linear), laid out
// as a render target, since its decode passes draw into it; any other shape as it is. route and create take it so.
TextureShape heldShape(const TextureShape &s);
Route route(const TextureShape &s);
// The texture header type (fx::kTexOneD to kTexThreeD) of MTLTextureType `type`
uint8_t headerType(uint32_t type);
// `s` laid out by `route` in `d`'s memory (gm20bNewTexture's creators); `compressedKind`: the compressible kind
// (private_comp.h) of a private plain texture, mip chain, 2D array or stored multisampled one, 0 for none
bool create(Device &d, const TextureShape &s, Route route, bool tiled, uint32_t compressedKind, Texture &t);

// Why Metal makes no texture of `format` by any route (descriptor, view, buffer or surface), null where its format allows one
const char *formatRefusal(uint32_t format);
// The rule by which Metal makes no texture of `s` on any device (its validation's stop or its nil), null where it makes one
const char *metalRefusal(const TextureShape &s);
// The rule by which identity `k`'s Metal makes none (its answers in device_answers.h), null where it makes one
const char *identityRefusal(const TextureShape &s, const answers::Kind &k);
// Why GM20B holds no texture of `s` that the identity makes, null where it holds one
const char *hardwareRefusal(const TextureShape &s);
// Why a decoded format's texture is made over no IOSurface or buffer, null for any other format
const char *decodedSurfaceRefusal(uint32_t format);
// Why neither identity's Metal makes a texture of `format` over an IOSurface, null where one does
const char *surfaceFormatRefusal(uint32_t format);

}  // namespace gm20b::shapes
