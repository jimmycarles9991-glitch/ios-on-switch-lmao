// SPDX-License-Identifier: MIT
// Core Image's filter-function linking (newLibraryWithImageFilterFunctionsSPI:imageFilterFunctionInfo:error:): the request
// MTLCompiler's front end links the functions' AIR from into a library holding ciKernelMain, build request type 5 ("CI SPI"),
// laid out as 21H461's MTLLibraryBuilder::newLibraryWithCIFilters writes it and MTLCompilerObject::buildRequest reads it:
//   0x00 u32 functions, 0x04 u32 the infos' offset, 0x08 u32 modules, 0x0c u32 the module table's offset, 0x10 u32 flags
//   0x14 each function's name, NUL-terminated, padded to 8 bytes as a whole
//   the infos: per function u32 early-out, u32 its module, u32 arguments, then per argument u32 type and two u32 operands
//   the module table, 8-aligned: per module u32 offset and u32 size; then each module's bytes, each 8-aligned
// A module is a function's wrapped bitcode from its metallib, sent once however many functions name it (Metal sends one per
// library: a Core Image kernel library's functions share one module); the front end finds each function by name in its own.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace switch_metal {

constexpr uint32_t kImageFilterLinkRequest = 5;

// MTLImageFilterFunctionArgumentInfoSPI (Core Image's CI::LegacyDAGDescriptor::ArgumentInfo::add(type, operand, operand))
struct ImageFilterArgument {
    uint64_t type;
    uint64_t operands[2];
};

// MTLImageFilterFunctionInfoSPI, one per function linked
struct ImageFilterFunctionInfo {
    bool earlyOut;
    uint64_t argumentCount;
    const ImageFilterArgument *arguments;
};

static_assert(sizeof(ImageFilterFunctionInfo) == 24 && sizeof(ImageFilterArgument) == 24, "Metal's SPI layout");

struct ImageFilterFunction {
    std::string name;
    const uint8_t *bitcode = nullptr;
    size_t bitcodeBytes = 0;
    bool earlyOut = false;
    std::vector<ImageFilterArgument> arguments;
};

inline size_t imageFilterAlign8(size_t n)
{
    return (n + 7) & ~size_t(7);
}

// Empty when a function has no bitcode or the request would not fit the front end's u32 offsets.
inline std::vector<uint8_t> imageFilterLinkRequest(const std::vector<ImageFilterFunction> &functions)
{
    std::vector<size_t> moduleOf(functions.size());
    std::vector<const ImageFilterFunction *> modules;
    size_t namesBytes = 0, infoBytes = 0;
    for (size_t i = 0; i < functions.size(); i++) {
        const ImageFilterFunction &f = functions[i];
        if (!f.bitcode || !f.bitcodeBytes)
            return {};
        size_t m = 0;
        while (m < modules.size() &&
               (modules[m]->bitcodeBytes != f.bitcodeBytes ||
                (modules[m]->bitcode != f.bitcode && memcmp(modules[m]->bitcode, f.bitcode, f.bitcodeBytes))))
            m++;
        if (m == modules.size())
            modules.push_back(&f);
        moduleOf[i] = m;
        namesBytes += f.name.size() + 1;
        infoBytes += 12 + 12 * f.arguments.size();
    }
    const size_t infoAt = 0x14 + imageFilterAlign8(namesBytes);
    const size_t tableAt = imageFilterAlign8(infoAt + infoBytes);
    size_t total = tableAt + 8 * modules.size();
    for (const ImageFilterFunction *m : modules)
        total += imageFilterAlign8(m->bitcodeBytes);
    if (functions.empty() || total > UINT32_MAX)
        return {};
    std::vector<uint8_t> request(total, 0);
    auto put = [&](size_t at, uint32_t v) {
        memcpy(request.data() + at, &v, 4);
    };
    put(0x00, uint32_t(functions.size()));
    put(0x04, uint32_t(infoAt));
    put(0x08, uint32_t(modules.size()));
    put(0x0c, uint32_t(tableAt));
    size_t at = 0x14;
    for (const ImageFilterFunction &f : functions) {
        memcpy(request.data() + at, f.name.c_str(), f.name.size() + 1);
        at += f.name.size() + 1;
    }
    at = infoAt;
    for (size_t i = 0; i < functions.size(); i++) {
        const ImageFilterFunction &f = functions[i];
        put(at, f.earlyOut ? 1 : 0);
        put(at + 4, uint32_t(moduleOf[i]));
        put(at + 8, uint32_t(f.arguments.size()));
        at += 12;
        for (const ImageFilterArgument &a : f.arguments) {
            put(at, uint32_t(a.type));
            put(at + 4, uint32_t(a.operands[0]));
            put(at + 8, uint32_t(a.operands[1]));
            at += 12;
        }
    }
    size_t data = tableAt + 8 * modules.size();
    for (size_t m = 0; m < modules.size(); m++) {
        put(tableAt + 8 * m, uint32_t(data));
        put(tableAt + 8 * m + 4, uint32_t(modules[m]->bitcodeBytes));
        memcpy(request.data() + data, modules[m]->bitcode, modules[m]->bitcodeBytes);
        data += imageFilterAlign8(modules[m]->bitcodeBytes);
    }
    return request;
}

}  // namespace switch_metal
