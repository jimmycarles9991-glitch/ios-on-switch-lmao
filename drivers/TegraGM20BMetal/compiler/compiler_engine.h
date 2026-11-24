// SPDX-License-Identifier: MIT
// What the system shader compiler compiles with: the translator's own libraries and functions (TegraGM20BMetalOffline.h: no
// m2v device, no shader cache on disk), the stage compile linked into the plugin (third_party/mesa/src/nouveau/
// switch_compile) and the installed pack's rules, as the pack generator makes its records.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "compiler_codegen.h"
#include "compiler_request.h"

namespace gm20b {
class Direct;
}

namespace switch_compiler {

// Where the engine reads: the pack, the root the system volumes are at ("" on the device), and where the system's
// Metal front end keeps its module cache and replies and finds the seed's
struct EngineInputs {
    std::string pack, systemRoot;
    CodeGenPaths codeGen;

    // The installed plugin's, beside it, with the seed's replies: the service names where the front end writes
    static EngineInputs installed();
};

class Engine {
public:
    Engine();
    ~Engine();

    // The pack read: what the store is named by
    bool open(uint64_t stack, const EngineInputs &inputs, std::string &why);
    // The installed pack's rules (record.h PackRules), as the store is named by them
    gm20b::Digest rules() const;
    // `p`'s record or refusal as the store keeps it (gm20b_cache.h's entry bytes); TegraGM20BCompilerABI.h's result
    int32_t compile(const Prepared &p, std::string &entry, std::string &why);

private:
    // What compiles, made at the first compile that needs it, so a compiler that serves its store alone has none. A
    // compile holds its own from take to give back: Direct::make keeps the state it makes in the object (its storage
    // refusal, which make reads back to choose the barrier form, its split, its stride twin), so the pool's workers
    // never share one; an idle one is taken before another is made
    std::unique_ptr<gm20b::Direct> takeCompiler();
    void giveBack(std::unique_ptr<gm20b::Direct> compiler);

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace switch_compiler
