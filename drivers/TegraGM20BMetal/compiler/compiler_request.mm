// SPDX-License-Identifier: MIT
// See compiler_request.h.
#import "compiler_request.h"

#include <limits.h>
#include <stdlib.h>
#include "compiler_key_difference.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/raw_buffer_encoders.h"
#include "../raw/runtime/raw_codegen_request.h"
#include "../raw/runtime/raw_compute_key.h"
#include "../raw/runtime/raw_function_attributes.h"
#include "../raw/runtime/raw_render_key.h"

namespace switch_compiler {
namespace {

constexpr size_t kSystemLibraries = 256;
constexpr char kKernelPrefix[] = "kernel | ";

bool airOf(const raw::CompileRequest &r, const std::vector<std::map<std::string, raw::AirHash>> &air,
           const raw::RequestStage &s, raw::AirHash &hash, std::string &why)
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

bool renderKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
               std::string &why)
{
    const raw::CompileRequest &r = p.request;
    raw::AirHash vertex, fragment{};
    const size_t f = raw::fragmentStage(r);
    if (!airOf(r, air, r.stages[0], vertex, why) || (f != raw::kNoStage && !airOf(r, air, r.stages[f], fragment, why)))
        return false;
    std::vector<raw::LinkedStageAir> linked;
    for (size_t i = raw::firstLinked(r); i < r.stages.size(); ++i) {
        raw::LinkedStageAir stage{r.stages[i].tag[0], {}};
        if (!airOf(r, air, r.stages[i], stage.air, why))
            return false;
        linked.push_back(stage);
    }
    bool own = false;
    @autoreleasepool {
        p.packKey = raw::requestPackKey(r, own);
    }
    p.key = raw::renderCacheKey(stack, p.packKey, vertex, fragment, linked, own);
    return true;
}

// A compute request's state key (raw_compute_key.h computeStateKey): its kernel's AIR and constants, its stage input, then
// its linked stages' (computeLinkedText), and its limits
bool stateKeyOf(const raw::CompileRequest &r, const std::vector<std::map<std::string, raw::AirHash>> &air,
                std::string &key, std::string &why)
{
    raw::AirHash hash;
    if (!airOf(r, air, r.stages[0], hash, why))
        return false;
    std::vector<raw::LinkedComputeFunction> linked;
    for (size_t i = 1; i < r.stages.size(); ++i) {
        raw::LinkedComputeFunction f;
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
        r.stages[0].constants + raw::stageInText(r.kernel.stageIn) + raw::computeLinkedText(linked);
    key = raw::computeStateKey(hash, constants, r.kernel.maxThreads, r.kernel.multipleOfWidth);
    return true;
}

// The compute key's own part (its function's AIR, constants, stage input, linked functions, limits and shape) is the
// compiler's; the bound textures', samplers' and memory's text past it is the client's, and the store name binds the
// dispatch they describe
bool kernelKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
               std::string &why)
{
    const raw::CompileRequest &r = p.request;
    std::string state;
    if (!stateKeyOf(r, air, state, why))
        return false;
    const raw::KernelRequest &k = r.kernel;
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
    p.key = raw::kernelCacheKey(stack, p.packKey);
    return true;
}

// A compute state's reflection is keyed by its function's AIR, constants and limits alone (raw_compute_key.h
// computeStateKey), as the pack keys it
bool computeReflectionKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
                          std::string &why)
{
    if (!stateKeyOf(p.request, air, p.packKey, why))
        return false;
    p.key = raw::computeReflectionKey(stack, p.packKey);
    return true;
}

// A compute state's facts: its function's AIR, constants and limits, as the pack keys its PackComputeFacts
bool factsKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
              std::string &why)
{
    if (!stateKeyOf(p.request, air, p.packKey, why))
        return false;
    p.key = raw::factsCacheKey(stack, p.packKey);
    return true;
}

// One function's attributes: its AIR, constants and the name it was made under (raw_function_attributes.h)
bool attributesKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
                   std::string &why)
{
    const raw::RequestStage &s = p.request.stages[0];
    raw::AirHash hash;
    if (!airOf(p.request, air, s, hash, why))
        return false;
    p.packKey = raw::functionAttributesKey(hash, s.constants, s.name);
    p.key = raw::functionAttributesCacheKey(stack, p.packKey);
    return true;
}

// One function's buffer encoders: its AIR, constants and whether it was made with them (raw_buffer_encoders.h)
bool bufferEncodersKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
                       std::string &why)
{
    const raw::RequestStage &s = p.request.stages[0];
    raw::AirHash hash;
    if (!airOf(p.request, air, s, hash, why))
        return false;
    p.packKey = raw::bufferEncodersKey(hash, s.constants, s.specialized);
    p.key = raw::bufferEncodersCacheKey(stack, p.packKey);
    return true;
}

// A library the front end makes: keyed by its code generation request's bytes, which must be what a client's builder
// makes (raw_codegen_request.h)
bool libraryKey(Prepared &p, uint64_t stack, std::string &why)
{
    const raw::CompileRequest &r = p.request;
    if (!raw::codegen::requestOk(r.codeGenType, r.codeGen))
        return why = "a library request of type " + std::to_string(r.codeGenType) + " no client's builder makes", false;
    p.key = raw::libraryCacheKey(stack, r.codeGenType, r.codeGen);
    if (p.key.key != r.key)
        return why = "a library request whose bytes give another key than its own", false;
    p.name = raw::storeName(p.key, r, {});
    return true;
}

}  // namespace

bool SystemLibraries::resolve(const std::string &root, const std::string &path, std::string &real, std::string &why)
{
    char resolved[PATH_MAX];
    if (!raw::systemLibraryPath(path) || !realpath((root + path).c_str(), resolved))
        return why = "not a system library: " + path, false;
    real = resolved;
    const std::string onVolume = real.compare(0, root.size(), root) ? std::string() : real.substr(root.size());
    // an OS cryptex's library resolves into its read-only volume under /private/preboot
    static const std::string cryptexes = "/private/preboot/Cryptexes/";
    if (!raw::systemLibraryPath(onVolume) &&
        !(onVolume.rfind(cryptexes, 0) == 0 && onVolume.find("/../") == std::string::npos))
        return why = path + " leaves the system volumes: " + real, false;
    return true;
}

bool SystemLibraries::air(const std::string &path, std::map<std::string, raw::AirHash> &out, std::string &why)
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
    if (!raw::metallibAirHashes(real, out))
        return why = "not a metallib: " + path, false;
    std::lock_guard<std::mutex> l(lock_);
    if (read_.size() < kSystemLibraries)
        read_[path] = out;
    return true;
}

bool prepare(const std::string &bytes, uint64_t stack, SystemLibraries &system, Prepared &out, std::string &why)
{
    Prepared p;
    if (!raw::decodeRequest(bytes, p.request))
        return why = "an unreadable request", false;
    if (p.request.stack != stack)
        return why = "a request of another stack", false;
    if (p.request.kind == raw::RequestKind::Library) {
        if (!libraryKey(p, stack, why))
            return false;
        out = std::move(p);
        return true;
    }
    std::vector<std::map<std::string, raw::AirHash>> air(p.request.libraries.size());
    std::vector<raw::LibraryIdentity> identities;
    for (size_t i = 0; i < p.request.libraries.size(); ++i) {
        const raw::RequestLibrary &library = p.request.libraries[i];
        const bool read = library.system
                              ? system.air(library.path, air[i], why)
                              : raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.bytes.data()),
                                                       library.bytes.size(), air[i]);
        if (!read)
            return why = why.empty() ? "a library that is not a metallib" : why, false;
        identities.push_back(raw::identityOf(library));
    }
    const raw::RequestKind kind = p.request.kind;
    const bool keyed = raw::functionAttributes(p.request)  ? attributesKey(p, air, stack, why)
                       : raw::bufferEncoders(p.request)    ? bufferEncodersKey(p, air, stack, why)
                       : raw::computeReflection(p.request) ? computeReflectionKey(p, air, stack, why)
                       : kind == raw::RequestKind::Kernel  ? kernelKey(p, air, stack, why)
                       : kind == raw::RequestKind::Facts   ? factsKey(p, air, stack, why)
                                                           : renderKey(p, air, stack, why);
    if (!keyed)
        return false;
    if (p.key.key != p.request.key)
        return why = "a request whose fields give another key than its own: " + keyDifference(p.request.key, p.key.key),
               false;
    p.name = raw::storeName(p.key, p.request, identities);
    out = std::move(p);
    return true;
}

}  // namespace switch_compiler
