// SPDX-License-Identifier: MIT
// The raw facade's argument buffers on the host (raw_facade_harness.h). An encoder of descriptors
// (RawMetalArgumentEncoder.mm) writes what raw::ArgumentEncoder writes over the same layout and refuses what it refuses; a
// function's encoder and its nested ones take the bridge's runs (RawMetalLibrary.mm); a texture's or sampler's ID names it
// in the device's handle tables from its first ask until it is gone (RawMetalResourceIds.mm).
#include "raw_device_harness.h"
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_argument_encoder.h"
#include "../raw/runtime/raw_resource_id.h"

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

TEST(FacadeArgumentEncoder, AFunctionsEncoderAndItsNestedOnesTakeTheBridgesRuns)
{
    @autoreleasepool {
        id twin = [NSObject new];
        RMFunction *function = [RMFunction new];
        function->_device = facade::device();
        function->_twin = twin;
        const uint32_t buffer = uint32_t(ArgumentKind::Buffer), tex = uint32_t(ArgumentKind::Texture);
        const uint32_t constant = uint32_t(ArgumentKind::Constant);
        standin::Layout outer{24, 8, {{0, 1, 1, 1, buffer, 2, 0, 0, 0, 8}, {1, 2, 1, 1, tex, 0, 8, 8, 0, 8}}};
        standin::Layout inner{32,
                              16,
                              {{0, 1, 1, 1, tex, 0, 0, 0, 0, 8},
                               {1, 1, 1, 1, buffer, 0, 8, 0, 0, 8},
                               {2, 1, 1, 1, constant, 0, 16, 0, 0, 16}}};
        standin::functionLayouts(twin, 0, {outer, inner});

        id<MTLArgumentEncoder> encoder = [(id)function newArgumentEncoderWithBufferIndex:0];
        ASSERT_NE(encoder, nil);
        EXPECT_EQ(encoder.encodedLength, 24u);
        EXPECT_EQ(encoder.alignment, 8u);
        EXPECT_EQ([(id)function newArgumentEncoderWithBufferIndex:3], nil) << "no argument buffer there";
        MTLArgument *reflected = (MTLArgument *)twin;
        EXPECT_NE([(id)function newArgumentEncoderWithBufferIndex:0 reflection:&reflected], nil);
        EXPECT_EQ(reflected, nil);

        id<MTLArgumentEncoder> nested = [encoder newArgumentEncoderForBufferAtIndex:0];
        ASSERT_NE(nested, nil);
        EXPECT_EQ(nested.encodedLength, 32u);
        EXPECT_EQ(nested.alignment, 16u);
        EXPECT_EQ(nested.device, facade::device());
        EXPECT_EQ([encoder newArgumentEncoderForBufferAtIndex:1], nil) << "a texture";
        EXPECT_EQ([nested newArgumentEncoderForBufferAtIndex:1], nil) << "a pointer to plain memory";

        id<MTLBuffer> top = filled(64), below = filled(64), data = shared(256);
        std::vector<uint8_t> wantTop = bytesOf(top), wantBelow = bytesOf(below);
        id<MTLTexture> a = texture(), b = texture(), c = texture();
        const id<MTLTexture> pair[2] = {a, b};
        const float colour[4] = {0, 1, 0, 1};
        [encoder setArgumentBuffer:top offset:0];
        [encoder setBuffer:below offset:0 atIndex:0];
        [encoder setTextures:pair withRange:NSMakeRange(1, 2)];
        [nested setArgumentBuffer:below offset:0];
        [nested setTexture:c atIndex:0];
        [nested setBuffer:data offset:16 atIndex:1];
        memcpy([nested constantDataAtIndex:2], colour, sizeof colour);
        EXPECT_EQ(at(top, 0), below.gpuAddress);
        EXPECT_EQ(at(top, 8), word([a gpuResourceID]));
        EXPECT_EQ(at(top, 16), word([b gpuResourceID]));
        EXPECT_EQ(at(below, 0), word([c gpuResourceID]));
        EXPECT_EQ(at(below, 8), data.gpuAddress + 16);
        EXPECT_EQ(memcmp(static_cast<const uint8_t *>(below.contents) + 16, colour, sizeof colour), 0);
        [nested setTexture:c atIndex:1];
        EXPECT_EQ(at(below, 8), data.gpuAddress + 16) << "a texture at a buffer's index";

        const raw::ArgumentRuns runs[2] = {
            {outer.encodedLength, outer.alignment, outer.runs.data(), uint32_t(outer.runs.size())},
            {inner.encodedLength, inner.alignment, inner.runs.data(), uint32_t(inner.runs.size())}};
        raw::ArgumentEncoder ref, refNested;
        ASSERT_TRUE(raw::ArgumentEncoder::fromRuns(runs, 2, ref));
        ASSERT_TRUE(ref.nested(0, refNested));
        ASSERT_EQ(ref.setBuffer(wantTop.data(), wantTop.size(), 0, 0), raw::ArgumentRefusal::None);
        EXPECT_EQ(ref.writeBuffer(below.gpuAddress, below.length, 0, 0), raw::ArgumentRefusal::None);
        EXPECT_EQ(ref.write(word([a gpuResourceID]), 1, ArgumentKind::Texture), raw::ArgumentRefusal::None);
        EXPECT_EQ(ref.write(word([b gpuResourceID]), 2, ArgumentKind::Texture), raw::ArgumentRefusal::None);
        ASSERT_EQ(refNested.setBuffer(wantBelow.data(), wantBelow.size(), 0, 0), raw::ArgumentRefusal::None);
        EXPECT_EQ(refNested.write(word([c gpuResourceID]), 0, ArgumentKind::Texture), raw::ArgumentRefusal::None);
        EXPECT_EQ(refNested.writeBuffer(data.gpuAddress, data.length, 16, 1), raw::ArgumentRefusal::None);
        raw::ArgumentRefusal why;
        memcpy(refNested.constantData(2, why), colour, sizeof colour);
        EXPECT_EQ(bytesOf(top), wantTop);
        EXPECT_EQ(bytesOf(below), wantBelow);
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
        const standin::Asks before = standin::asks();
        EXPECT_EQ(word([t gpuResourceID]), raw::textureResourceId(tic));
        EXPECT_EQ(word([t gpuResourceID]), raw::textureResourceId(tic));
        EXPECT_EQ(standin::asks().textureFlags, before.textureFlags + 1) << "one ask names it";
        ASSERT_TRUE(tables.attached());
        EXPECT_EQ(tables.textureEntryAt(tic).element, tic);
        EXPECT_EQ(tables.textureEntryAt(tic).flags, standin::kTextureFlags);

        EXPECT_EQ(word([s gpuResourceID]), raw::samplerResourceId(tsc));
        EXPECT_EQ(standin::asks().samplerWords, before.samplerWords + 1);
        EXPECT_EQ(tables.samplerEntryAt(tsc).element, tsc);
        EXPECT_EQ(tables.samplerEntryAt(tsc).flags, standin::kSamplerWord);
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
