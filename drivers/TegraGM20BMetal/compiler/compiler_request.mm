// SPDX-License-Identifier: MIT
// See compiler_request.h.
#import "compiler_request.h"

#include <limits.h>
#include <stdlib.h>
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/raw_compute_key.h"
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
    raw::AirHash vertex, fragment;
    if (!airOf(r, air, r.stages[0], vertex, why) || !airOf(r, air, r.stages[1], fragment, why))
        return false;
    std::vector<raw::LinkedStageAir> linked;
    for (size_t i = 2; i < r.stages.size(); ++i) {
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

// The compute key's own part (its function's AIR, constants, limits and shape) is the compiler's; the bound textures',
// samplers' and memory's text past it is the client's, and the store name binds the dispatch they describe
bool kernelKey(Prepared &p, const std::vector<std::map<std::string, raw::AirHash>> &air, uint64_t stack,
               std::string &why)
{
    const raw::CompileRequest &r = p.request;
    raw::AirHash hash;
    if (!airOf(r, air, r.stages[0], hash, why))
        return false;
    const raw::KernelRequest &k = r.kernel;
    char shape[64];
    snprintf(shape, sizeof shape, " | tg=%u,%u,%u | tgmem=", k.threads[0], k.threads[1], k.threads[2]);
    const std::string own =
        kKernelPrefix + raw::computeStateKey(hash, r.stages[0].constants, k.maxThreads, k.multipleOfWidth) + shape;
    if (r.key.compare(0, own.size(), own))
        return why = "a kernel request whose key is not its function's", false;
    const std::string region = " | region=1";
    const bool regionKey =
        r.key.size() > region.size() && !r.key.compare(r.key.size() - region.size(), region.size(), region);
    if (k.regionMode != regionKey)
        return why = "a kernel request whose key names another dispatch mode", false;
    p.packKey = r.key.substr(sizeof kKernelPrefix - 1);
    p.key = raw::kernelCacheKey(stack, p.packKey);
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
    const bool kernel = p.request.kind == raw::RequestKind::Kernel;
    if (!(kernel ? kernelKey(p, air, stack, why) : renderKey(p, air, stack, why)))
        return false;
    if (p.key.key != p.request.key)
        return why = "a request whose fields give another key than its own: " + p.key.key.substr(0, 300), false;
    p.name = raw::storeName(p.key, p.request, identities);
    out = std::move(p);
    return true;
}

}  // namespace switch_compiler
