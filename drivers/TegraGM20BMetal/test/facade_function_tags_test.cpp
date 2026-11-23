// SPDX-License-Identifier: MIT
// A function's functionConstantsDictionary, vertexAttributes and stageInputAttributes come from its metallib's public metadata,
// with no m2v device opened. testdata/function_tags/probe.metallib is Xcode's metal 32023.922 build of probe.metal; the answers
// below are what an M-series Mac's Metal gives for it (the same as the tag reader gave for all 10753 functions of the macOS
// 27.2 MPS, MPSGraph, Core Image, CoreUI, Vision, RenderBox and QuartzCore metallibs).
#include "facade_harness.h"
#include "../compile_request.h"
#include "../function_attributes.h"
#include "../reflection_blob.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#include <fstream>
#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

// name:type:index:(r)equired or (o)ptional, in name order
std::string constants(id<MTLFunction> f)
{
    std::string out;
    for (NSString *name in [f.functionConstantsDictionary.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        MTLFunctionConstant *c = f.functionConstantsDictionary[name];
        EXPECT_TRUE([c isKindOfClass:[MTLFunctionConstant class]]);
        out += std::string(c.name.UTF8String) + ":" + std::to_string(c.type) + ":" + std::to_string(c.index) +
               (c.required ? ":r " : ":o ");
    }
    return out;
}

// name:index:type:(a)ctive,(p)atch data,(c)ontrol-point data; "nil" for nil
std::string attributes(NSArray *list)
{
    if (!list)
        return "nil";
    std::string out;
    for (MTLAttribute *a in list)
        out += std::string(a.name.UTF8String) + ":" + std::to_string(a.attributeIndex) + ":" +
               std::to_string(a.attributeType) + ":" + (a.active ? "a" : "-") + (a.patchData ? "p" : "-") +
               (a.patchControlPointData ? "c" : "-") + " ";
    return out;
}

struct Measured {
    const char *function, *constants, *vertexAttributes, *stageInputAttributes;
};

constexpr Measured kProbe[] = {
    {"v_attrs", "hasNormal:53:4:o scale:3:1:r ", "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- ",
     "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- "},
    {"v_patch", "", "p:0:6:a-c extra:3:6:ap- ", "p:0:6:a-c extra:3:6:ap- "},
    {"f_tint", "useTint:53:0:o ", "nil", "nil"},
    {"k_stage", "mode:29:3:r ", "nil", "p:1:4:a-- id:4:33:--- "},
    {"k_plain", "", "nil", "nil"},
};

void measuredAnswers()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const char *probe = getenv("SWITCH_FUNCTION_TAGS_PROBE");
    ASSERT_TRUE(probe != nullptr);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@(probe) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    for (const Measured &m : kProbe) {
        id<MTLFunction> f = [library newFunctionWithName:@(m.function)];
        ASSERT_TRUE(f != nil) << m.function;
        EXPECT_EQ(constants(f), m.constants) << m.function;
        EXPECT_EQ(attributes(f.vertexAttributes), m.vertexAttributes) << m.function;
        EXPECT_EQ(attributes(f.stageInputAttributes), m.stageInputAttributes) << m.function;
        for (id attribute in f.vertexAttributes)
            EXPECT_TRUE([attribute isKindOfClass:[MTLVertexAttribute class]]) << m.function;
        for (id attribute in f.stageInputAttributes)
            EXPECT_TRUE([attribute isKindOfClass:[MTLAttribute class]]) << m.function;
    }
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const int mode = 2;
    [values setConstantValue:&mode type:MTLDataTypeInt atIndex:3];
    id<MTLFunction> specialized = [library newFunctionWithName:@"k_plain" constantValues:values error:&error];
    ASSERT_TRUE(specialized != nil) << text(error);
    EXPECT_EQ(constants(specialized), "") << "a function made with constant values answers none";
    EXPECT_EQ(attributes(specialized.stageInputAttributes), "nil");
}

std::vector<uint8_t> tagged(const char *fourcc, const std::vector<uint8_t> &value)
{
    std::vector<uint8_t> out;
    made_metallib::tag(out, fourcc, value.data(), uint16_t(value.size()));
    return out;
}

// A u16 count, then each entry's name and `fields`
std::vector<uint8_t> entries(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &list)
{
    std::vector<uint8_t> out = {uint8_t(list.size()), 0};
    for (const auto &[name, fields] : list) {
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
        out.insert(out.end(), fields.begin(), fields.end());
    }
    return out;
}

// What m2v refused as not measured is answered as it answered: no constants, nil attributes; with no m2v device
void unmeasuredTags()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const std::vector<uint8_t> flagTwo = tagged("CNST", entries({{"k", {29, 0, 0, 2}}}));
    std::vector<uint8_t> highBit = tagged("VATT", entries({{"a", {0x00, 0x10}}}));
    const std::vector<uint8_t> types = tagged("VATY", {1, 0, 6});
    highBit.insert(highBit.end(), types.begin(), types.end());
    std::vector<uint8_t> typeless = tagged("VATT", entries({{"b", {0x01, 0x80}}}));
    const std::vector<uint8_t> unknownType = tagged("VATY", {1, 0, 57});
    typeless.insert(typeless.end(), unknownType.begin(), unknownType.end());
    const std::string bytes = made_metallib::thin(
        {"flagged", "high_bit", "unknown_type"}, {2, 0, 0},
        {made_metallib::wrappedBitcode(0x31), made_metallib::wrappedBitcode(0x32), made_metallib::wrappedBitcode(0x33)},
        {flagTwo, highBit, typeless});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_EQ(constants([library newFunctionWithName:@"flagged"]), "") << "a flag byte of 2";
    EXPECT_EQ(attributes([library newFunctionWithName:@"high_bit"].vertexAttributes), "nil") << "VATT bit 12";
    EXPECT_EQ(attributes([library newFunctionWithName:@"unknown_type"].stageInputAttributes), "nil")
        << "MTLDataType 57";
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

// The shader compiler as the kernel model runs it: each attributes request answered with `vertex` (and the same as its
// stage-input attributes) in the store, under the name its own key and libraries give
struct AttributesCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<std::string> asked;
    std::vector<gm20b::FunctionAttributeTag> vertex;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x77;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            constexpr char kPrefix[] = "attributes | ";
            gm20b::CompileRequest r;
            if (!gm20b::decodeRequest(bytes, r) || !gm20b::functionAttributes(r) || r.key.rfind(kPrefix, 0))
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r.key);
            std::vector<gm20b::LibraryIdentity> ids;
            for (const gm20b::RequestLibrary &l : r.libraries)
                ids.push_back(gm20b::identityOf(l));
            const gm20b::CacheKey k = gm20b::functionAttributesCacheKey(r.stack, r.key.substr(sizeof kPrefix - 1));
            const std::string blob = gm20b::encodeAttributeLists(vertex, vertex);
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

// A function made with constant values that has attributes: which its constants gate off is in its AIR, so the shader
// compiler answers them (once: the second ask is its store's); one with none answers nil from its tags, asking nothing
void specializedAttributes()
{
    AttributesCompiler compiler;
    gm20b::FunctionAttributeTag position;
    position.name = "position";
    position.type = 6;
    position.active = true;
    compiler.vertex = {position};
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    const char *probe = getenv("SWITCH_FUNCTION_TAGS_PROBE");
    ASSERT_TRUE(probe != nullptr);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@(probe) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const bool normal = false;
    const float scale = 1;
    [values setConstantValue:&normal type:MTLDataTypeBool atIndex:4];
    [values setConstantValue:&scale type:MTLDataTypeFloat atIndex:1];
    id<MTLFunction> specialized = [library newFunctionWithName:@"v_attrs" constantValues:values error:&error];
    ASSERT_TRUE(specialized != nil) << text(error);
    EXPECT_EQ(attributes(specialized.vertexAttributes), "position:0:6:a-- ");
    EXPECT_EQ(attributes(specialized.stageInputAttributes), "position:0:6:a-- ");
    ASSERT_EQ(compiler.asked.size(), 1u) << "the second list is the store's";
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(probe, hashes));
    EXPECT_EQ(compiler.asked[0], "attributes | " + gm20b::functionAttributesKey(
                                                       hashes["v_attrs"], gm20b::constantsText(values), "v_attrs"));
    id<MTLFunction> plain = [library newFunctionWithName:@"k_plain" constantValues:values error:&error];
    EXPECT_EQ(attributes(plain.stageInputAttributes), "nil");
    EXPECT_EQ(compiler.asked.size(), 1u) << "a function of no attributes asks nothing";
}

// With no shader compiler this boot, a specialized function's attributes are nil (with an error line), never m2v's
void specializedAttributesWithNoCompiler()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@(getenv("SWITCH_FUNCTION_TAGS_PROBE")) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLFunction> specialized =
        [library newFunctionWithName:@"k_stage" constantValues:[MTLFunctionConstantValues new] error:&error];
    ASSERT_TRUE(specialized != nil) << text(error);
    EXPECT_EQ(attributes(specialized.stageInputAttributes), "nil");
}

}  // namespace

TEST(GM20BFacadeFunctionTags, AnswersAreMetalsForAMetalBuiltLibrary)
{
    switch_ios::test::ExpectPassesInFreshProcess(measuredAnswers);
}

TEST(GM20BFacadeFunctionTags, UnmeasuredTagsAnswerAsM2VDid)
{
    switch_ios::test::ExpectPassesInFreshProcess(unmeasuredTags);
}

TEST(GM20BFacadeFunctionTags, ASpecializedFunctionsAttributesAreTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(specializedAttributes);
}

TEST(GM20BFacadeFunctionTags, ASpecializedFunctionsAttributesWithNoCompilerAreNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(specializedAttributesWithNoCompiler);
}
