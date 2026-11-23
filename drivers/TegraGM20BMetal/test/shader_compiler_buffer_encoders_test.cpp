// SPDX-License-Identifier: MIT
// A function's buffer encoders as a compile request (buffer_encoders.h): one stage tagged "b", keyed by
// the function's AIR, constants and whether it was made with them, and refused with any other stage or a key naming
// other constants; and the blob the compiler stores, read back whole or not at all.
#include "../TegraGM20BMetalCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../buffer_encoders.h"
#include "made_metallib.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

gm20b::RequestStage stage(const char *tag, const char *function, const char *constants = "")
{
    gm20b::RequestStage s;
    s.tag = tag;
    s.function = s.name = function;
    s.constants = constants;
    s.specialized = constants[0] != 0;
    return s;
}

gm20b::CompileRequest request(const char *constants)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Reflection;
    gm20b::RequestLibrary l;
    l.bytes = made_metallib::universal({"fragment_fn"}, 0x30);
    r.libraries.push_back(l);
    r.stages = {stage("b", "fragment_fn", constants)};
    gm20b::AirHash air;
    air.fill(0x30);
    r.key = gm20b::bufferEncodersCacheKey(kStack, gm20b::bufferEncodersKey(air, constants, constants[0] != 0)).key;
    return r;
}

TEST(BufferEncodersRequest, IsKeyedByItsFunctionsAirAndConstants)
{
    const gm20b::CompileRequest r = request("#0:53:01");
    EXPECT_TRUE(gm20b::bufferEncoders(r));
    EXPECT_FALSE(gm20b::functionAttributes(r));
    EXPECT_FALSE(gm20b::computeReflection(r));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    gm20b::AirHash air;
    air.fill(0x30);
    EXPECT_EQ(p.key.key, "buffer encoders | " + gm20b::bufferEncodersKey(air, "#0:53:01", true));
    EXPECT_NE(p.key.key, request("").key) << "other constants, another key";
    gm20b::CompileRequest none = request("");
    none.stages[0].specialized = true;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(none), kStack, system, p, why))
        << "made with no constant values names another key than made without";
    none.key = gm20b::bufferEncodersCacheKey(kStack, gm20b::bufferEncodersKey(air, "", true)).key;
    EXPECT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(none), kStack, system, p, why)) << why;

    gm20b::CompileRequest other = r;
    other.stages[0].constants = "#0:53:00";
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(other), kStack, system, p, why));
    gm20b::CompileRequest two = r;
    two.stages.push_back(stage("b", "fragment_fn"));
    gm20b::CompileRequest back;
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(two), back)) << "one function";
    gm20b::CompileRequest kernel = r;
    kernel.stages[0].tag = "k";
    EXPECT_FALSE(gm20b::bufferEncoders(kernel));
}

gm20b::BufferEncoders answered()
{
    gm20b::BufferEncoders out(3);
    out[0].index = 0;
    out[0].argumentBuffer = true;
    out[0].layouts = {{160, 16, {{0, 1, 1, 1, 3, 0, 0, 4, 0, 4}, {8, 1, 1, 1, 0, 2, 144, 8, 0, 8}}},
                      {16, 8, {{0, 1, 1, 1, 1, 0, 0, 8, 0, 8}, {1, 1, 1, 1, 0, 0, 8, 8, 0, 8}}}};
    out[1].index = 1;
    out[1].layouts = {{0, 1, {}}};
    out[2].index = 4;
    out[2].argumentBuffer = true;
    out[2].refusal = "ArrayOfArrays { id: 0 }";
    return out;
}

TEST(BufferEncodersRequest, TheBlobReadsBackWhole)
{
    const std::string blob = gm20b::encodeBufferEncoders(answered());
    gm20b::BufferEncoders back;
    ASSERT_TRUE(gm20b::decodeBufferEncoders(blob, back));
    ASSERT_EQ(back.size(), 3u);
    EXPECT_TRUE(back[0].argumentBuffer);
    ASSERT_EQ(back[0].layouts.size(), 2u);
    EXPECT_EQ(back[0].layouts[0].encodedLength, 160u);
    EXPECT_EQ(back[0].layouts[0].runs[1].offset, 144u);
    EXPECT_EQ(back[0].layouts[0].runs[1].nested, 2u);
    EXPECT_EQ(back[0].layouts[1].alignment, 8u);
    EXPECT_FALSE(back[1].argumentBuffer);
    EXPECT_EQ(back[1].layouts[0].encodedLength, 0u);
    EXPECT_TRUE(back[1].layouts[0].runs.empty());
    EXPECT_EQ(back[2].refusal, "ArrayOfArrays { id: 0 }");
    EXPECT_TRUE(back[2].layouts.empty());
    EXPECT_EQ(gm20b::bufferEncoderAt(back, 4), &back[2]);
    EXPECT_EQ(gm20b::bufferEncoderAt(back, 2), nullptr) << "no buffer at 2";

    EXPECT_FALSE(gm20b::decodeBufferEncoders(blob.substr(0, blob.size() - 1), back)) << "cut short";
    EXPECT_FALSE(gm20b::decodeBufferEncoders(blob + "x", back)) << "trailing bytes";
    gm20b::BufferEncoders unordered = answered();
    std::swap(unordered[0], unordered[1]);
    EXPECT_FALSE(gm20b::decodeBufferEncoders(gm20b::encodeBufferEncoders(unordered), back)) << "indices out of order";
    gm20b::BufferEncoders none = answered();
    none[1].layouts.clear();
    EXPECT_FALSE(gm20b::decodeBufferEncoders(gm20b::encodeBufferEncoders(none), back)) << "an encoder of no layout";
    gm20b::BufferEncoders past = answered();
    past[2].index = gm20b::kBufferEncoderLimit;
    EXPECT_FALSE(gm20b::decodeBufferEncoders(gm20b::encodeBufferEncoders(past), back)) << "no buffer index 31";
}

}  // namespace
