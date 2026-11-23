// SPDX-License-Identifier: MIT
// The code generation requests MTLCompiler.framework's front end takes for a library made at run time
// (MTLCodeGenServiceBuildRequest; compile_request.h's kCodeGen types), laid out as this IPSW kind's front end reads
// them: Metal source, a stitched library's DAG, Core Image's filter-function link (ImageFilterLink.h) and a function's
// specialization. A GPU client builds them from its own functions' metallibs; the system shader compiler takes only a
// request framed as these builders frame it, as a request the front end cannot frame aborts it, and source options or
// header flags that would run anything else in its process are refused.
#pragma once
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ImageFilterLink.h"
#include "MetalABI.h"
#include "MetallibEntries.h"
#include "compile_request.h"

// The bytes Metal stores a function constant of MTLDataType `type` in (0: a type no constant takes)
extern "C" size_t _MTLConstantDataSize(uint64_t type);

namespace gm20b::codegen {

static_assert(kCodeGenImageFilters == switch_metal::kImageFilterLinkRequest, "the filter link's request type");

// A stitching request's tail after its functions: 'nums', 'ptfm' and 'arid' words, each 0. 24B119's front end (MTLCompiler
// 32024) reads it and aborts without it. 21H461's reads none of it; the replies its devices made, which the seed ships
// named by the request's bytes, were made with it.
struct Kind {
    std::string_view kind;
    bool stitchTail;
};

inline constexpr Kind kKinds[] = {
    {"21H461", true},
    {"24B119", true},
};

inline constexpr size_t kindIndex()
{
    for (size_t i = 0; i < std::size(kKinds); i++)
        if (kKinds[i].kind == SWITCH_METAL_KIND)
            return i;
    return std::size(kKinds);
}

static_assert(kindIndex() < std::size(kKinds), "no code generation layout for this kind (codegen_request.h)");
inline constexpr const Kind &kThisKind = kKinds[kindIndex()];

// A function's wrapped bitcode in its metallib's AIR slice
struct Bitcode {
    const uint8_t *bytes = nullptr;
    size_t size = 0;
};

constexpr uint32_t fourcc(std::string_view tag)
{
    return uint32_t(uint8_t(tag[0])) << 24 | uint32_t(uint8_t(tag[1])) << 16 | uint32_t(uint8_t(tag[2])) << 8 |
           uint32_t(uint8_t(tag[3]));
}

inline void word(std::string &out, uint32_t v)
{
    out.append(reinterpret_cast<const char *>(&v), sizeof v);
}

inline bool wordAt(const std::string &in, size_t at, uint32_t &v)
{
    if (at > in.size() || in.size() - at < sizeof v)
        return false;
    memcpy(&v, in.data() + at, sizeof v);
    return true;
}

// The entry named `name` in a metallib's AIR slice, its bitcode where the slice places it (header: the bitcode section's
// offset at 72 and size at 80), as Metal's MTLLibraryBuilder sends a function
inline bool functionBitcode(const uint8_t *slice, size_t size, const std::string &name, Bitcode &out)
{
    if (!slice || name.empty() || size < switch_metal::kMetallibHeaderBytes)
        return false;
    uint64_t at = 0, bytes = 0;
    memcpy(&at, slice + 72, 8);
    memcpy(&bytes, slice + 80, 8);
    if (at > size || bytes > size - at)
        return false;
    bool found = false;
    switch_metal::forEachMetallibEntry(slice, size, [&](const switch_metal::MetallibEntry &e) {
        if (e.name != name)
            return true;
        if (e.placed && e.bitcodeBytes && e.bitcodeOffset <= bytes && e.bitcodeBytes <= bytes - e.bitcodeOffset) {
            out = {slice + at + e.bitcodeOffset, size_t(e.bitcodeBytes)};
            found = true;
        }
        return false;
    });
    return found;
}

// An argument as the front end splits its options: one holding a space, tab, quote or backslash in double quotes, with
// its backslashes and double quotes escaped
inline std::string quotedArgument(std::string_view argument)
{
    if (argument.find_first_of(" \t\"\\'") == std::string_view::npos)
        return std::string(argument);
    std::string out = "\"";
    for (const char c : argument) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

inline bool control(char c)
{
    return uint8_t(c) < 0x20 || c == 0x7f;
}

// `options` split as quotedArgument() writes them, one space apart; false for any other spelling, which another splitter
// could read otherwise
inline bool splitArguments(std::string_view options, std::vector<std::string> &out)
{
    out.clear();
    for (size_t at = 0;;) {
        std::string argument;
        if (at < options.size() && options[at] == '"') {
            for (++at;; ++at) {
                if (at >= options.size())
                    return false;
                char c = options[at];
                if (c == '"') {
                    ++at;
                    break;
                }
                if (c == '\\') {
                    if (++at >= options.size() || (options[at] != '\\' && options[at] != '"'))
                        return false;
                    c = options[at];
                }
                if (control(c))
                    return false;
                argument += c;
            }
        } else {
            for (; at < options.size() && options[at] != ' '; ++at) {
                const char c = options[at];
                if (control(c) || c == '"' || c == '\\' || c == '\'')
                    return false;
                argument += c;
            }
        }
        if (argument.empty())
            return false;
        out.push_back(std::move(argument));
        if (at == options.size())
            return true;
        if (options[at++] != ' ')
            return false;
    }
}

inline bool languageStandard(std::string_view a)
{
    for (const std::string_view prefix : {"-std=metal", "-std=ios-metal"}) {
        if (a.substr(0, prefix.size()) != prefix)
            continue;
        const std::string_view version = a.substr(prefix.size());
        const size_t dot = version.find('.');
        return dot != std::string_view::npos && dot > 0 && dot + 1 < version.size() &&
               version.find_first_not_of("0123456789.") == std::string_view::npos &&
               version.find('.', dot + 1) == std::string_view::npos;
    }
    return false;
}

inline bool macro(std::string_view a)
{
    if (a.substr(0, 2) != "-D")
        return false;
    const std::string_view name =
        a.substr(2, a.find('=') == std::string_view::npos ? std::string_view::npos : a.find('=') - 2);
    return !name.empty() && !(name[0] >= '0' && name[0] <= '9') &&
           name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
               std::string_view::npos;
}

// Whether `options` are only what a client gives the front end (TegraGM20BMetalCompiledLibraries.mm compileArguments): the driver
// mode and language, a language standard and fast math, then invariance, size optimisation and macros. Any other argument
// (a plugin to load, an output, another module cache) would act in the compiler's own process.
inline bool clientOptions(std::string_view options)
{
    std::vector<std::string> a;
    if (!splitArguments(options, a) || a.size() < 5 || a[0] != "--driver-mode=metal" || a[1] != "-x" ||
        a[2] != "metal" || !languageStandard(a[3]) || (a[4] != "-ffast-math" && a[4] != "-fno-fast-math"))
        return false;
    for (size_t i = 5; i < a.size(); i++)
        if (a[i] != "-fpreserve-invariance" && a[i] != "-Os" && !macro(a[i]))
            return false;
    return true;
}

// Metal source (MTLCompilerObject::buildRequest): u32 source bytes, u32 bytes of an extra block, u32 option bytes and u32
// flags; the source NUL-padded to a multiple of 4 bytes, no extra block, then the options NUL-terminated
inline std::string sourceRequest(std::string_view source, std::string_view options)
{
    const uint32_t sourceBytes = uint32_t((source.size() + 1 + 3) & ~size_t(3));
    std::string r;
    word(r, sourceBytes);
    word(r, 0);
    word(r, uint32_t(options.size() + 1));
    word(r, 0);
    r.append(source);
    r.append(sourceBytes - source.size(), '\0');
    r.append(options);
    r.push_back('\0');
    return r;
}

inline bool readSource(const std::string &r, std::string_view &source, std::string_view &options)
{
    uint32_t sourceBytes = 0, extra = 0, optionBytes = 0, flags = 0;
    if (!wordAt(r, 0, sourceBytes) || !wordAt(r, 4, extra) || !wordAt(r, 8, optionBytes) || !wordAt(r, 12, flags) ||
        extra || flags || !optionBytes || 16 + uint64_t(sourceBytes) + optionBytes != r.size())
        return false;
    const std::string_view text(r.data() + 16, sourceBytes);
    source = text.substr(0, text.find('\0'));
    options = std::string_view(r.data() + 16 + sourceBytes, optionBytes - 1);
    return source.size() < text.size();
}

// The front end's own module cache at `modules` among a client's source request's options, as the compiler runs it
inline std::string withModuleCache(const std::string &request, const std::string &modules)
{
    std::string_view source, options;
    if (!readSource(request, source, options))
        return std::string();
    return sourceRequest(source, std::string(options) + " " + quotedArgument("-fmodules-cache-path=" + modules));
}

// A stitched library (MTLCompilerObject::stitchFunctionDag): 'dag ' and the graphs' DAG text NUL-terminated, 'numf' and
// the count of functions, then per function 'bitc', its size and its wrapped bitcode, then this kind's tail
inline std::string stitchRequest(std::string_view dag, const std::vector<Bitcode> &functions)
{
    std::string r;
    word(r, fourcc("dag "));
    r.append(dag);
    r.push_back('\0');
    word(r, fourcc("numf"));
    word(r, uint32_t(functions.size()));
    for (const Bitcode &f : functions) {
        word(r, fourcc("bitc"));
        word(r, uint32_t(f.size));
        r.append(reinterpret_cast<const char *>(f.bytes), f.size);
    }
    if (kThisKind.stitchTail)
        for (const std::string_view tag : {"nums", "ptfm", "arid"}) {
            word(r, fourcc(tag));
            word(r, 0);
        }
    return r;
}

// LLVM's bitcode wrapper (0x0B17C0DE), which every metallib entry's bitcode starts with
inline constexpr char kWrappedBitcode[4] = {char(0xde), char(0xc0), 0x17, 0x0b};

inline bool stitchRequestOk(const std::string &r)
{
    uint32_t tag = 0, count = 0;
    if (!wordAt(r, 0, tag) || tag != fourcc("dag "))
        return false;
    const size_t end = r.find('\0', 4);
    if (end == std::string::npos || end == 4)
        return false;
    size_t at = end + 1;
    if (!wordAt(r, at, tag) || tag != fourcc("numf") || !wordAt(r, at + 4, count) || !count)
        return false;
    at += 8;
    std::vector<Bitcode> functions;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = 0;
        if (!wordAt(r, at, tag) || tag != fourcc("bitc") || !wordAt(r, at + 4, size) || size < sizeof kWrappedBitcode ||
            size > r.size() - (at + 8) || memcmp(r.data() + at + 8, kWrappedBitcode, sizeof kWrappedBitcode))
            return false;
        functions.push_back({reinterpret_cast<const uint8_t *>(r.data() + at + 8), size});
        at += 8 + size;
    }
    return stitchRequest(std::string_view(r.data() + 4, end - 4), functions) == r;
}

// Constant values as MTLFunctionConstantValues serializes them for a function (MTLConstantStorage): a u32 count, then
// each value's u16 index, u8 MTLDataType and the bytes Metal stores that type in
inline bool constantDataOk(std::string_view constants)
{
    uint32_t count = 0;
    if (constants.size() < sizeof count)
        return false;
    memcpy(&count, constants.data(), sizeof count);
    size_t at = sizeof count;
    for (uint32_t i = 0; i < count; i++) {
        if (constants.size() - at < 3)
            return false;
        const size_t bytes = _MTLConstantDataSize(uint8_t(constants[at + 2]));
        if (!bytes || bytes > constants.size() - at - 3)
            return false;
        at += 3 + bytes;
    }
    return at == constants.size();
}

inline constexpr size_t aligned8(size_t bytes)
{
    return (bytes + 7) & ~size_t(7);
}

// MTLSpecializeFunctionToArchive, laid out as MetalABI.h's kSpecializeRequest: the header, then the serialized constant
// values, the source function's name with its NUL, its wrapped bitcode and the specialized name NUL-padded to 8 bytes,
// each block 8-byte aligned. No specialized name keeps the function's own. `hash` is the source function's bitcode hash
// (its metallib HASH), which Metal writes there from -[_MTLFunction bitCodeHash]. The front end answers a metallib of the
// specialized function. Every other header field stays zero: no forced AIR version or target triple, the front end's own
// iOS target check, and the archive path rather than its loader.
inline std::string specializationRequest(std::string_view name, std::string_view specializedName, const Bitcode &air,
                                         const Digest &hash, std::string_view constants)
{
    const SpecializeRequestLayout &l = kSpecializeRequest;
    if (name.empty() || name.size() >= UINT16_MAX || constants.size() > kRequestLimit || air.size > kRequestLimit ||
        specializedName.size() > kRequestLimit ||
        l.headerBytes + aligned8(constants.size()) + aligned8(name.size() + 1) + aligned8(air.size) +
                aligned8(specializedName.size() + 1) >
            kRequestLimit)
        return {};
    if (name.find('\0') != std::string_view::npos || specializedName.find('\0') != std::string_view::npos ||
        !constantDataOk(constants) || !air.bytes || air.size < sizeof kWrappedBitcode ||
        memcmp(air.bytes, kWrappedBitcode, sizeof kWrappedBitcode))
        return {};
    std::string r(l.headerBytes, '\0');
    const auto put = [&](size_t field, uint32_t v) {
        memcpy(r.data() + field, &v, sizeof v);
    };
    const auto append = [&](const void *bytes, size_t size, size_t block) {
        const uint32_t at = uint32_t(r.size());
        r.append(static_cast<const char *>(bytes), size);
        r.resize(at + block, '\0');
        return at;
    };
    put(l.constants, append(constants.data(), constants.size(), aligned8(constants.size())));
    put(l.constants + 4, uint32_t(constants.size()));
    put(l.name, append(name.data(), name.size(), aligned8(name.size() + 1)));
    const uint16_t nameBytes = uint16_t(name.size() + 1);
    memcpy(r.data() + l.name + 4, &nameBytes, sizeof nameBytes);
    put(l.air, append(air.bytes, air.size, aligned8(air.size)));
    put(l.air + 4, uint32_t(air.size));
    if (!specializedName.empty()) {
        const size_t block = aligned8(specializedName.size() + 1);
        put(l.specializedName, append(specializedName.data(), specializedName.size(), block));
        put(l.specializedName + 4, uint32_t(block));
    }
    put(l.requestType, kCodeGenSpecialize);
    put(l.requestType + 4, kCodeGenSpecialize);
    memcpy(r.data() + l.hash, hash.data(), hash.size());
    return r;
}

// A specialization request's fields where kSpecializeRequest puts them, each inside the request: the constants Metal's
// serialization right after the header, the name NUL-terminated at its stated length, the bitcode wrapped, a specialized
// name (if any) NUL-terminated inside its block, the request type twice, and every other header byte zero
inline bool specializationRequestOk(const std::string &r)
{
    const SpecializeRequestLayout &l = kSpecializeRequest;
    if (r.size() < l.headerBytes || r.size() > kRequestLimit)
        return false;
    const auto block = [&](size_t field, uint32_t &at, uint32_t &size) {
        return wordAt(r, field, at) && wordAt(r, field + 4, size) && at >= l.headerBytes && at <= r.size() &&
               size <= r.size() - at;
    };
    uint32_t constantsAt = 0, constantsBytes = 0, airAt = 0, airBytes = 0, nameAt = 0, specializedAt = 0,
             specializedBytes = 0, type = 0, downgraded = 0;
    uint16_t nameBytes = 0;
    memcpy(&nameBytes, r.data() + l.name + 4, sizeof nameBytes);
    if (!block(l.constants, constantsAt, constantsBytes) || constantsAt != l.headerBytes ||
        !block(l.air, airAt, airBytes) || airBytes < sizeof kWrappedBitcode ||
        memcmp(r.data() + airAt, kWrappedBitcode, sizeof kWrappedBitcode) || !wordAt(r, l.name, nameAt) ||
        nameAt < l.headerBytes || nameAt > r.size() || nameBytes < 2 || nameBytes > r.size() - nameAt ||
        r[nameAt + nameBytes - 1] != '\0' || memchr(r.data() + nameAt, 0, nameBytes - 1u) ||
        !wordAt(r, l.requestType, type) || !wordAt(r, l.requestType + 4, downgraded) || type != kCodeGenSpecialize ||
        downgraded != kCodeGenSpecialize || !constantDataOk(std::string_view(r.data() + constantsAt, constantsBytes)))
        return false;
    if (!wordAt(r, l.specializedName, specializedAt) || !wordAt(r, l.specializedName + 4, specializedBytes))
        return false;
    if ((specializedAt || specializedBytes) &&
        (!block(l.specializedName, specializedAt, specializedBytes) || specializedBytes < 2 || !r[specializedAt] ||
         !memchr(r.data() + specializedAt, 0, specializedBytes)))
        return false;
    const std::pair<size_t, size_t> written[] = {{l.constants, 8},       {l.name, 6},        {l.air, 8},
                                                 {l.specializedName, 8}, {l.requestType, 8}, {l.hash, sizeof(Digest)}};
    std::string header = r.substr(0, l.headerBytes);
    for (const auto &[field, bytes] : written)
        memset(header.data() + field, 0, bytes);
    return header.find_first_not_of('\0') == std::string::npos;
}

// ImageFilterLink.h's layout read back: its functions, each module's bytes in place
inline bool readImageFilters(const std::string &r, std::vector<switch_metal::ImageFilterFunction> &functions)
{
    uint32_t count = 0, infoAt = 0, modules = 0, tableAt = 0, flags = 0;
    if (!wordAt(r, 0, count) || !wordAt(r, 4, infoAt) || !wordAt(r, 8, modules) || !wordAt(r, 12, tableAt) ||
        !wordAt(r, 16, flags) || flags || !count || !modules || modules > count || count > r.size() / 13 ||
        infoAt > r.size())
        return false;
    std::vector<Bitcode> moduleBytes(modules);
    for (uint32_t m = 0; m < modules; m++) {
        uint32_t at = 0, size = 0;
        if (!wordAt(r, tableAt + 8 * size_t(m), at) || !wordAt(r, tableAt + 8 * size_t(m) + 4, size) || !size ||
            at > r.size() || size > r.size() - at)
            return false;
        moduleBytes[m] = {reinterpret_cast<const uint8_t *>(r.data() + at), size};
    }
    functions.assign(count, {});
    size_t name = 0x14, info = infoAt;
    for (switch_metal::ImageFilterFunction &f : functions) {
        const size_t nul = r.find('\0', name);
        uint32_t earlyOut = 0, module = 0, arguments = 0;
        if (nul == std::string::npos || nul >= infoAt || !wordAt(r, info, earlyOut) || !wordAt(r, info + 4, module) ||
            !wordAt(r, info + 8, arguments) || module >= modules || arguments > (r.size() - info) / 12)
            return false;
        f.name = r.substr(name, nul - name);
        name = nul + 1;
        f.earlyOut = earlyOut != 0;
        f.bitcode = moduleBytes[module].bytes;
        f.bitcodeBytes = moduleBytes[module].size;
        info += 12;
        for (uint32_t a = 0; a < arguments; a++, info += 12) {
            uint32_t type = 0, first = 0, second = 0;
            if (!wordAt(r, info, type) || !wordAt(r, info + 4, first) || !wordAt(r, info + 8, second))
                return false;
            f.arguments.push_back({type, {first, second}});
        }
    }
    return true;
}

inline bool imageFilterRequestOk(const std::string &r)
{
    std::vector<switch_metal::ImageFilterFunction> functions;
    if (!readImageFilters(r, functions))
        return false;
    const std::vector<uint8_t> made = switch_metal::imageFilterLinkRequest(functions);
    return made.size() == r.size() && !memcmp(made.data(), r.data(), r.size());
}

// Whether `request` of `type` is what a client's builder makes: framed as this kind's front end reads it, and asking
// nothing of the compiler's process but a library
inline bool requestOk(uint32_t type, const std::string &request)
{
    std::string_view source, options;
    switch (type) {
    case kCodeGenSource:
        return readSource(request, source, options) && sourceRequest(source, options) == request &&
               clientOptions(options);
    case kCodeGenStitch:
        return stitchRequestOk(request);
    case kCodeGenImageFilters:
        return imageFilterRequestOk(request);
    case kCodeGenSpecialize:
        return specializationRequestOk(request);
    default:
        return false;
    }
}

}  // namespace gm20b::codegen
