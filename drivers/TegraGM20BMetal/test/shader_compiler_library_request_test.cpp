// SPDX-License-Identifier: MIT
// A library the system's Metal front end makes (raw/runtime/raw_compile_request.h RequestKind::Library) as the client
// asks for it and the compiler takes it (compiler/compiler_request.h): keyed by its code generation request
// (raw_codegen_request.h), which the compiler takes only exactly as a client's builder writes it, its source options
// only what Metal passes, run with the compiler's own module cache.
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_codegen_request.h"
#include "MetallibSlice.h"
#include "made_metallib.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

raw::CompileRequest libraryRequest(uint32_t type, const std::string &codeGen)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Library;
    r.codeGenType = type;
    r.codeGen = codeGen;
    r.key = raw::libraryCacheKey(kStack, type, codeGen).key;
    return r;
}

const std::string kOptions = "--driver-mode=metal -x metal -std=metal3.1 -ffast-math";

bool prepared(const raw::CompileRequest &r, std::string &why)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    return switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why);
}

// A library's request carries the code generation request alone and is keyed by its type and bytes; it names no stage
// or library, and only the front end's request types
TEST(LibraryRequest, ALibraryIsKeyedByItsCodeGenerationRequest)
{
    const raw::CompileRequest r =
        libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest("kernel void k() {}", kOptions));
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));
    EXPECT_EQ(back.codeGen, r.codeGen);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key.rfind("library | 13 | ", 0), 0u) << p.key.key;
    EXPECT_EQ(p.name, raw::storeName(p.key, r, {}));

    const raw::CompileRequest other =
        libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest("kernel void j() {}", kOptions));
    EXPECT_NE(other.key, r.key);
    raw::CompileRequest claimed = r;
    claimed.key = other.key;
    EXPECT_FALSE(prepared(claimed, why));
    EXPECT_NE(why.find("another key"), std::string::npos) << why;
    raw::CompileRequest staged = r;
    staged.stages.resize(1);
    staged.stages[0].tag = staged.stages[0].function = staged.stages[0].name = "k";
    staged.libraries.resize(1);
    staged.libraries[0].bytes = "bytes";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(staged), back)) << "a library request names no stage or library";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(libraryRequest(7, r.codeGen)), back));
}

// Of a source request the compiler takes only the options a client's builder writes: nothing that acts in its process
TEST(LibraryRequest, ASourceRequestTakesOnlyAClientsOptions)
{
    std::string why;
    const auto takes = [&](const std::string &options, const std::string &source = "kernel void k() {}") {
        return prepared(libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest(source, options)), why);
    };
    EXPECT_TRUE(takes(kOptions)) << why;
    EXPECT_TRUE(
        takes(kOptions + " -fpreserve-invariance -Os -DTILE=16 " + raw::codegen::quotedArgument("-DNAME=\"a b\\'c\"")))
        << why;
    EXPECT_TRUE(takes("--driver-mode=metal -x metal -std=ios-metal2.4 -fno-fast-math")) << why;
    for (const char *extra :
         {" -fmodules-cache-path=/private/var/mobile", " -fplugin=/tmp/x.dylib", " -Xclang -load", " -o /tmp/out",
          "  -Os", " -Os ", " \"-Os\"x", " -DA=1\n#include", " -D1X=2", " '-Os'", " \"-DA=\\n\""})
        EXPECT_FALSE(takes(kOptions + extra)) << extra;
    EXPECT_FALSE(takes("--driver-mode=metal -x c -std=metal3.1 -ffast-math"));
    EXPECT_FALSE(takes("--driver-mode=metal -x metal -std=metal3 -ffast-math"));
    std::string padded = raw::codegen::sourceRequest("kernel void k() {}", kOptions);
    padded[16 + 19] = 'x';
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenSource, padded), why)) << "a byte past the source's NUL";
}

// The compiler runs a source request with a module cache of its own, the only place its sandbox lets the front end write
TEST(LibraryRequest, TheCompilerRunsASourceRequestWithItsOwnModuleCache)
{
    const std::string request = raw::codegen::sourceRequest("kernel void k() {}", kOptions);
    const std::string run = raw::codegen::withModuleCache(request, "/private/var/Switch/shaders/modules");
    std::string_view source, options;
    ASSERT_TRUE(raw::codegen::readSource(run, source, options));
    EXPECT_EQ(source, "kernel void k() {}");
    EXPECT_EQ(options, kOptions + " -fmodules-cache-path=/private/var/Switch/shaders/modules");
    const std::string spaced = raw::codegen::withModuleCache(request, "/a b");
    ASSERT_TRUE(raw::codegen::readSource(spaced, source, options));
    std::vector<std::string> arguments;
    ASSERT_TRUE(raw::codegen::splitArguments(options, arguments));
    EXPECT_EQ(arguments.back(), "-fmodules-cache-path=/a b");
}

// A stitching request is the DAG, each function's bitcode as its metallib places it and this kind's tail, taken only
// exactly as the builder writes them
TEST(LibraryRequest, AStitchingRequestIsTakenOnlyAsTheBuilderWritesIt)
{
    const std::string add = made_metallib::wrappedBitcode(0x41), mul = made_metallib::wrappedBitcode(0x42, 96);
    const std::string library = made_metallib::thin({"add", "mul"}, {4, 4}, {add, mul});
    const auto *bytes = reinterpret_cast<const uint8_t *>(library.data());
    raw::codegen::Bitcode a, m;
    ASSERT_TRUE(raw::codegen::functionBitcode(bytes, library.size(), "add", a));
    ASSERT_TRUE(raw::codegen::functionBitcode(bytes, library.size(), "mul", m));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(a.bytes), a.size), add);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(m.bytes), m.size), mul);
    raw::codegen::Bitcode none;
    EXPECT_FALSE(raw::codegen::functionBitcode(bytes, library.size(), "sub", none));
    const std::string placeless = made_metallib::universal({"add"}, 0xA0, 0xEE, {4});
    size_t at = 0, size = 0;
    ASSERT_TRUE(
        switch_metal::airSlice(reinterpret_cast<const uint8_t *>(placeless.data()), placeless.size(), at, size));
    EXPECT_FALSE(
        raw::codegen::functionBitcode(reinterpret_cast<const uint8_t *>(placeless.data()) + at, size, "add", none));

    const std::string dag = "DAGS {\nstitched {\nOUTPUT(node_output) : FUNCTION(mul), INPUT(0), INPUT(1);\n}\n}\n";
    const std::string request = raw::codegen::stitchRequest(dag, {a, m});
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenStitch, request));
    uint32_t tag = 0;
    memcpy(&tag, request.data(), 4);
    EXPECT_EQ(tag, raw::codegen::fourcc("dag "));
    EXPECT_EQ(request.size(),
              4 + dag.size() + 1 + 8 + 8 + add.size() + 8 + mul.size() + (raw::codegen::kThisKind.stitchTail ? 24 : 0));
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenStitch, request), why)) << why;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, request + std::string(4, '\0')));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, request.substr(0, request.size() - 4)));
    std::string notBitcode = request;
    notBitcode[4 + dag.size() + 1 + 8 + 8] ^= 0x7f;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, notBitcode));
    std::string more = request;
    more[4 + dag.size() + 1 + 4] = 3;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, more));
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenStitch, more), why));
}

// A filter link request is taken only as ImageFilterLink.h writes it: its functions, their infos and each module once
TEST(LibraryRequest, AFilterLinkRequestIsTakenOnlyAsTheBuilderWritesIt)
{
    const std::string bitcode = made_metallib::wrappedBitcode(0x51, 40);
    std::vector<switch_metal::ImageFilterFunction> functions(2);
    for (switch_metal::ImageFilterFunction &f : functions) {
        f.bitcode = reinterpret_cast<const uint8_t *>(bitcode.data());
        f.bitcodeBytes = bitcode.size();
    }
    functions[0].name = "tint";
    functions[0].earlyOut = true;
    functions[0].arguments = {{3, {1, 2}}};
    functions[1].name = "swap_rb";
    const std::vector<uint8_t> made = switch_metal::imageFilterLinkRequest(functions);
    const std::string request(made.begin(), made.end());
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenImageFilters, request));
    std::vector<switch_metal::ImageFilterFunction> back;
    ASSERT_TRUE(raw::codegen::readImageFilters(request, back));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[1].name, "swap_rb");
    EXPECT_TRUE(back[0].earlyOut);
    ASSERT_EQ(back[0].arguments.size(), 1u);
    EXPECT_EQ(back[0].arguments[0].operands[1], 2u);
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenImageFilters, request), why)) << why;
    std::string flagged = request;
    flagged[0x10] = 1;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, flagged));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, request + std::string(8, '\0')));
    std::string counted = request;
    counted[0] = 3;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, counted));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenSource, request));
}

}  // namespace
