// SPDX-License-Identifier: MIT
// See compiler_request.h.
#import "compiler_request.h"

#include <limits.h>
#include <stdlib.h>
#include "compiler_key_difference.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../buffer_encoders.h"
#include "../codegen_request.h"
#include "../compute_key.h"
#include "../function_attributes.h"
#include "../render_key.h"

namespace switch_compiler {
namespace {

constexpr size_t kSystemLibraries = 256;
constexpr char kKernelPrefix[] = "kernel | ";

bool airOf(const gm20b::CompileRequest &r, const std::vector<std::map<std::string, gm20b::AirHash>> &air,
           const gm20b::RequestStage &s, gm20b::AirHash &hash, std::string &why)
{
    const auto &hashes = air[s.library];
    const auto found = hashes.find(s.function);
    if (found == hashes.end()) {
        const std::string &path = r.libraries[s.library].path;
        return why = "no AIR for " + s.function + " in " + (path.empty() ? "its library's data" : path), false;
    }
    hash = found->second;
    return true;
}

bool renderKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
               std::string &why)
{
    const gm20b::CompileRequest &r = p.request;
    gm20b::AirHash vertex, fragment{};
    const size_t f = gm20b::fragmentStage(r);
    if (!airOf(r, air, r.stages[0], vertex, why) ||
        (f != gm20b::kNoStage && !airOf(r, air, r.stages[f], fragment, why)))
        return false;
    std::vector<gm20b::LinkedStageAir> linked;
    for (size_t i = gm20b::firstLinked(r); i < r.stages.size(); ++i) {
        gm20b::LinkedStageAir stage{r.stages[i].tag[0], {}};
        if (!airOf(r, air, r.stages[i], stage.air, why))
            return false;
        linked.push_back(stage);
    }
    bool own = false;
    @autoreleasepool {
        p.packKey = gm20b::requestPackKey(r, own);
    }
    p.key = gm20b::renderCacheKey(stack, p.packKey, vertex, fragment, linked, own);
    return true;
}

// A compute request's state key (compute_key.h computeStateKey): its kernel's AIR and constants, its stage input, then
// its linked stages' (computeLinkedText), and its limits
bool stateKeyOf(const gm20b::CompileRequest &r, const std::vector<std::map<std::string, gm20b::AirHash>> &air,
                std::string &key, std::string &why)
{
    gm20b::AirHash hash;
    if (!airOf(r, air, r.stages[0], hash, why))
        return false;
    std::vector<gm20b::LinkedComputeFunction> linked;
    for (size_t i = 1; i < r.stages.size(); ++i) {
        gm20b::LinkedComputeFunction f;
        f.hidden = r.stages[i].tag == "kp";
        f.binary = r.stages[i].tag == "kb";
        if (r.stages[i].tag == "kg")
            f.group = r.stages[i].group;
        f.symbol = r.stages[i].function;
        f.name = r.stages[i].name;
        f.constants = r.stages[i].constants;
        if (!airOf(r, air, r.stages[i], f.air, why))
            return false;
        linked.push_back(std::move(f));
    }
    const std::string constants =
        r.stages[0].constants + gm20b::stageInText(r.kernel.stageIn) + gm20b::computeLinkedText(linked);
    key = gm20b::computeStateKey(hash, constants, r.kernel.maxThreads, r.kernel.multipleOfWidth);
    return true;
}

// The compute key's own part (its function's AIR, constants, stage input, linked functions, limits and shape) is the
// compiler's; the bound textures', samplers' and memory's text past it is the client's, and the store name binds the
// dispatch they describe
bool kernelKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
               std::string &why)
{
    const gm20b::CompileRequest &r = p.request;
    std::string state;
    if (!stateKeyOf(r, air, state, why))
        return false;
    const gm20b::KernelRequest &k = r.kernel;
    char shape[64];
    snprintf(shape, sizeof shape, " | tg=%u,%u,%u | tgmem=", k.threads[0], k.threads[1], k.threads[2]);
    const std::string own = kKernelPrefix + state + shape;
    if (r.key.compare(0, own.size(), own))
        return why = "a kernel request whose key is not its function's: " + keyDifference(r.key, own), false;
    const std::string region = " | region=1";
    const bool regionKey =
        r.key.size() > region.size() && !r.key.compare(r.key.size() - region.size(), region.size(), region);
    if (k.regionMode != regionKey)
        return why = std::string("a kernel request whose key names another dispatch mode than its dispatch's (") +
                     (k.regionMode ? "regions" : "one grid") + "): its key " + key_difference::excerpt(r.key, 0),
               false;
    p.packKey = r.key.substr(sizeof kKernelPrefix - 1);
    p.key = gm20b::kernelCacheKey(stack, p.packKey);
    return true;
}

// A compute state's reflection is keyed by its function's AIR, constants and limits alone (compute_key.h
// computeStateKey), as the pack keys it
bool computeReflectionKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
                          std::string &why)
{
    if (!stateKeyOf(p.request, air, p.packKey, why))
        return false;
    p.key = gm20b::computeReflectionKey(stack, p.packKey);
    return true;
}

// A compute state's facts: its function's AIR, constants and limits, as the pack keys its PackComputeFacts
bool factsKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
              std::string &why)
{
    if (!stateKeyOf(p.request, air, p.packKey, why))
        return false;
    p.key = gm20b::factsCacheKey(stack, p.packKey);
    return true;
}

// One function's attributes: its AIR, constants and the name it was made under (function_attributes.h)
bool attributesKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
                   std::string &why)
{
    const gm20b::RequestStage &s = p.request.stages[0];
    gm20b::AirHash hash;
    if (!airOf(p.request, air, s, hash, why))
        return false;
    p.packKey = gm20b::functionAttributesKey(hash, s.constants, s.name);
    p.key = gm20b::functionAttributesCacheKey(stack, p.packKey);
    return true;
}

// One function's buffer encoders: its AIR, constants and whether it was made with them (buffer_encoders.h)
bool bufferEncodersKey(Prepared &p, const std::vector<std::map<std::string, gm20b::AirHash>> &air, uint64_t stack,
                       std::string &why)
{
    const gm20b::RequestStage &s = p.request.stages[0];
    gm20b::AirHash hash;
    if (!airOf(p.request, air, s, hash, why))
        return false;
    p.packKey = gm20b::bufferEncodersKey(hash, s.constants, s.specialized);
    p.key = gm20b::bufferEncodersCacheKey(stack, p.packKey);
    return true;
}

// A library the front end makes: keyed by its code generation request's bytes, which must be what a client's builder
// makes (codegen_request.h)
bool libraryKey(Prepared &p, uint64_t stack, std::string &why)
{
    const gm20b::CompileRequest &r = p.request;
    if (!gm20b::codegen::requestOk(r.codeGenType, r.codeGen))
        return why = "a library request of type " + std::to_string(r.codeGenType) + " no client's builder makes", false;
    p.key = gm20b::libraryCacheKey(stack, r.codeGenType, r.codeGen);
    if (p.key.key != r.key)
        return why = "a library request whose bytes give another key than its own", false;
    p.name = gm20b::storeName(p.key, r, {});
    return true;
}

}  // namespace

bool SystemLibraries::resolve(const std::string &root, const std::string &path, std::string &real, std::string &why)
{
    char resolved[PATH_MAX];
    if (!gm20b::systemLibraryPath(path) || !realpath((root + path).c_str(), resolved))
        return why = "not a system library: " + path, false;
    real = resolved;
    const std::string onVolume = real.compare(0, root.size(), root) ? std::string() : real.substr(root.size());
    // an OS cryptex's library resolves into its read-only volume under /private/preboot
    static const std::string cryptexes = "/private/preboot/Cryptexes/";
    if (!gm20b::systemLibraryPath(onVolume) &&
        !(onVolume.rfind(cryptexes, 0) == 0 && onVolume.find("/../") == std::string::npos))
        return why = path + " leaves the system volumes: " + real, false;
    return true;
}

bool SystemLibraries::air(const std::string &path, std::map<std::string, gm20b::AirHash> &out, std::string &why)
{
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto found = read_.find(path);
        if (found != read_.end()) {
            out = found->second;
            return true;
        }
    }
    std::string real;
    if (!resolve(root_, path, real, why))
        return false;
    if (!gm20b::metallibAirHashes(real, out))
        return why = "not a metallib: " + path, false;
    std::lock_guard<std::mutex> l(lock_);
    if (read_.size() < kSystemLibraries)
        read_[path] = out;
    return true;
}

std::string restacked(const std::string &bytes, uint64_t stack)
{
    gm20b::CompileRequest r;
    if (!gm20b::decodeRequest(bytes, r))
        return bytes;
    r.stack = stack;
    return gm20b::encodeRequest(r);
}

bool prepare(const std::string &bytes, uint64_t stack, SystemLibraries &system, Prepared &out, std::string &why)
{
    Prepared p;
    if (!gm20b::decodeRequest(bytes, p.request))
        return why = "an unreadable request", false;
    if (p.request.stack != stack)
        return why = "a request of another stack", false;
    if (p.request.kind == gm20b::RequestKind::Library) {
        if (!libraryKey(p, stack, why))
            return false;
        out = std::move(p);
        return true;
    }
    std::vector<std::map<std::string, gm20b::AirHash>> air(p.request.libraries.size());
    std::vector<gm20b::LibraryIdentity> identities;
    for (size_t i = 0; i < p.request.libraries.size(); ++i) {
        const gm20b::RequestLibrary &library = p.request.libraries[i];
        const bool read = library.system
                              ? system.air(library.path, air[i], why)
                              : gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.bytes.data()),
                                                         library.bytes.size(), air[i]);
        if (!read)
            return why = why.empty() ? "a library that is not a metallib" : why, false;
        identities.push_back(gm20b::identityOf(library));
    }
    const gm20b::RequestKind kind = p.request.kind;
    const bool keyed = gm20b::functionAttributes(p.request)  ? attributesKey(p, air, stack, why)
                       : gm20b::bufferEncoders(p.request)    ? bufferEncodersKey(p, air, stack, why)
                       : gm20b::computeReflection(p.request) ? computeReflectionKey(p, air, stack, why)
                       : kind == gm20b::RequestKind::Kernel  ? kernelKey(p, air, stack, why)
                       : kind == gm20b::RequestKind::Facts   ? factsKey(p, air, stack, why)
                                                             : renderKey(p, air, stack, why);
    if (!keyed)
        return false;
    if (p.key.key != p.request.key)
        return why = "a request whose fields give another key than its own: " + keyDifference(p.request.key, p.key.key),
               false;
    p.name = gm20b::storeName(p.key, p.request, identities);
    out = std::move(p);
    return true;
}

RefusalLine refusalLine(const gm20b::CompileRequest &r, int32_t pid, const std::string &why)
{
    const auto named = [&](size_t i) {
        const gm20b::RequestStage &s = r.stages[i];
        return s.name.empty() || s.name == s.function ? s.function : s.name + " (" + s.function + ")";
    };
    const bool render = r.kind == gm20b::RequestKind::Render ||
                        (r.kind == gm20b::RequestKind::Reflection && !gm20b::computeReflection(r));
    RefusalLine line{"state", ""};
    std::string what;
    if (r.kind == gm20b::RequestKind::Library) {
        line.reason = "library";
        what = "library of front end request type " + std::to_string(r.codeGenType);
    } else if (r.stages.empty()) {
        what = "request of no function";
    } else if (gm20b::functionAttributes(r)) {
        line.reason = "attributes";
        what = "attributes of " + named(0);
    } else if (gm20b::bufferEncoders(r)) {
        line.reason = "encoders";
        what = "buffer encoders of " + named(0);
    } else {
        line.reason = r.kind == gm20b::RequestKind::Reflection ? "reflection"
                      : r.kind == gm20b::RequestKind::Kernel   ? "kernel"
                      : r.kind == gm20b::RequestKind::Facts    ? "facts"
                                                               : "state";
        what = r.kind == gm20b::RequestKind::Reflection ? "reflection of "
               : render                                 ? "render state of "
               : r.kind == gm20b::RequestKind::Kernel   ? "kernel "
                                                        : "compute state of ";
        what += named(0);
        if (const size_t f = render ? gm20b::fragmentStage(r) : gm20b::kNoStage; f != gm20b::kNoStage)
            what += " and " + named(f);
        if (r.kind == gm20b::RequestKind::Kernel)
            what += " at " + std::to_string(r.kernel.threads[0]) + "," + std::to_string(r.kernel.threads[1]) + "," +
                    std::to_string(r.kernel.threads[2]) + " threads";
        if (!render && r.stages.size() > 1)
            what += " with " + std::to_string(r.stages.size() - 1) + " linked functions";
    }
    line.text = "pid " + std::to_string(pid) + "'s " + what + " refused: " + why;
    return line;
}

}  // namespace switch_compiler
