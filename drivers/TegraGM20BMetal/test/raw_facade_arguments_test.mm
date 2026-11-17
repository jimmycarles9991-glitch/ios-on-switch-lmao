// SPDX-License-Identifier: MIT
// The raw facade's argument buffers on the host (raw_facade_harness.h). An encoder of descriptors
// (RawMetalArgumentEncoder.mm) writes what raw::ArgumentEncoder writes over the same layout and refuses what it refuses (a
// function's encoder: raw_facade_buffer_encoders_test); a texture's or sampler's ID names it in the device's handle tables
// from its first ask until it is gone (RawMetalResourceIds.mm).
#include "raw_device_harness.h"
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_argument_encoder.h"
#include "../raw/runtime/raw_heap_fit.h"
#include "../raw/runtime/raw_resource_id.h"
#include "../raw/runtime/raw_sampler_word.h"

namespace {

using facade::at;
using facade::bytesOf;
using facade::filled;
using facade::shared;
using facade::texture;
using facade::word;
using raw::ArgumentKind;

MTLArgumentDescriptor *argument(MTLDataType type, NSUInteger index, NSUInteger length = 0)
{
    MTLArgumentDescriptor *a = [MTLArgumentDescriptor argumentDescriptor];
    a.dataType = type;
    a.index = index;
    a.arrayLength = length;
    return a;
}

// A pointer, three textures, two samplers, a float4, a uint and a texture
NSArray<MTLArgumentDescriptor *> *descriptors()
{
    return @[
        argument(MTLDataTypePointer, 0), argument(MTLDataTypeTexture, 1, 3), argument(MTLDataTypeSampler, 4, 2),
        argument(MTLDataTypeFloat4, 6), argument(MTLDataTypeUInt, 7), argument(MTLDataTypeTexture, 8)
    ];
}

raw::ArgumentLayout layoutOf(const std::vector<raw::ArgumentDescription> &d)
{
    raw::ArgumentLayout layout;
    size_t position = 0;
    EXPECT_EQ(raw::ArgumentLayout::fromDescriptions(d, layout, position), raw::ArgumentLayoutError::None);
    return layout;
}

constexpr raw::ArgumentType kPointer = {ArgumentKind::Buffer, 0, 1, 1}, kTexture = {ArgumentKind::Texture, 0, 1, 1},
                            kSampler = {ArgumentKind::Sampler, 0, 1, 1};

TEST(FacadeArgumentEncoder, WritesEveryArgumentWhereTheLayoutPlacesIt)
{
    @autoreleasepool {
        id<MTLArgumentEncoder> encoder = [facade::device() newArgumentEncoderWithArguments:descriptors()];
        ASSERT_NE(encoder, nil);
        const raw::ArgumentLayout layout = layoutOf({{kPointer, 0, 0, 0},
                                                     {kTexture, 1, 3, 0},
                                                     {kSampler, 4, 2, 0},
                                                     {{ArgumentKind::Constant, 4, 1, 4}, 6, 0, 0},
                                                     {{ArgumentKind::Constant, 4, 1, 1}, 7, 0, 0},
                                                     {kTexture, 8, 0, 0}});
        EXPECT_EQ(encoder.encodedLength, 80u);
        EXPECT_EQ(encoder.alignment, 16u);
        EXPECT_EQ(encoder.encodedLength, layout.encodedLength);
        id<MTLBuffer> arguments = filled(256), data = shared(1024);
        std::vector<uint8_t> want = bytesOf(arguments);
        id<MTLTexture> textures[3] = {texture(), texture(), texture()};
        id<MTLTexture> last = texture();
        id<MTLSamplerState> samplers[2] = {facade::sampler(), facade::sampler()};
        const float colour[4] = {1, 0.5f, 0.25f, 1};
        const uint32_t constant = 0xC0FFEE;
        [encoder setArgumentBuffer:arguments offset:64];
        [encoder setBuffer:data offset:48 atIndex:0];
        [encoder setTextures:textures withRange:NSMakeRange(1, 3)];
        [encoder setSamplerStates:samplers withRange:NSMakeRange(4, 2)];
        memcpy([encoder constantDataAtIndex:6], colour, sizeof colour);
        memcpy([encoder constantDataAtIndex:7], &constant, sizeof constant);
        [encoder setTexture:last atIndex:8];

        raw::ArgumentEncoder ref(layout);
        ASSERT_EQ(ref.setBuffer(want.data(), want.size(), 64, 0), raw::ArgumentRefusal::None);
        EXPECT_EQ(ref.writeBuffer(data.gpuAddress, data.length, 48, 0), raw::ArgumentRefusal::None);
        for (uint32_t k = 0; k < 3; k++)
            ref.write(word([textures[k] gpuResourceID]), 1 + k, ArgumentKind::Texture);
        for (uint32_t k = 0; k < 2; k++)
            ref.write(word([samplers[k] gpuResourceID]), 4 + k, ArgumentKind::Sampler);
        raw::ArgumentRefusal why;
        memcpy(ref.constantData(6, why), colour, sizeof colour);
        memcpy(ref.constantData(7, why), &constant, sizeof constant);
        ref.write(word([last gpuResourceID]), 8, ArgumentKind::Texture);
        EXPECT_EQ(bytesOf(arguments), want);

        EXPECT_EQ(at(arguments, 64), data.gpuAddress + 48);
        EXPECT_EQ(at(arguments, 64 + 16), raw::textureResourceId(((RMTexture *)textures[1])->_raw.tic));
        EXPECT_EQ(at(arguments, 64 + 40), raw::samplerResourceId(((RMSampler *)samplers[1])->_raw.tsc));
        EXPECT_EQ(at(arguments, 64 + 72), raw::textureResourceId(((RMTexture *)last)->_raw.tic));
    }
}

TEST(FacadeArgumentEncoder, ArraysOfBuffersAndConstantsTakeAStrideEach)
{
    @autoreleasepool {
        id<MTLArgumentEncoder> encoder = [facade::device()
            newArgumentEncoderWithArguments:@[ argument(MTLDataTypePointer, 0, 3), argument(MTLDataTypeUInt2, 3, 2) ]];
        ASSERT_NE(encoder, nil);
        const raw::ArgumentLayout layout =
            layoutOf({{kPointer, 0, 3, 0}, {{ArgumentKind::Constant, 4, 1, 2}, 3, 2, 0}});
        EXPECT_EQ(encoder.encodedLength, layout.encodedLength);
        EXPECT_EQ(encoder.encodedLength, 40u);
        id<MTLBuffer> arguments = filled(128), a = shared(64), b = shared(64), c = shared(64);
        const id<MTLBuffer> three[3] = {a, b, c};
        const NSUInteger offsets[3] = {0, 16, 32};
        [encoder setArgumentBuffer:arguments offset:0];
        [encoder setBuffers:three offsets:offsets withRange:NSMakeRange(0, 3)];
        const uint32_t pairs[4] = {1, 2, 3, 4};
        memcpy([encoder constantDataAtIndex:3], pairs, sizeof pairs);
        EXPECT_EQ(at(arguments, 0), a.gpuAddress);
        EXPECT_EQ(at(arguments, 8), b.gpuAddress + 16);
        EXPECT_EQ(at(arguments, 16), c.gpuAddress + 32);
        EXPECT_EQ(memcmp(static_cast<const uint8_t *>(arguments.contents) + 24, pairs, sizeof pairs), 0);
        EXPECT_EQ(static_cast<uint8_t *>([encoder constantDataAtIndex:4]),
                  static_cast<uint8_t *>(arguments.contents) + 32);
        [encoder setBuffer:nil offset:0 atIndex:1];
        EXPECT_EQ(at(arguments, 8), 0u) << "a nil buffer is a null pointer";
    }
}

TEST(FacadeArgumentEncoder, ARangeWithOneOffsetPastItsBufferWritesNone)
{
    @autoreleasepool {
        id<MTLArgumentEncoder> encoder =
            [facade::device() newArgumentEncoderWithArguments:@[ argument(MTLDataTypePointer, 0, 3) ]];
        id<MTLBuffer> arguments = filled(64), a = shared(64), b = shared(64);
        [encoder setArgumentBuffer:arguments offset:0];
        const std::vector<uint8_t> untouched = bytesOf(arguments);
        const id<MTLBuffer> three[3] = {a, b, a};
        const NSUInteger offsets[3] = {0, 64, 8};
        [encoder setBuffers:three offsets:offsets withRange:NSMakeRange(0, 3)];
        EXPECT_EQ(bytesOf(arguments), untouched) << "b's offset is its length";
        const NSUInteger inside[3] = {0, 63, 8};
        [encoder setBuffers:three offsets:inside withRange:NSMakeRange(0, 3)];
        EXPECT_EQ(at(arguments, 8), b.gpuAddress + 63);
        EXPECT_EQ(at(arguments, 16), a.gpuAddress + 8);
    }
}

TEST(FacadeArgumentEncoder, RefusesWhatMetalsValidationStopsAndWritesNothing)
{
    @autoreleasepool {
        id<MTLArgumentEncoder> encoder = [facade::device() newArgumentEncoderWithArguments:descriptors()];
        id<MTLBuffer> arguments = filled(256), data = shared(1024);
        id<MTLTexture> t = texture();
        [encoder setTexture:t atIndex:1];
        EXPECT_EQ([encoder constantDataAtIndex:6], nullptr) << "no argument buffer yet";
        [encoder setArgumentBuffer:arguments offset:0];
        const std::vector<uint8_t> untouched = bytesOf(arguments);
        [encoder setBuffer:data offset:1024 atIndex:0];
        [encoder setTexture:t atIndex:4];
        const id<MTLBuffer> two[2] = {data, data};
        const NSUInteger offsets[2] = {0, 0};
        [encoder setBuffers:two offsets:offsets withRange:NSMakeRange(0, 2)];
        EXPECT_EQ(bytesOf(arguments), untouched);

        [encoder setArgumentBuffer:arguments offset:200];
        [encoder setTexture:t atIndex:1];
        EXPECT_EQ(bytesOf(arguments), untouched) << "the encoded length passes the buffer's end";
        id<MTLBuffer> hidden = [facade::device() newBufferWithLength:256 options:MTLResourceStorageModePrivate];
        [encoder setArgumentBuffer:hidden offset:0];
        [encoder setTexture:t atIndex:1];
        EXPECT_EQ(bytesOf(arguments), untouched);
        EXPECT_EQ([encoder newArgumentEncoderForBufferAtIndex:0], nil) << "a layout of descriptors names no other";

        [encoder setArgumentBuffer:arguments startOffset:0 arrayElement:2];
        [encoder setTexture:t atIndex:1];
        EXPECT_EQ(at(arguments, 2 * encoder.encodedLength + 8), word([t gpuResourceID]));
    }
}

TEST(FacadeResourceIds, AnIdNamesItsObjectFromItsFirstAskUntilItIsGone)
{
    const raw::HeapTables &tables = raw::DeviceHarness::tables(*gRM->raw);
    uint32_t tic = 0, tsc = 0;
    @autoreleasepool {
        id<MTLTexture> t = texture(MTLStorageModePrivate);
        id<MTLSamplerState> s = facade::sampler(false);
        tic = ((RMTexture *)t)->_raw.tic;
        tsc = ((RMSampler *)s)->_raw.tsc;
        ASSERT_NE(tic, 0u);
        ASSERT_NE(tsc, 0u);
        if (tables.attached())
            EXPECT_EQ(tables.textureEntryAt(tic).element, 0u) << "made, never asked";
        EXPECT_EQ(word([t gpuResourceID]), raw::textureResourceId(tic));
        EXPECT_EQ(word([t gpuResourceID]), raw::textureResourceId(tic));
        ASSERT_TRUE(tables.attached());
        EXPECT_EQ(tables.textureEntryAt(tic).element, tic);
        const MTLTextureSwizzleChannels swizzle = t.swizzle;
        const raw::HeapTextureFacts facts = raw::heapFacts(
            {uint32_t(t.pixelFormat),
             uint32_t(t.textureType),
             uint32_t(t.sampleCount),
             uint32_t(t.usage),
             uint32_t(t.arrayLength),
             uint32_t(t.mipmapLevelCount),
             {uint8_t(swizzle.red), uint8_t(swizzle.green), uint8_t(swizzle.blue), uint8_t(swizzle.alpha)}});
        EXPECT_NE(raw::heapTextureFlags(facts), 0u);
        EXPECT_EQ(tables.textureEntryAt(tic).flags, raw::heapTextureFlags(facts));

        EXPECT_EQ(word([s gpuResourceID]), raw::samplerResourceId(tsc));
        EXPECT_EQ(tables.samplerEntryAt(tsc).element, tsc);
        MTLSamplerDescriptor *d = ((RMSampler *)s)->_descriptor;
        const raw::SamplerFacts sampler = {d.minFilter,     d.magFilter,       d.mipFilter,   d.sAddressMode,
                                           d.tAddressMode,  d.rAddressMode,    d.borderColor, d.normalizedCoordinates,
                                           d.maxAnisotropy, d.compareFunction, d.lodAverage,  d.lodMinClamp,
                                           d.lodMaxClamp};
        EXPECT_EQ(tables.samplerEntryAt(tsc).flags, raw::samplerWord(sampler));
        EXPECT_NE(((RMSampler *)s)->_normalized.tsc, 0u) << "a pixel-coordinate sampler's heap holds its twin";
    }
    EXPECT_EQ(tables.textureEntryAt(tic).element, 0u) << "the texture is gone";
    EXPECT_EQ(tables.textureEntryAt(tic).flags, 0u);
    EXPECT_EQ(tables.samplerEntryAt(tsc).element, 0u) << "the sampler is gone";
}

TEST(FacadeResourceIds, AnEncodersSetAndAHeapsTextureAskToo)
{
    const raw::HeapTables &tables = raw::DeviceHarness::tables(*gRM->raw);
    uint32_t unasked = 0, placed = 0;
    @autoreleasepool {
        id<MTLTexture> t = texture(MTLStorageModePrivate);
        unasked = ((RMTexture *)t)->_raw.tic;
        id<MTLArgumentEncoder> encoder = [facade::device() newArgumentEncoderWithArguments:descriptors()];
        id<MTLBuffer> arguments = shared(128);
        [encoder setArgumentBuffer:arguments offset:0];
        [encoder setTexture:t atIndex:8];
        EXPECT_EQ(tables.textureEntryAt(unasked).element, unasked) << "an encoder's set asks for the ID";

        id<MTLHeap> heap = facade::heap(MTLStorageModePrivate, 4u << 20);
        id<MTLTexture> inHeap = texture(MTLStorageModePrivate, heap);
        ASSERT_NE(inHeap, nil);
        ASSERT_TRUE(((RMTexture *)inHeap)->_raw.arena);
        placed = ((RMTexture *)inHeap)->_raw.tic;
        EXPECT_EQ(word([inHeap gpuResourceID]), raw::textureResourceId(placed));
        EXPECT_EQ(tables.textureEntryAt(placed).element, placed);
    }
    EXPECT_EQ(tables.textureEntryAt(unasked).element, 0u);
    EXPECT_EQ(tables.textureEntryAt(placed).element, 0u) << "a heap's texture is forgotten with it";
}

}  // namespace
