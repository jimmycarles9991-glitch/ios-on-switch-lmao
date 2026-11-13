// SPDX-License-Identifier: MIT
// What the system shader compiler compiles with: the plugin's own m2v translator and NVK's stage compile, over the frontend's
// compile context (bridge/NativeMetalCompile.h: no m2v device, no shader cache on disk), and the installed pack's rules, so
// a record made here is the one the runtime would make in process (RawComplete.mm make, kernel).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "compiler_request.h"

namespace switch_compiler {

// Where the engine reads: the pack and NVK, and the root the system volumes are at ("" on the device)
struct EngineInputs {
    std::string pack, nvk, systemRoot;

    // The installed plugin's, beside it
    static EngineInputs installed();
};

class Engine {
public:
    Engine();
    ~Engine();

    // The pack read: what the store is named by
    bool open(uint64_t stack, const EngineInputs &inputs, std::string &why);
    // The compile context opened: what compiles. compile() opens it at the first compile that needs it, so a compiler
    // that serves its store alone has none
    bool openCompiler(std::string &why);
    // The installed pack's rules (raw_record.h PackRules), as the store is named by them
    raw::Digest rules() const;
    // `p`'s record or refusal as the store keeps it (raw_cache.h's entry bytes); TegraGM20BCompilerABI.h's result
    int32_t compile(const Prepared &p, std::string &entry, std::string &why);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace switch_compiler
