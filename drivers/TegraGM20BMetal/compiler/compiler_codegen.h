// SPDX-License-Identifier: MIT
// The system's Metal front end (MTLCompiler.framework's code generation service, MetalABI.h SWITCH_METAL_COMPILER) as the
// shader compiler runs a library request (compile_request.h RequestKind::Library), so no GPU client loads it or
// carries a compile's memory. It is loaded at the first such request, inside the sandbox. A reply the compiler kept, or
// one the image's seed ships, is named as m2v's frontend named it (the SHA-256 of the front end's LC_UUID, the request type
// and the bytes the front end reads) and answers without a compile; every other is made and kept, with its provenance
// (SwitchCompileReply.h), for the next build's store and for the device harvest.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "compiler_request.h"

namespace switch_compiler {

// The front end's module cache, the replies the compiler keeps, and the seed's (none: "")
struct CodeGenPaths {
    std::string modules, replies, seed;
};

class CodeGen {
public:
    explicit CodeGen(CodeGenPaths paths);
    ~CodeGen();

    // `p`'s library, or the front end's error log, as the store keeps it (library_entry.h); TegraGM20BCompilerABI.h's
    // result. No entry where the front end did not load or did not answer: a later ask may.
    int32_t make(const Prepared &p, std::string &entry, std::string &why);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace switch_compiler
