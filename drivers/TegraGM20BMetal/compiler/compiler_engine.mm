// SPDX-License-Identifier: MIT
// See compiler_engine.h.
#import <Metal/Metal.h>
#import "compiler_engine.h"

#include <dlfcn.h>
#include <os/log.h>
#include <mutex>
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BCompilerABI.h"
#include "compiler_libraries.h"
#include "compiler_record.h"
#include "../raw/runtime/RawDirect.h"
#include "../raw/runtime/raw_pack.h"

namespace switch_compiler {
namespace {

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

raw::Digest rulesDigest(const raw::PackRules &r)
{
    std::string bytes;
    const auto word = [&](uint32_t v) {
        bytes.append(reinterpret_cast<const char *>(&v), sizeof v);
    };
    word(uint32_t(r.varying.size()));
    for (uint32_t v : r.varying)
        word(v);
    for (const auto *registers : {&r.baseline, &r.context}) {
        word(uint32_t(registers->size()));
        for (const auto &[method, value] : *registers) {
            word(method);
            word(value);
        }
    }
    return raw::sha256(bytes.data(), bytes.size());
}

}  // namespace

struct Engine::State {
    std::mutex opening;
    bool opened = false, compiles = false;
    std::string openRefusal;
    EngineInputs inputs;
    uint64_t stack = 0;
    raw::Pack pack;
    raw::Digest rules{};
    std::unique_ptr<raw::Direct> direct;
    std::shared_ptr<LibraryCache> libraries;
};

Engine::Engine() : state_(std::make_unique<State>())
{}

Engine::~Engine() = default;

EngineInputs EngineInputs::installed()
{
    return {besidePlugin("TegraGM20BMetal.raw/pack.bin", true), besidePlugin("libvulkan_nouveau.dylib"), ""};
}

bool Engine::open(uint64_t stack, const EngineInputs &inputs, std::string &why)
{
    State &s = *state_;
    s.stack = stack;
    s.inputs = inputs;
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    if (!s.pack.load(inputs.pack.c_str(), stack, why))
        return why = "the raw pack: " + why, false;
    s.rules = rulesDigest(raw::PackRules::of(s.pack));
    s.libraries = std::make_shared<LibraryCache>(inputs.systemRoot);
    return true;
}

bool Engine::openCompiler(std::string &why)
{
    State &s = *state_;
    std::lock_guard<std::mutex> l(s.opening);
    if (s.opened)
        return s.compiles ? true : (why = s.openRefusal, false);
    s.opened = true;
#if SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    if (!native_bridge_metal_compile_open(s.inputs.nvk.c_str())) {
        s.openRefusal = std::string("no compile context: ") + nmb_last_error();
        return why = s.openRefusal, false;
    }
    s.direct = std::make_unique<raw::Direct>(s.pack);
    s.compiles = true;
#if SWITCH_DEBUG
    os_log(logger(), "SwitchShaderCompiler: compile context opened for its first compile in %llu ms",
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    return true;
}

raw::Digest Engine::rules() const
{
    return state_->rules;
}

int32_t Engine::compile(const Prepared &p, std::string &entry, std::string &why)
{
    if (!openCompiler(why))
        return TegraGM20BCompileUnavailable;
    State &s = *state_;
    LibraryCache &cache = *s.libraries;
    const Libraries libraries = [&cache](const raw::RequestLibrary &l, std::string &failed) {
        return cache.library(l, failed);
    };
    cache.began();
    const int32_t result = makeEntry(p, libraries, *s.direct, s.stack, entry, why);
    cache.ended();
    return result;
}

}  // namespace switch_compiler
