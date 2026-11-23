// SPDX-License-Identifier: MIT
// A function's newArgumentEncoderWithBufferIndex: is the shader compiler's answer (buffer_encoders.h), with no m2v
// device opened. testdata/function_arguments/args.metallib is Xcode's metal 32023.922 build of args.metal. The stand-in
// compiler answers with the runs the real compiler made of it on this Mac (the bridge's
// nmb_offline_function_buffer_encoders over the translator); the expected lengths, alignments and offsets are what
// Metal answered for the same metallib on an Apple M4 Max under macOS 27.2: constantDataAtIndex:, and where
// setTexture:, setSamplerState: and setBuffer: wrote.
#include "facade_harness.h"
#include "../buffer_encoders.h"
#include "../compile_request.h"
#include "../reflection_blob.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr uint32_t kBuffer = 0, kTexture = 1, kSampler = 2, kConstant = 3;

// The compiler's answers for args.metal's functions; "<name> made with constant values" where that differs
const std::map<std::string, gm20b::BufferEncoders> &compiled()
{
    static const std::map<std::string, gm20b::BufferEncoders> answers = {
        {"mixed_fragment",
         {{0,
           true,
           "",
           {{160,
             16,
             {{0, 1, 1, 1, kConstant, 0, 0, 4, 0, 4},
              {1, 2, 1, 1, kTexture, 0, 8, 8, 0, 8},
              {3, 1, 1, 1, kSampler, 0, 24, 8, 0, 8},
              {4, 1, 1, 1, kBuffer, 0, 32, 8, 0, 8},
              {5, 1, 1, 1, kConstant, 0, 48, 16, 0, 16},
              {6, 1, 1, 1, kConstant, 0, 64, 4, 0, 4},
              {7, 1, 1, 1, kConstant, 0, 80, 64, 0, 64},
              {8, 1, 1, 1, kBuffer, 2, 144, 8, 0, 8},
              {9, 1, 1, 1, kConstant, 0, 152, 1, 0, 1},
              {10, 1, 1, 1, kConstant, 0, 153, 1, 0, 1}}},
            {16, 8, {{0, 1, 1, 1, kTexture, 0, 0, 8, 0, 8}, {1, 1, 1, 1, kBuffer, 0, 8, 8, 0, 8}}}}},
          {1, false, "", {{0, 1, {}}}}}},
        {"plain_fragment",
         {{0, true, "", {{16, 8, {{0, 1, 1, 1, kTexture, 0, 0, 8, 0, 8}, {1, 1, 1, 1, kBuffer, 0, 8, 8, 0, 8}}}}},
          {1, false, "", {{24, 8, {{0, 1, 1, 1, kConstant, 0, 0, 8, 0, 8}, {0, 1, 1, 1, kConstant, 0, 16, 8, 0, 8}}}}},
          {2, false, "", {{0, 1, {}}}}}},
        {"gated_fragment", {}},
        {"gated_fragment made with constant values",
         {{0, true, "Compile(NeedsInput(FunctionConstant { index: 0 }))", {}}}},
    };
    return answers;
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

// The shader compiler as the kernel model runs it: each buffer-encoder request answered in the store with compiled()'s
// answer for its function, under the name its own key and libraries give
struct EncodersCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<std::string> asked;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x62;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            constexpr char kPrefix[] = "buffer encoders | ";
            gm20b::CompileRequest r;
            if (!gm20b::decodeRequest(bytes, r) || !gm20b::bufferEncoders(r) || r.key.rfind(kPrefix, 0))
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r.key);
            auto found = compiled().find(r.stages[0].function + " made with constant values");
            if (!r.stages[0].specialized || found == compiled().end())
                found = compiled().find(r.stages[0].function);
            if (found == compiled().end())
                return int32_t(TegraGM20BCompileUnavailable);
            std::vector<gm20b::LibraryIdentity> ids;
            for (const gm20b::RequestLibrary &l : r.libraries)
                ids.push_back(gm20b::identityOf(l));
            const gm20b::CacheKey k = gm20b::bufferEncodersCacheKey(r.stack, r.key.substr(sizeof kPrefix - 1));
            const std::string blob = gm20b::encodeBufferEncoders(found->second);
            const std::string entry = gm20b::encodeReflectionEntry(k, &blob, "");
            const gm20b::Digest name = gm20b::storeName(k, r, ids);
            store.put(name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(TegraGM20BCompileMade);
        };
        return true;
    }

    // last: goes first, before what the compiler hook captures, which the device may still call
    kernel_model::HooksCleared cleared;
};

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

id<MTLLibrary> argsLibrary(id<MTLDevice> device)
{
    const char *path = getenv("SWITCH_FUNCTION_ARGUMENTS_PROBE");
    EXPECT_TRUE(path != nullptr);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@(path ?: "") error:&error];
    EXPECT_TRUE(library != nil) << text(error);
    return library;
}

id<MTLDevice> servedDevice(EncodersCompiler &compiler)
{
    if (!compiler.serve())
        return nil;
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return device;
}

// Each id's offset as constantDataAtIndex: answers it, from the arguments' start
std::vector<long> offsets(id<MTLArgumentEncoder> encoder, id<MTLBuffer> arguments, uint32_t ids)
{
    std::vector<long> out;
    for (uint32_t k = 0; k < ids; k++)
        out.push_back(static_cast<uint8_t *>([encoder constantDataAtIndex:k]) -
                      static_cast<uint8_t *>(arguments.contents) - 256);
    return out;
}

uint64_t wordAt(id<MTLBuffer> b, uint64_t offset)
{
    uint64_t w;
    memcpy(&w, static_cast<const uint8_t *>(b.contents) + offset, sizeof w);
    return w;
}

uint64_t idWord(MTLResourceID id)
{
    uint64_t w;
    memcpy(&w, &id, sizeof w);
    return w;
}

// mixed_fragment's argument buffer and the one behind its pointer at id 8 answer Metal's lengths, alignments and
// offsets, and the setters write there; its plain float4 pointer takes Metal's empty encoder; the one request is the
// compiler's, and the function made with constant values is another request (a constant-gated buffer is absent only
// made without them)
void aFunctionsEncoderIsTheCompilersAndMetals()
{
    EncodersCompiler compiler;
    id<MTLDevice> device = servedDevice(compiler);
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> library = argsLibrary(device);
    ASSERT_TRUE(library != nil);
    id<MTLFunction> mixed = [library newFunctionWithName:@"mixed_fragment"];
    ASSERT_TRUE(mixed != nil);

    id<MTLArgumentEncoder> encoder = [mixed newArgumentEncoderWithBufferIndex:0];
    ASSERT_TRUE(encoder != nil);
    EXPECT_EQ(encoder.encodedLength, 160u);
    EXPECT_EQ(encoder.alignment, 16u);
    id<MTLBuffer> arguments = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    memset(arguments.contents, 0xA5, 1024);
    [encoder setArgumentBuffer:arguments offset:256];
    EXPECT_EQ(offsets(encoder, arguments, 13), (std::vector<long>{0, 8, 16, 24, 32, 48, 64, 80, 144, 152, 153, 0, 0}));

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:4
                                                                                 height:4
                                                                              mipmapped:NO];
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    id<MTLBuffer> inner = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> colour = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    [encoder setTexture:texture atIndex:2];
    [encoder setSamplerState:sampler atIndex:3];
    [encoder setBuffer:inner offset:0 atIndex:8];
    EXPECT_EQ(wordAt(arguments, 256 + 16), idWord(texture.gpuResourceID));
    EXPECT_EQ(wordAt(arguments, 256 + 24), idWord(sampler.gpuResourceID));
    EXPECT_EQ(wordAt(arguments, 256 + 144), inner.gpuAddress);

    id<MTLArgumentEncoder> nested = [encoder newArgumentEncoderForBufferAtIndex:8];
    ASSERT_TRUE(nested != nil);
    EXPECT_EQ(nested.encodedLength, 16u);
    EXPECT_EQ(nested.alignment, 8u);
    EXPECT_EQ([encoder newArgumentEncoderForBufferAtIndex:4], nil) << "a pointer to plain memory";
    [nested setArgumentBuffer:inner offset:0];
    [nested setTexture:texture atIndex:0];
    [nested setBuffer:colour offset:16 atIndex:1];
    EXPECT_EQ(wordAt(inner, 0), idWord(texture.gpuResourceID));
    EXPECT_EQ(wordAt(inner, 8), colour.gpuAddress + 16);

    id<MTLArgumentEncoder> plain = [mixed newArgumentEncoderWithBufferIndex:1];
    ASSERT_TRUE(plain != nil) << "Metal's encoder of a plain buffer, made without API validation";
    EXPECT_EQ(plain.encodedLength, 0u);
    EXPECT_EQ(plain.alignment, 1u);
    EXPECT_EQ([mixed newArgumentEncoderWithBufferIndex:7], nil) << "no buffer there: Metal asserts";
    MTLArgument *reflected = (MTLArgument *)mixed;
    EXPECT_NE([mixed newArgumentEncoderWithBufferIndex:0 reflection:&reflected], nil);
    EXPECT_EQ(reflected, nil);

    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(getenv("SWITCH_FUNCTION_ARGUMENTS_PROBE"), hashes));
    ASSERT_EQ(compiler.asked.size(), 1u) << "every ask after the first is the store's";
    EXPECT_EQ(compiler.asked[0], "buffer encoders | " + gm20b::bufferEncodersKey(hashes["mixed_fragment"], "", false));

    NSError *error = nil;
    id<MTLFunction> specialized =
        [library newFunctionWithName:@"mixed_fragment" constantValues:[MTLFunctionConstantValues new] error:&error];
    ASSERT_TRUE(specialized != nil) << text(error);
    EXPECT_EQ([specialized newArgumentEncoderWithBufferIndex:0].encodedLength, 160u);
    ASSERT_EQ(compiler.asked.size(), 2u) << "made with constant values, even none, it is asked apart";
    EXPECT_EQ(compiler.asked[1], "buffer encoders | " + gm20b::bufferEncodersKey(hashes["mixed_fragment"], "", true));
}

// A plain struct buffer answers the layout its program reads (Metal's on an M4: plain_fragment's float4 and float at 0
// and 16, id 0 the later); an index the compiler refuses (gated_fragment made with no value for the constant its buffer
// is gated on) or one with no buffer (gated_fragment made without constant values: Metal asserts) is nil with one error
// line each
void aPlainStructAnswersAndARefusedIndexIsNilWithItsLine()
{
    EncodersCompiler compiler;
    id<MTLDevice> device = servedDevice(compiler);
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> library = argsLibrary(device);
    id<MTLFunction> plain = [library newFunctionWithName:@"plain_fragment"];
    ASSERT_TRUE(plain != nil);
    id<MTLArgumentEncoder> structured = [plain newArgumentEncoderWithBufferIndex:1];
    ASSERT_TRUE(structured != nil);
    EXPECT_EQ(structured.encodedLength, 24u);
    EXPECT_EQ(structured.alignment, 8u);
    id<MTLBuffer> arguments = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    [structured setArgumentBuffer:arguments offset:0];
    EXPECT_EQ(static_cast<uint8_t *>([structured constantDataAtIndex:0]) - static_cast<uint8_t *>(arguments.contents),
              16);
    EXPECT_EQ([plain newArgumentEncoderWithBufferIndex:0].encodedLength, 16u);
    EXPECT_EQ([plain newArgumentEncoderWithBufferIndex:2].encodedLength, 0u);

    NSError *error = nil;
    id<MTLFunction> gated =
        [library newFunctionWithName:@"gated_fragment" constantValues:[MTLFunctionConstantValues new] error:&error];
    ASSERT_TRUE(gated != nil) << text(error);
    id<MTLFunction> unspecialized = [library newFunctionWithName:@"gated_fragment"];
    testing::internal::CaptureStderr();
    EXPECT_EQ([gated newArgumentEncoderWithBufferIndex:0], nil);
    EXPECT_EQ([unspecialized newArgumentEncoderWithBufferIndex:0], nil);
    const std::string lines = testing::internal::GetCapturedStderr();
    EXPECT_NE(lines.find("SwitchMetal: argument encoder creation refused: buffer 0 of gated_fragment: "
                         "Compile(NeedsInput(FunctionConstant { index: 0 }))"),
              std::string::npos)
        << lines;
    EXPECT_NE(
        lines.find(
            "SwitchMetal: argument encoder creation refused: buffer 0 of gated_fragment: the function declares no "
            "buffer there"),
        std::string::npos)
        << lines;
    EXPECT_EQ(compiler.asked.size(), 3u) << "plain_fragment, and gated_fragment made each way";
}

// With no shader compiler this boot a function's encoder is nil, with an error line, never m2v's
void withNoCompilerAFunctionsEncoderIsNil()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLFunction> mixed = [argsLibrary(device) newFunctionWithName:@"mixed_fragment"];
    ASSERT_TRUE(mixed != nil);
    EXPECT_EQ([mixed newArgumentEncoderWithBufferIndex:0], nil);
}

}  // namespace

TEST(GM20BFacadeBufferEncoders, AFunctionsEncoderIsTheCompilersAndMetals)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFunctionsEncoderIsTheCompilersAndMetals);
}

TEST(GM20BFacadeBufferEncoders, APlainStructAnswersAndARefusedIndexIsNilWithItsLine)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPlainStructAnswersAndARefusedIndexIsNilWithItsLine);
}

TEST(GM20BFacadeBufferEncoders, WithNoCompilerAFunctionsEncoderIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(withNoCompilerAFunctionsEncoderIsNil);
}
