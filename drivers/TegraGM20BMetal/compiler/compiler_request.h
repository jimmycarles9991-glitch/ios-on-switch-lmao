// SPDX-License-Identifier: MIT
// A request as the compiler takes it (compile_request.h): decoded, each system library's function list read from the
// system volume itself (a path that leaves /System, through a link or otherwise, is refused), and the state's CacheKey made
// from what was read, the way the runtime keys a state or a kernel. The key the client sent is only checked against it: a
// request whose fields or libraries give another key is refused, and nothing else the client computed is kept.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../compile_request.h"
#include "../metallib.h"

namespace switch_compiler {

// The function lists of the system libraries asked for, read once per path (the system volume does not change), from the
// system volumes at `root` ("" on the device)
class SystemLibraries {
public:
    explicit SystemLibraries(std::string root = "") : root_(std::move(root))
    {}

    bool air(const std::string &path, std::map<std::string, gm20b::AirHash> &out, std::string &why);
    // Where `path` is under `root`, once it is known to stay on the system volume: what the compiler opens
    static bool resolve(const std::string &root, const std::string &path, std::string &real, std::string &why);

private:
    const std::string root_;
    std::mutex lock_;
    std::map<std::string, std::map<std::string, gm20b::AirHash>> read_;
};

struct Prepared {
    gm20b::CompileRequest request;
    gm20b::CacheKey key;
    std::string packKey;  // the key Direct makes the record under (a kernel's compute key)
    gm20b::Digest name{};
};

bool prepare(const std::string &bytes, uint64_t stack, SystemLibraries &system, Prepared &out, std::string &why);

// The request `bytes` as one of stack `stack`, which prepare then keys from its fields as this build keys a state: what
// an earlier build logged, made again under this build's key. Bytes of another layout (its magic, compile_request.cpp)
// or no request at all, as they are: prepare refuses them
std::string restacked(const std::string &bytes, uint64_t stack);

// The one error line a refused request makes (compiler_service.mm, SwitchRefusal under the translator's domain "m2v"):
// the request's kind as its reason key, and what was refused, of whose functions by the names their client made them
// under, for which process, with the compiler's reason, which the client's NSError carries too
struct RefusalLine {
    const char *reason;  // a literal: SwitchRefusal keeps the pointer
    std::string text;
};

RefusalLine refusalLine(const gm20b::CompileRequest &r, int32_t pid, const std::string &why);

}  // namespace switch_compiler
