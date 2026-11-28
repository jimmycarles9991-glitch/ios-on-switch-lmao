// SPDX-License-Identifier: MIT
// See compiler_engine.h.
#import <Metal/Metal.h>
#import "compiler_engine.h"

#include <dlfcn.h>
#include <os/log.h>
#include <mutex>
#include <vector>
#include "TegraGM20BCompilerABI.h"
#include "compiler_facts.h"
#include "compiler_libraries.h"
#include "compiler_record.h"
#include "compiler_stages.h"
#include "compiler_times.h"
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalOffline.h"
#include "../pack.h"
#include "../stats.h"

namespace switch_compiler {
namespace {

// What a compiler keeps of its compiles beside its store, for the states that share stages: m2v's translations of each
// kind (portable and native) and the stages NIR and NAK compiled. Both are bounds of a few MB under the daemon's active
// limit (SwitchShaderCompilerJetsam.h), past which the least recently used go.
constexpr size_t kKeptTranslationBytes = 8u << 20, kKeptStageBytes = 8u << 20;

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "compiler");
    return l;
}

// <Extensions>/TegraGM20BMetal.bundle/<name>, and with `up` <Extensions>/<name>: what the runtime opens beside the plugin
std::string besidePlugin(const char *name, bool up = false)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&logger), &info) || !info.dli_fname)
        return std::string();
    std::string path = info.dli_fname;
    for (int i = 0; i < (up ? 2 : 1); ++i)
        path.erase(path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    return path + "/" + name;
}

}  // namespace

struct Engine::State {
    // The idle compilers: taken and given back around a compile, never held across one
    std::mutex idle;
    std::vector<std::unique_ptr<gm20b::Direct>> compilers;
    EngineInputs inputs;
    uint64_t stack = 0;
    gm20b::Pack pack;
    gm20b::Digest rules{};
    std::shared_ptr<LibraryCache> libraries;
    std::unique_ptr<CodeGen> codeGen;
    std::unique_ptr<CompiledStages> stages;
};

Engine::Engine() : state_(std::make_unique<State>())
{}

Engine::~Engine() = default;

EngineInputs EngineInputs::installed()
{
    EngineInputs inputs = {besidePlugin("TegraGM20BMetal.pack/pack.bin", true), "", {}};
    inputs.codeGen.seed = besidePlugin("TegraGM20BMetal.seed/shared/msl", true);
    return inputs;
}

bool Engine::open(uint64_t stack, const EngineInputs &inputs, std::string &why)
{
    State &s = *state_;
    s.stack = stack;
    s.inputs = inputs;
    if (!s.pack.load(inputs.pack.c_str(), stack, why))
        return why = "the pack: " + why, false;
    memcpy(s.rules.data(), s.pack.header()->rules, sizeof s.pack.header()->rules);
    s.libraries = std::make_shared<LibraryCache>(inputs.systemRoot);
    s.codeGen = std::make_unique<CodeGen>(inputs.codeGen);
    s.stages = std::make_unique<CompiledStages>(kKeptStageBytes);
    nmb_keep_translations(kKeptTranslationBytes);
    return true;
}

std::unique_ptr<gm20b::Direct> Engine::takeCompiler()
{
    State &s = *state_;
    {
        std::lock_guard<std::mutex> l(s.idle);
        if (!s.compilers.empty()) {
            std::unique_ptr<gm20b::Direct> compiler = std::move(s.compilers.back());
            s.compilers.pop_back();
            return compiler;
        }
    }
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    auto compiler = std::make_unique<gm20b::Direct>(gm20b::PackRules::of(s.pack), false, offline::translator());
    compiler->keepStages(s.stages.get());
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    os_log(logger(), "SwitchShaderCompiler: a compiler made for a compile no idle one could take in %llu ms",
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    return compiler;
}

void Engine::giveBack(std::unique_ptr<gm20b::Direct> compiler)
{
    State &s = *state_;
    std::lock_guard<std::mutex> l(s.idle);
    s.compilers.push_back(std::move(compiler));
}

gm20b::Digest Engine::rules() const
{
    return state_->rules;
}

int32_t Engine::compile(const Prepared &p, std::string &entry, std::string &why)
{
    State &s = *state_;
    if (p.request.kind == gm20b::RequestKind::Library)
        return s.codeGen->make(p, entry, why);
    LibraryCache &cache = *s.libraries;
    const Libraries libraries = [&cache, &p](const gm20b::RequestLibrary &l, std::string &failed) {
        std::shared_ptr<const MappedClientLibrary> mapped;
        if (l.libraryId && !p.request.libraries.empty()) {
            const ptrdiff_t idx = &l - p.request.libraries.data();
            if (idx >= 0 && size_t(idx) < p.clientLibraries.size())
                mapped = p.clientLibraries[size_t(idx)];
        }
#if SWITCH_DEBUG
        const uint64_t began = uptimeNs();
        id<MTLLibrary> library = cache.library(l, failed, mapped);
        tJobSplit.libraryNs += uptimeNs() - began;
        return library;
#else
        return cache.library(l, failed, mapped);
#endif
    };
#if SWITCH_DEBUG
    uint64_t before[2] = {}, after[2] = {};
    nmb_kept_translations(before);
    const uint64_t began = uptimeNs();
#endif
    const auto record = [&] {
        std::unique_ptr<gm20b::Direct> compiler = takeCompiler();
        const int32_t made = makeEntry(p, libraries, *compiler, s.stack, entry, why);
        giveBack(std::move(compiler));
        return made;
    };
    cache.began();
    const int32_t result = p.request.kind == gm20b::RequestKind::Facts ? makeFacts(p, libraries, entry, why)
                           : gm20b::functionAttributes(p.request)      ? makeAttributes(p, libraries, entry, why)
                           : gm20b::bufferEncoders(p.request)          ? makeBufferEncoders(p, libraries, entry, why)
                           : p.request.kind == gm20b::RequestKind::Reflection
                               ? makeReflectionEntry(p, libraries, s.stack, entry, why)
                               : record();
    cache.ended();
#if SWITCH_DEBUG
    tJobSplit.makeNs = uptimeNs() - began;
    nmb_kept_translations(after);
    tJobSplit.translationsKept = int64_t(after[0]) - int64_t(before[0]);
#endif
    return result;
}

}  // namespace switch_compiler
